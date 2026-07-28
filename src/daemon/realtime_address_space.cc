#include "daemon/realtime_address_space.h"

#include <algorithm>
#include <exception>
#include <utility>
#include <variant>

namespace opcua {

namespace {

constexpr UA_UInt16 kRealtimeNamespace = 2;
constexpr char kRealtimeNamespaceUri[] = "urn:opcua:realtime";
constexpr char kDiagnosticsObjectId[] = "MqttSource";
constexpr char kDiagnosticNodeIds[][48] = {
    "MqttSource.ConnectionState",
    "MqttSource.LastSuccessfulUpdate",
    "MqttSource.ConsecutiveFailures",
};

bool RemovalConfirmed(UA_StatusCode code) {
  return code == UA_STATUSCODE_GOOD ||
         code == UA_STATUSCODE_BADNODEIDUNKNOWN;
}

Status Open62541Error(const char* operation, UA_StatusCode code) {
  return Status::Error(std::string(operation) + " failed: " +
                       UA_StatusCode_name(code));
}

const UA_DataType* UaType(ScalarType type) {
  switch (type) {
    case ScalarType::kBoolean:
      return &UA_TYPES[UA_TYPES_BOOLEAN];
    case ScalarType::kInt64:
      return &UA_TYPES[UA_TYPES_INT64];
    case ScalarType::kDouble:
      return &UA_TYPES[UA_TYPES_DOUBLE];
  }
  return nullptr;
}

UA_NodeId DataTypeNodeId(const UA_DataType* type) {
  return type == nullptr ? UA_NODEID_NULL : type->typeId;
}

UA_StatusCode SetReadStatus(UA_DataValue* value, UA_StatusCode status) {
  value->hasStatus = true;
  value->status = status;
  return UA_STATUSCODE_GOOD;
}

}  // namespace

struct RealtimeAddressSpace::NodeContext {
  enum class Kind {
    kData,
    kConnectionState,
    kLastSuccessfulUpdate,
    kConsecutiveFailures,
  };

  RealtimeValueStore* store;
  UA_Server* server;
  Kind kind;
  ScalarType type;
  ValueSlotId slot_id;
  bool adding;
  std::uint32_t node_id;
};

RealtimeAddressSpace::RealtimeAddressSpace(RealtimeValueStore* store)
    : store_(store) {}

RealtimeAddressSpace::~RealtimeAddressSpace() noexcept {
  auto remove_if_owned = [](UA_Server* server, UA_NodeId node_id,
                            const NodeContext* expected_context) {
    void* registered_context = nullptr;
    const UA_StatusCode context_status =
        UA_Server_getNodeContext(server, node_id, &registered_context);
    if (context_status == UA_STATUSCODE_BADNODEIDUNKNOWN ||
        (context_status == UA_STATUSCODE_GOOD &&
         registered_context != expected_context)) {
      return true;
    }
    if (context_status != UA_STATUSCODE_GOOD) {
      return false;
    }
    return RemovalConfirmed(UA_Server_deleteNode(server, node_id, true));
  };

  for (auto context = contexts_.rbegin(); context != contexts_.rend();
       ++context) {
    if (*context == nullptr) {
      continue;
    }
    if ((*context)->server == nullptr) {
      context->reset();
      continue;
    }

    UA_NodeId node_id = UA_NODEID_NULL;
    switch ((*context)->kind) {
      case NodeContext::Kind::kData:
        node_id = UA_NODEID_NUMERIC(kRealtimeNamespace, (*context)->node_id);
        break;
      case NodeContext::Kind::kConnectionState:
        node_id = UA_NODEID_STRING(
            kRealtimeNamespace,
            const_cast<char*>(kDiagnosticNodeIds[0]));
        break;
      case NodeContext::Kind::kLastSuccessfulUpdate:
        node_id = UA_NODEID_STRING(
            kRealtimeNamespace,
            const_cast<char*>(kDiagnosticNodeIds[1]));
        break;
      case NodeContext::Kind::kConsecutiveFailures:
        node_id = UA_NODEID_STRING(
            kRealtimeNamespace,
            const_cast<char*>(kDiagnosticNodeIds[2]));
        break;
    }

    bool context_can_be_freed =
        remove_if_owned((*context)->server, node_id, context->get());
    if ((*context)->kind == NodeContext::Kind::kConnectionState) {
      context_can_be_freed =
          remove_if_owned(
              (*context)->server,
              UA_NODEID_STRING(kRealtimeNamespace,
                               const_cast<char*>(kDiagnosticsObjectId)),
              context->get()) &&
          context_can_be_freed;
    }
    if (!context_can_be_freed) {
      context->release();
      continue;
    }
    context->reset();
  }
}

void RealtimeAddressSpace::DetachServer(UA_Server* server) noexcept {
  for (auto& context : contexts_) {
    if (context != nullptr && context->server == server) {
      context->server = nullptr;
    }
  }
}

Status RealtimeAddressSpace::AddNode(UA_Server* server,
                                     const RealtimeNodeConfig& config) {
  if (server == nullptr) {
    return Status::Error("server must not be null");
  }
  if (store_ == nullptr) {
    return Status::Error("realtime value store must not be null");
  }
  if (config.node_id == 0) {
    return Status::Error("realtime node ID must be greater than zero");
  }
  if (config.browse_name.empty()) {
    return Status::Error("realtime node browse name must not be empty");
  }
  if (config.browse_name.find('\0') != std::string::npos) {
    return Status::Error("realtime node browse name must not contain NUL");
  }

  const auto snapshot = store_->ReadSnapshot(config.slot_id);
  if (!snapshot.ok()) {
    return Status::Error("realtime node slot does not exist: " +
                         snapshot.status().message());
  }
  if (snapshot.value().type != config.type) {
    return Status::Error("realtime node type does not match slot type");
  }

  const UA_NodeId diagnostics_id = UA_NODEID_STRING(
      kRealtimeNamespace, const_cast<char*>(kDiagnosticsObjectId));
  std::unique_ptr<NodeContext> state_context;
  std::unique_ptr<NodeContext> update_context;
  std::unique_ptr<NodeContext> failures_context;
  bool diagnostics_object_created = false;
  bool diagnostic_variables_created[3] = {false, false, false};
  auto rollback_diagnostics = [&]() {
    std::unique_ptr<NodeContext>* diagnostic_contexts[] = {
        &state_context, &update_context, &failures_context};
    for (std::size_t i = 3; i > 0; --i) {
      const std::size_t index = i - 1;
      if (!diagnostic_variables_created[index]) {
        continue;
      }
      const UA_StatusCode delete_status = UA_Server_deleteNode(
          server, UA_NODEID_STRING(
                      kRealtimeNamespace,
                      const_cast<char*>(kDiagnosticNodeIds[index])),
          true);
      if (!RemovalConfirmed(delete_status) &&
          *diagnostic_contexts[index] != nullptr) {
        diagnostic_contexts[index]->release();
      }
      if (RemovalConfirmed(delete_status)) {
        diagnostic_variables_created[index] = false;
      }
    }
    if (diagnostics_object_created) {
      const UA_StatusCode delete_status =
          UA_Server_deleteNode(server, diagnostics_id, true);
      if (RemovalConfirmed(delete_status)) {
        diagnostics_object_created = false;
      } else if (state_context != nullptr) {
        state_context.release();
      }
    }
  };

  try {
    UA_String namespace_uri;
    UA_String_init(&namespace_uri);
    const UA_StatusCode namespace_status = UA_Server_getNamespaceByIndex(
        server, kRealtimeNamespace, &namespace_uri);
    if (namespace_status == UA_STATUSCODE_GOOD) {
      const UA_String expected_uri =
          UA_STRING(const_cast<char*>(kRealtimeNamespaceUri));
      const bool namespace_matches =
          UA_String_equal(&namespace_uri, &expected_uri);
      UA_String_clear(&namespace_uri);
      if (!namespace_matches) {
        return Status::Error(
            "namespace index 2 is not urn:opcua:realtime");
      }
    } else if (namespace_status == UA_STATUSCODE_BADNOTFOUND) {
      UA_String_clear(&namespace_uri);
      const UA_UInt16 namespace_index =
          UA_Server_addNamespace(server, kRealtimeNamespaceUri);
      if (namespace_index != kRealtimeNamespace) {
        return Status::Error("unable to register realtime namespace at index 2");
      }
    } else {
      UA_String_clear(&namespace_uri);
      return Open62541Error("read namespace index 2", namespace_status);
    }

    const bool diagnostics_exist =
        std::any_of(contexts_.begin(), contexts_.end(),
                    [server](const std::unique_ptr<NodeContext>& context) {
                      return context->server == server &&
                             context->kind != NodeContext::Kind::kData;
                    });

    if (!diagnostics_exist) {
      contexts_.reserve(contexts_.size() + 4);
      state_context = std::make_unique<NodeContext>(NodeContext{
          store_, server, NodeContext::Kind::kConnectionState,
          ScalarType::kBoolean, 0, false, 0});
      update_context = std::make_unique<NodeContext>(NodeContext{
          store_, server, NodeContext::Kind::kLastSuccessfulUpdate,
          ScalarType::kBoolean, 0, false, 0});
      failures_context = std::make_unique<NodeContext>(NodeContext{
          store_, server, NodeContext::Kind::kConsecutiveFailures,
          ScalarType::kBoolean, 0, false, 0});

      UA_ObjectAttributes object_attributes = UA_ObjectAttributes_default;
      object_attributes.displayName = UA_LOCALIZEDTEXT(
          const_cast<char*>("en-US"), const_cast<char*>("MqttSource"));
      UA_StatusCode code = UA_Server_addObjectNode(
          server, diagnostics_id, UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
          UA_QUALIFIEDNAME(kRealtimeNamespace,
                           const_cast<char*>("MqttSource")),
          UA_NS0ID(FOLDERTYPE), object_attributes, state_context.get(),
          nullptr);
      if (UA_StatusCode_isBad(code)) {
        return Open62541Error("add MqttSource diagnostics object", code);
      }
      diagnostics_object_created = true;

      struct DiagnosticDefinition {
        const char* node_id;
        const char* browse_name;
        const UA_DataType* type;
        NodeContext* context;
      };
      const DiagnosticDefinition definitions[] = {
          {"MqttSource.ConnectionState", "ConnectionState",
           &UA_TYPES[UA_TYPES_STRING], state_context.get()},
          {"MqttSource.LastSuccessfulUpdate", "LastSuccessfulUpdate",
           &UA_TYPES[UA_TYPES_DATETIME], update_context.get()},
          {"MqttSource.ConsecutiveFailures", "ConsecutiveFailures",
           &UA_TYPES[UA_TYPES_UINT32], failures_context.get()},
      };

      for (std::size_t i = 0; i < 3; ++i) {
        const auto& definition = definitions[i];
        UA_VariableAttributes attributes = UA_VariableAttributes_default;
        attributes.displayName = UA_LOCALIZEDTEXT(
            const_cast<char*>("en-US"),
            const_cast<char*>(definition.browse_name));
        attributes.dataType = DataTypeNodeId(definition.type);
        attributes.valueRank = UA_VALUERANK_SCALAR;
        attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
        attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
        const UA_CallbackValueSource value_source{Read, nullptr};
        code = UA_Server_addCallbackValueSourceVariableNode(
            server,
            UA_NODEID_STRING(kRealtimeNamespace,
                             const_cast<char*>(definition.node_id)),
            diagnostics_id, UA_NS0ID(HASCOMPONENT),
            UA_QUALIFIEDNAME(kRealtimeNamespace,
                             const_cast<char*>(definition.browse_name)),
            UA_NS0ID(BASEDATAVARIABLETYPE), attributes, value_source,
            definition.context, nullptr);
        if (UA_StatusCode_isBad(code)) {
          rollback_diagnostics();
          return Open62541Error("add MqttSource diagnostics variable", code);
        }
        diagnostic_variables_created[i] = true;
      }
    } else {
      contexts_.reserve(contexts_.size() + 1);
    }

    auto context = std::make_unique<NodeContext>(NodeContext{
        store_, server, NodeContext::Kind::kData, config.type, config.slot_id,
        true, config.node_id});
    const UA_DataType* type = UaType(config.type);
    if (type == nullptr) {
      rollback_diagnostics();
      return Status::Error("realtime node scalar type is invalid");
    }

    UA_VariableAttributes attributes = UA_VariableAttributes_default;
    attributes.displayName = UA_LOCALIZEDTEXT(
        const_cast<char*>("en-US"),
        const_cast<char*>(config.browse_name.c_str()));
    attributes.dataType = DataTypeNodeId(type);
    attributes.valueRank = UA_VALUERANK_SCALAR;
    attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
    attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
    const UA_CallbackValueSource value_source{Read, nullptr};
    const UA_StatusCode code = UA_Server_addCallbackValueSourceVariableNode(
        server, UA_NODEID_NUMERIC(kRealtimeNamespace, config.node_id),
        UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
        UA_QUALIFIEDNAME(kRealtimeNamespace,
                         const_cast<char*>(config.browse_name.c_str())),
        UA_NS0ID(BASEDATAVARIABLETYPE), attributes, value_source, context.get(),
        nullptr);
    if (UA_StatusCode_isBad(code)) {
      rollback_diagnostics();
      return Open62541Error("add realtime variable", code);
    }

    context->adding = false;
    if (diagnostics_object_created) {
      contexts_.push_back(std::move(state_context));
      contexts_.push_back(std::move(update_context));
      contexts_.push_back(std::move(failures_context));
      diagnostics_object_created = false;
      for (bool& created : diagnostic_variables_created) {
        created = false;
      }
    }
    contexts_.push_back(std::move(context));
    return Status::Ok();
  } catch (const std::exception& error) {
    rollback_diagnostics();
    return Status::Error(std::string("add realtime node failed: ") +
                         error.what());
  } catch (...) {
    rollback_diagnostics();
    return Status::Error("add realtime node failed with unknown exception");
  }
}

UA_StatusCode RealtimeAddressSpace::Read(
    UA_Server* server, const UA_NodeId* session_id, void* session_context,
    const UA_NodeId* node_id, void* node_context,
    UA_Boolean include_source_timestamp, const UA_NumericRange* range,
    UA_DataValue* value) {
  (void)server;
  (void)session_id;
  (void)session_context;
  (void)node_id;

  if (value == nullptr) {
    return UA_STATUSCODE_BADINTERNALERROR;
  }
  if (node_context == nullptr) {
    return SetReadStatus(value, UA_STATUSCODE_BADINTERNALERROR);
  }
  auto* context = static_cast<NodeContext*>(node_context);
  if (context->store == nullptr) {
    return SetReadStatus(value, UA_STATUSCODE_BADINTERNALERROR);
  }
  if (range != nullptr) {
    return SetReadStatus(value, UA_STATUSCODE_BADINDEXRANGEINVALID);
  }

  try {
    value->hasStatus = true;
    value->status = UA_STATUSCODE_GOOD;

    if (context->kind != NodeContext::Kind::kData) {
      const SourceHealthSnapshot health = context->store->ReadSourceHealth();
      UA_StatusCode code = UA_STATUSCODE_BADINTERNALERROR;
      switch (context->kind) {
        case NodeContext::Kind::kConnectionState: {
          const char* state_text = nullptr;
          switch (health.connection_state) {
            case SourceConnectionState::kDisabled:
              state_text = "disabled";
              break;
            case SourceConnectionState::kConnecting:
              state_text = "connecting";
              break;
            case SourceConnectionState::kConnected:
              state_text = "connected";
              break;
            case SourceConnectionState::kDisconnected:
              state_text = "disconnected";
              break;
          }
          if (state_text == nullptr) {
            return SetReadStatus(value, UA_STATUSCODE_BADINTERNALERROR);
          }
          const UA_String state =
              UA_STRING(const_cast<char*>(state_text));
          code = UA_Variant_setScalarCopy(&value->value, &state,
                                          &UA_TYPES[UA_TYPES_STRING]);
          break;
        }
        case NodeContext::Kind::kLastSuccessfulUpdate:
          code = UA_Variant_setScalarCopy(
              &value->value, &health.last_successful_update,
              &UA_TYPES[UA_TYPES_DATETIME]);
          break;
        case NodeContext::Kind::kConsecutiveFailures:
          code = UA_Variant_setScalarCopy(
              &value->value, &health.consecutive_failures,
              &UA_TYPES[UA_TYPES_UINT32]);
          break;
        case NodeContext::Kind::kData:
          return SetReadStatus(value, UA_STATUSCODE_BADINTERNALERROR);
      }
      if (UA_StatusCode_isBad(code)) {
        return SetReadStatus(value, code);
      }
      value->hasValue = true;
      return UA_STATUSCODE_GOOD;
    }

    if (context->adding) {
      UA_StatusCode code = UA_STATUSCODE_BADTYPEMISMATCH;
      switch (context->type) {
        case ScalarType::kBoolean: {
          const UA_Boolean scalar = false;
          code = UA_Variant_setScalarCopy(
              &value->value, &scalar, &UA_TYPES[UA_TYPES_BOOLEAN]);
          break;
        }
        case ScalarType::kInt64: {
          const UA_Int64 scalar = 0;
          code = UA_Variant_setScalarCopy(
              &value->value, &scalar, &UA_TYPES[UA_TYPES_INT64]);
          break;
        }
        case ScalarType::kDouble: {
          const UA_Double scalar = 0.0;
          code = UA_Variant_setScalarCopy(
              &value->value, &scalar, &UA_TYPES[UA_TYPES_DOUBLE]);
          break;
        }
      }
      if (UA_StatusCode_isBad(code)) {
        return SetReadStatus(value, code);
      }
      value->hasValue = true;
      return UA_STATUSCODE_GOOD;
    }

    const auto snapshot = context->store->ReadSnapshot(context->slot_id);
    if (!snapshot.ok()) {
      return SetReadStatus(value, UA_STATUSCODE_BADNODEIDUNKNOWN);
    }
    if (snapshot.value().type != context->type) {
      return SetReadStatus(value, UA_STATUSCODE_BADTYPEMISMATCH);
    }

    value->status = snapshot.value().status;
    if (!snapshot.value().has_value) {
      return UA_STATUSCODE_GOOD;
    }

    UA_StatusCode code = UA_STATUSCODE_BADTYPEMISMATCH;
    switch (snapshot.value().type) {
      case ScalarType::kBoolean:
        if (const auto* scalar =
                std::get_if<bool>(&snapshot.value().value)) {
          const UA_Boolean ua_scalar = *scalar;
          code = UA_Variant_setScalarCopy(
              &value->value, &ua_scalar, &UA_TYPES[UA_TYPES_BOOLEAN]);
        }
        break;
      case ScalarType::kInt64:
        if (const auto* scalar =
                std::get_if<std::int64_t>(&snapshot.value().value)) {
          const UA_Int64 ua_scalar = static_cast<UA_Int64>(*scalar);
          code = UA_Variant_setScalarCopy(
              &value->value, &ua_scalar, &UA_TYPES[UA_TYPES_INT64]);
        }
        break;
      case ScalarType::kDouble:
        if (const auto* scalar =
                std::get_if<double>(&snapshot.value().value)) {
          const UA_Double ua_scalar = *scalar;
          code = UA_Variant_setScalarCopy(
              &value->value, &ua_scalar, &UA_TYPES[UA_TYPES_DOUBLE]);
        }
        break;
    }
    if (UA_StatusCode_isBad(code)) {
      return SetReadStatus(value, code);
    }

    value->hasValue = true;
    if (include_source_timestamp && snapshot.value().source_timestamp != 0) {
      value->sourceTimestamp = snapshot.value().source_timestamp;
      value->hasSourceTimestamp = true;
    }
    return UA_STATUSCODE_GOOD;
  } catch (...) {
    return SetReadStatus(value, UA_STATUSCODE_BADUNEXPECTEDERROR);
  }
}

}  // namespace opcua
