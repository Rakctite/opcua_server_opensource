#include "daemon/realtime_address_space.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <variant>

namespace {

struct ServerDeleter {
  void operator()(UA_Server* server) const {
    if (server != nullptr) {
      UA_Server_delete(server);
    }
  }
};

using ServerPtr = std::unique_ptr<UA_Server, ServerDeleter>;

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

ServerPtr MakeServer() { return ServerPtr(UA_Server_new()); }

UA_NodeId NumericNodeId(std::uint32_t id) {
  return UA_NODEID_NUMERIC(2, static_cast<UA_UInt32>(id));
}

UA_NodeId StringNodeId(const char* id) {
  return UA_NODEID_STRING(2, const_cast<char*>(id));
}

UA_DataValue Read(UA_Server* server, UA_NodeId node_id,
                  UA_TimestampsToReturn timestamps =
                      UA_TIMESTAMPSTORETURN_SOURCE,
                  const char* index_range = nullptr) {
  UA_ReadValueId item;
  UA_ReadValueId_init(&item);
  item.nodeId = node_id;
  item.attributeId = UA_ATTRIBUTEID_VALUE;
  if (index_range != nullptr) {
    item.indexRange = UA_STRING(const_cast<char*>(index_range));
  }
  return UA_Server_read(server, &item, timestamps);
}

template <typename T>
bool HasScalar(const UA_DataValue& value, const UA_DataType* type,
               const T& expected) {
  return value.hasValue && UA_Variant_hasScalarType(&value.value, type) &&
         *static_cast<const T*>(value.value.data) == expected;
}

bool HasString(const UA_DataValue& value, const UA_String& expected) {
  return value.hasValue &&
         UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_STRING]) &&
         UA_String_equal(static_cast<const UA_String*>(value.value.data),
                         &expected);
}

bool NodeIsAbsent(UA_Server* server, UA_NodeId node_id) {
  UA_NodeClass node_class = UA_NODECLASS_UNSPECIFIED;
  return UA_Server_readNodeClass(server, node_id, &node_class) ==
         UA_STATUSCODE_BADNODEIDUNKNOWN;
}

UA_StatusCode InvokeReadCallback(UA_Server* server, UA_NodeId node_id,
                                 const UA_NumericRange* range,
                                 UA_DataValue* value) {
  UA_ServerConfig* config = UA_Server_getConfig(server);
  if (config == nullptr || config->nodestore == nullptr) {
    return UA_STATUSCODE_BADINTERNALERROR;
  }
  const UA_Node* node = config->nodestore->getNode(
      config->nodestore, &node_id, 0, UA_REFERENCETYPESET_NONE,
      UA_BROWSEDIRECTION_INVALID);
  if (node == nullptr || node->head.nodeClass != UA_NODECLASS_VARIABLE) {
    if (node != nullptr) {
      config->nodestore->releaseNode(config->nodestore, node);
    }
    return UA_STATUSCODE_BADNODEIDUNKNOWN;
  }
  const UA_CallbackValueSource value_source =
      node->variableNode.valueSource.callback;
  void* node_context = node->head.context;
  config->nodestore->releaseNode(config->nodestore, node);
  return value_source.read(server, nullptr, nullptr, &node_id, node_context,
                           false, range, value);
}

struct EmptyCallbackContext {
  bool adding = true;
};

UA_StatusCode EmptyReadCallback(
    UA_Server* server, const UA_NodeId* session_id, void* session_context,
    const UA_NodeId* node_id, void* node_context,
    UA_Boolean include_source_timestamp, const UA_NumericRange* range,
    UA_DataValue* value) {
  (void)server;
  (void)session_id;
  (void)session_context;
  (void)node_id;
  (void)include_source_timestamp;
  (void)range;
  auto* context = static_cast<EmptyCallbackContext*>(node_context);
  if (context != nullptr && context->adding) {
    const UA_Boolean construction_value = false;
    const UA_StatusCode result = UA_Variant_setScalarCopy(
        &value->value, &construction_value, &UA_TYPES[UA_TYPES_BOOLEAN]);
    if (result != UA_STATUSCODE_GOOD) return result;
    value->hasValue = true;
    value->hasStatus = true;
    value->status = UA_STATUSCODE_GOOD;
  }
  return UA_STATUSCODE_GOOD;
}

UA_StatusCode SeedObject(UA_Server* server, UA_NodeId node_id,
                         const char* browse_name) {
  UA_ObjectAttributes attributes = UA_ObjectAttributes_default;
  attributes.displayName = UA_LOCALIZEDTEXT(
      const_cast<char*>("en-US"), const_cast<char*>(browse_name));
  return UA_Server_addObjectNode(
      server, node_id, UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
      UA_QUALIFIEDNAME(2, const_cast<char*>(browse_name)),
      UA_NS0ID(BASEOBJECTTYPE), attributes, nullptr, nullptr);
}

int TestDestructorRemovesRegisteredNodes() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  {
    opcua::RealtimeAddressSpace address_space(&store);
    if (int rc = Expect(address_space.AddNode(
                            server.get(), {1701, "Scoped",
                                           opcua::ScalarType::kDouble, slot})
                            .ok(),
                        "scoped node add failed")) return rc;
  }

  const bool all_absent =
      NodeIsAbsent(server.get(), NumericNodeId(1701)) &&
      NodeIsAbsent(server.get(), StringNodeId("MqttSource")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.ConnectionState")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.LastSuccessfulUpdate")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.ConsecutiveFailures"));
  return Expect(all_absent,
                "address-space destruction should remove registered nodes");
}

int TestDiagnosticConflictPreservesPreexistingNode(const char* conflict_id,
                                                   std::uint32_t data_node_id) {
  auto server = MakeServer();
  if (UA_Server_addNamespace(server.get(), "urn:opcua:realtime") != 2) {
    return Expect(false, "realtime namespace should be namespace 2");
  }
  if (int rc = Expect(SeedObject(server.get(), StringNodeId(conflict_id),
                                 "PreexistingConflict") ==
                          UA_STATUSCODE_GOOD,
                      "diagnostic conflict seed failed")) return rc;

  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {data_node_id, "ConflictData",
                                          opcua::ScalarType::kDouble, slot})
                           .ok(),
                      "diagnostic conflict should fail AddNode")) return rc;

  UA_NodeClass conflict_class = UA_NODECLASS_UNSPECIFIED;
  const bool conflict_preserved =
      UA_Server_readNodeClass(server.get(), StringNodeId(conflict_id),
                              &conflict_class) == UA_STATUSCODE_GOOD &&
      conflict_class == UA_NODECLASS_OBJECT;
  if (int rc = Expect(conflict_preserved,
                      "pre-existing diagnostic conflict must remain")) {
    return rc;
  }
  if (int rc = Expect(NodeIsAbsent(server.get(), StringNodeId("MqttSource")),
                      "created diagnostics object should roll back")) {
    return rc;
  }
  if (std::string(conflict_id) == "MqttSource.LastSuccessfulUpdate") {
    return Expect(
        NodeIsAbsent(server.get(),
                     StringNodeId("MqttSource.ConnectionState")),
        "diagnostic created before middle conflict should roll back");
  }
  return 0;
}

int TestNamespaceConflictRejectsWithoutNodes() {
  auto server = MakeServer();
  if (UA_Server_addNamespace(server.get(), "urn:unrelated") != 2) {
    return Expect(false, "unrelated namespace should occupy index 2");
  }
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  opcua::RealtimeAddressSpace address_space(&store);
  const auto status = address_space.AddNode(
      server.get(), {1801, "NamespaceConflict", opcua::ScalarType::kDouble,
                     slot});
  return Expect(!status.ok() &&
                    NodeIsAbsent(server.get(), NumericNodeId(1801)) &&
                    NodeIsAbsent(server.get(), StringNodeId("MqttSource")),
                "namespace URI conflict should add no realtime nodes");
}

int TestEmbeddedNulBrowseNameIsRejected() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  opcua::RealtimeAddressSpace address_space(&store);
  const auto status = address_space.AddNode(
      server.get(), {1901, std::string("a\0b", 3),
                     opcua::ScalarType::kDouble, slot});
  return Expect(!status.ok() &&
                    NodeIsAbsent(server.get(), NumericNodeId(1901)) &&
                    NodeIsAbsent(server.get(), StringNodeId("MqttSource")),
                "embedded NUL browse name should be rejected before adds");
}

int TestQualityReviewRegressions() {
  int failures = 0;
  failures += TestDestructorRemovesRegisteredNodes();
  failures += TestDiagnosticConflictPreservesPreexistingNode(
      "MqttSource.ConnectionState", 1751);
  failures += TestDiagnosticConflictPreservesPreexistingNode(
      "MqttSource.LastSuccessfulUpdate", 1752);
  failures += TestNamespaceConflictRejectsWithoutNodes();
  failures += TestEmbeddedNulBrowseNameIsRejected();
  return failures;
}

int TestTypedNodesReadLatestSnapshots() {
  auto server = MakeServer();
  if (int rc = Expect(server != nullptr, "server creation failed")) return rc;
  opcua::RealtimeValueStore store;
  const auto double_slot = store.AddSlot(opcua::ScalarType::kDouble);
  const auto bool_slot = store.AddSlot(opcua::ScalarType::kBoolean);
  const auto int64_slot = store.AddSlot(opcua::ScalarType::kInt64);
  opcua::RealtimeAddressSpace address_space(&store);

  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1001, "Temperature",
                                         opcua::ScalarType::kDouble,
                                         double_slot})
                          .ok(),
                      "double node add failed")) return rc;
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1002, "Enabled",
                                         opcua::ScalarType::kBoolean,
                                         bool_slot})
                          .ok(),
                      "boolean node add failed")) return rc;
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1003, "Counter",
                                         opcua::ScalarType::kInt64,
                                         int64_slot})
                          .ok(),
                      "int64 node add failed")) return rc;

  store.Update(double_slot, 42.25, 101);
  store.Update(bool_slot, true, 102);
  store.Update(int64_slot, std::int64_t{-73}, 103);

  UA_DataValue double_value = Read(server.get(), NumericNodeId(1001));
  UA_DataValue bool_value = Read(server.get(), NumericNodeId(1002));
  UA_DataValue int64_value = Read(server.get(), NumericNodeId(1003));
  const bool values_match =
      HasScalar(double_value, &UA_TYPES[UA_TYPES_DOUBLE], UA_Double{42.25}) &&
      HasScalar(bool_value, &UA_TYPES[UA_TYPES_BOOLEAN], UA_Boolean{true}) &&
      HasScalar(int64_value, &UA_TYPES[UA_TYPES_INT64], UA_Int64{-73});
  const bool metadata_matches =
      double_value.hasStatus && double_value.status == UA_STATUSCODE_GOOD &&
      double_value.hasSourceTimestamp && double_value.sourceTimestamp == 101 &&
      bool_value.hasStatus && bool_value.status == UA_STATUSCODE_GOOD &&
      bool_value.hasSourceTimestamp && bool_value.sourceTimestamp == 102 &&
      int64_value.hasStatus && int64_value.status == UA_STATUSCODE_GOOD &&
      int64_value.hasSourceTimestamp && int64_value.sourceTimestamp == 103;
  UA_DataValue callback_value;
  UA_DataValue_init(&callback_value);
  const UA_StatusCode callback_status = InvokeReadCallback(
      server.get(), NumericNodeId(1001), nullptr, &callback_value);
  const bool callback_matches =
      callback_status == UA_STATUSCODE_GOOD && callback_value.hasValue &&
      callback_value.hasStatus && callback_value.status == UA_STATUSCODE_GOOD;
  UA_DataValue_clear(&callback_value);
  UA_DataValue_clear(&double_value);
  UA_DataValue_clear(&bool_value);
  UA_DataValue_clear(&int64_value);
  if (int rc = Expect(values_match, "typed callback values mismatch")) return rc;
  if (int rc = Expect(callback_matches,
                      "valued callback should execute successfully")) return rc;
  return Expect(metadata_matches, "typed callback metadata mismatch");
}

int TestReadOnlyAccessRejectsWrites() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1101, "ReadOnly",
                                         opcua::ScalarType::kDouble, slot})
                          .ok(),
                      "read-only node add failed")) return rc;

  UA_Byte access_level = 0;
  const UA_StatusCode read_access_status = UA_Server_readAccessLevel(
      server.get(), NumericNodeId(1101), &access_level);
  UA_Variant attempted_value;
  UA_Variant_init(&attempted_value);
  UA_Double attempted_scalar = 7.0;
  UA_Variant_setScalar(&attempted_value, &attempted_scalar,
                       &UA_TYPES[UA_TYPES_DOUBLE]);
  const UA_StatusCode write_status = UA_Server_writeValue(
      server.get(), NumericNodeId(1101), attempted_value);

  if (int rc = Expect(read_access_status == UA_STATUSCODE_GOOD &&
                          access_level == UA_ACCESSLEVELMASK_READ,
                      "access level should be exactly read")) return rc;
  return Expect(UA_StatusCode_isBad(write_status),
                "write to callback-backed node should be rejected");
}

int TestSnapshotQualityAndValuePresence() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto waiting_slot = store.AddSlot(opcua::ScalarType::kDouble);
  const auto disabled_slot = store.AddSlot(opcua::ScalarType::kBoolean, false);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1201, "Waiting",
                                         opcua::ScalarType::kDouble,
                                         waiting_slot})
                          .ok(),
                      "waiting node add failed")) return rc;
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1202, "Disabled",
                                         opcua::ScalarType::kBoolean,
                                         disabled_slot})
                          .ok(),
                      "disabled node add failed")) return rc;

  UA_DataValue waiting = Read(server.get(), NumericNodeId(1201));
  UA_DataValue disabled = Read(server.get(), NumericNodeId(1202));
  UA_DataValue waiting_callback;
  UA_DataValue_init(&waiting_callback);
  UA_DataValue disabled_callback;
  UA_DataValue_init(&disabled_callback);
  const UA_StatusCode waiting_callback_status = InvokeReadCallback(
      server.get(), NumericNodeId(1201), nullptr, &waiting_callback);
  const UA_StatusCode disabled_callback_status = InvokeReadCallback(
      server.get(), NumericNodeId(1202), nullptr, &disabled_callback);
  const bool initial_matches =
      waiting.hasStatus &&
      waiting.status == UA_STATUSCODE_BADWAITINGFORINITIALDATA &&
      !waiting.hasValue && disabled.hasStatus &&
      disabled.status == UA_STATUSCODE_BADOUTOFSERVICE && !disabled.hasValue;
  const bool callback_initial_matches =
      waiting_callback_status == UA_STATUSCODE_GOOD &&
      waiting_callback.hasStatus &&
      waiting_callback.status == UA_STATUSCODE_BADWAITINGFORINITIALDATA &&
      !waiting_callback.hasValue &&
      disabled_callback_status == UA_STATUSCODE_GOOD &&
      disabled_callback.hasStatus &&
      disabled_callback.status == UA_STATUSCODE_BADOUTOFSERVICE &&
      !disabled_callback.hasValue;
  UA_DataValue_clear(&waiting);
  UA_DataValue_clear(&disabled);
  UA_DataValue_clear(&waiting_callback);
  UA_DataValue_clear(&disabled_callback);
  if (int rc = Expect(initial_matches, "initial quality projection mismatch")) {
    return rc;
  }
  if (int rc = Expect(callback_initial_matches,
                      "no-value callbacks should return status in DataValue")) {
    return rc;
  }

  store.Update(waiting_slot, 8.5, 222);
  store.MarkUnavailable(waiting_slot);
  UA_DataValue unavailable = Read(server.get(), NumericNodeId(1201));
  UA_DataValue unavailable_callback;
  UA_DataValue_init(&unavailable_callback);
  const UA_StatusCode unavailable_callback_status = InvokeReadCallback(
      server.get(), NumericNodeId(1201), nullptr, &unavailable_callback);
  const bool unavailable_matches =
      HasScalar(unavailable, &UA_TYPES[UA_TYPES_DOUBLE], UA_Double{8.5}) &&
      unavailable.hasStatus &&
      unavailable.status ==
          UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE;
  const bool unavailable_callback_matches =
      unavailable_callback_status == UA_STATUSCODE_GOOD &&
      unavailable_callback.hasStatus &&
      unavailable_callback.status ==
          UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE &&
      unavailable_callback.hasValue;
  UA_DataValue_clear(&unavailable);
  UA_DataValue_clear(&unavailable_callback);
  if (int rc = Expect(unavailable_callback_matches,
                      "unavailable callback execution mismatch")) return rc;
  return Expect(unavailable_matches,
                "unavailable quality should retain last usable value");
}

int TestTimestampModesAndScalarRange() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kInt64);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1301, "Timestamped",
                                         opcua::ScalarType::kInt64, slot})
                          .ok(),
                      "timestamped node add failed")) return rc;
  store.Update(slot, std::int64_t{99}, 777);

  UA_DataValue neither =
      Read(server.get(), NumericNodeId(1301), UA_TIMESTAMPSTORETURN_NEITHER);
  UA_DataValue source =
      Read(server.get(), NumericNodeId(1301), UA_TIMESTAMPSTORETURN_SOURCE);
  UA_DataValue range = Read(server.get(), NumericNodeId(1301),
                            UA_TIMESTAMPSTORETURN_SOURCE, "0");
  UA_NumericRange callback_range = UA_NUMERICRANGE("0");
  UA_DataValue range_callback;
  UA_DataValue_init(&range_callback);
  const UA_StatusCode range_callback_status = InvokeReadCallback(
      server.get(), NumericNodeId(1301), &callback_range, &range_callback);
  const bool timestamps_match = !neither.hasSourceTimestamp &&
                                source.hasSourceTimestamp &&
                                source.sourceTimestamp == 777;
  const bool range_matches = range.hasStatus &&
                             range.status == UA_STATUSCODE_BADINDEXRANGEINVALID &&
                             !range.hasValue;
  const bool range_callback_matches =
      range_callback_status == UA_STATUSCODE_GOOD &&
      range_callback.hasStatus &&
      range_callback.status == UA_STATUSCODE_BADINDEXRANGEINVALID &&
      !range_callback.hasValue;
  UA_DataValue_clear(&neither);
  UA_DataValue_clear(&source);
  UA_DataValue_clear(&range);
  UA_DataValue_clear(&range_callback);
  if (int rc = Expect(timestamps_match, "timestamp return mode mismatch")) return rc;
  if (int rc = Expect(range_matches,
                      "scalar numeric range should be invalid")) return rc;
  return Expect(range_callback_matches,
                "range callback should return status in DataValue");
}

int TestEmptyGoodCallbackFallsBackToWaiting() {
  auto server = MakeServer();
  if (UA_Server_addNamespace(server.get(), "urn:opcua:realtime") != 2) {
    return Expect(false, "realtime namespace should be namespace 2");
  }
  UA_VariableAttributes attributes = UA_VariableAttributes_default;
  attributes.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
  attributes.valueRank = UA_VALUERANK_SCALAR;
  attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
  attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
  const UA_CallbackValueSource value_source{EmptyReadCallback, nullptr};
  EmptyCallbackContext context;
  const UA_NodeId node_id = NumericNodeId(1951);
  if (int rc = Expect(UA_Server_addCallbackValueSourceVariableNode(
                          server.get(), node_id, UA_NS0ID(OBJECTSFOLDER),
                          UA_NS0ID(ORGANIZES),
                          UA_QUALIFIEDNAME(2,
                                           const_cast<char*>("EmptyCallback")),
                          UA_NS0ID(BASEDATAVARIABLETYPE), attributes,
                          value_source, &context, nullptr) ==
                          UA_STATUSCODE_GOOD,
                      "empty callback node add failed")) return rc;
  context.adding = false;
  UA_DataValue value = Read(server.get(), node_id);
  const bool waiting = value.hasStatus &&
                       value.status ==
                           UA_STATUSCODE_BADWAITINGFORINITIALDATA &&
                       !value.hasValue;
  UA_DataValue_clear(&value);
  return Expect(waiting,
                "empty successful callback should fall back to waiting");
}

int TestCallbackNodeAddReferenceFailureIsAtomic() {
  auto server = MakeServer();
  if (UA_Server_addNamespace(server.get(), "urn:opcua:realtime") != 2) {
    return Expect(false, "realtime namespace should be namespace 2");
  }

  UA_VariableAttributes attributes = UA_VariableAttributes_default;
  attributes.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
  attributes.valueRank = UA_VALUERANK_SCALAR;
  attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
  attributes.userAccessLevel = UA_ACCESSLEVELMASK_READ;
  const UA_CallbackValueSource value_source{EmptyReadCallback, nullptr};
  EmptyCallbackContext context;
  const UA_NodeId node_id = NumericNodeId(1952);
  const UA_StatusCode status = UA_Server_addCallbackValueSourceVariableNode(
      server.get(), node_id, NumericNodeId(999999), UA_NS0ID(ORGANIZES),
      UA_QUALIFIEDNAME(2, const_cast<char*>("OrphanCandidate")),
      UA_NS0ID(BASEDATAVARIABLETYPE), attributes, value_source, &context,
      nullptr);
  return Expect(status == UA_STATUSCODE_BADPARENTNODEIDINVALID &&
                    NodeIsAbsent(server.get(), node_id),
                "failed callback node add should remove the raw node");
}

int TestDiagnosticsTrackSourceHealthAndAreIdempotent() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto first_slot = store.AddSlot(opcua::ScalarType::kDouble);
  const auto second_slot = store.AddSlot(opcua::ScalarType::kBoolean);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1401, "First",
                                         opcua::ScalarType::kDouble,
                                         first_slot})
                          .ok(),
                      "first diagnostics-triggering add failed")) return rc;

  UA_DataValue state =
      Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  UA_DataValue update =
      Read(server.get(), StringNodeId("MqttSource.LastSuccessfulUpdate"));
  UA_DataValue failures =
      Read(server.get(), StringNodeId("MqttSource.ConsecutiveFailures"));
  const UA_String connecting = UA_STRING(const_cast<char*>("connecting"));
  const bool initial_matches =
      HasString(state, connecting) &&
      HasScalar(update, &UA_TYPES[UA_TYPES_DATETIME], UA_DateTime{0}) &&
      HasScalar(failures, &UA_TYPES[UA_TYPES_UINT32], UA_UInt32{0});
  UA_DataValue_clear(&state);
  UA_DataValue_clear(&update);
  UA_DataValue_clear(&failures);
  if (int rc = Expect(initial_matches, "initial diagnostics mismatch")) return rc;

  store.SetSourceConnected();
  store.Update(first_slot, 3.5, 4567);
  state = Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  update = Read(server.get(), StringNodeId("MqttSource.LastSuccessfulUpdate"));
  const UA_String connected = UA_STRING(const_cast<char*>("connected"));
  const bool connected_matches =
      HasString(state, connected) &&
      HasScalar(update, &UA_TYPES[UA_TYPES_DATETIME], UA_DateTime{4567});
  UA_DataValue_clear(&state);
  UA_DataValue_clear(&update);
  if (int rc = Expect(connected_matches, "connected diagnostics mismatch")) return rc;

  store.SetSourceDisconnected();
  state = Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  failures = Read(server.get(), StringNodeId("MqttSource.ConsecutiveFailures"));
  const UA_String disconnected = UA_STRING(const_cast<char*>("disconnected"));
  const bool disconnected_matches =
      HasString(state, disconnected) &&
      HasScalar(failures, &UA_TYPES[UA_TYPES_UINT32], UA_UInt32{1});
  UA_DataValue_clear(&state);
  UA_DataValue_clear(&failures);
  if (int rc = Expect(disconnected_matches,
                      "disconnected diagnostics mismatch")) return rc;

  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1402, "Second",
                                         opcua::ScalarType::kBoolean,
                                         second_slot})
                          .ok(),
                      "second add should not duplicate diagnostics")) return rc;
  state = Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  const bool diagnostics_still_readable =
      HasString(state, disconnected);
  UA_DataValue_clear(&state);
  return Expect(diagnostics_still_readable,
                "idempotent diagnostics should remain readable");
}

int TestValidationDuplicateFailureAndContextLifetime() {
  auto server = MakeServer();
  opcua::RealtimeValueStore store;
  const auto double_slot = store.AddSlot(opcua::ScalarType::kDouble);
  const auto bool_slot = store.AddSlot(opcua::ScalarType::kBoolean);
  opcua::RealtimeAddressSpace null_store(nullptr);
  opcua::RealtimeAddressSpace address_space(&store);

  if (int rc = Expect(!null_store.AddNode(
                           server.get(), {1500, "NullStore",
                                          opcua::ScalarType::kDouble,
                                          double_slot})
                           .ok(),
                      "null store should be rejected")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           nullptr, {1500, "NullServer",
                                     opcua::ScalarType::kDouble, double_slot})
                           .ok(),
                      "null server should be rejected")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {0, "ZeroId",
                                          opcua::ScalarType::kDouble,
                                          double_slot})
                           .ok(),
                      "zero node ID should be rejected")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {1500, "",
                                          opcua::ScalarType::kDouble,
                                          double_slot})
                           .ok(),
                      "empty browse name should be rejected")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {1500, "MissingSlot",
                                          opcua::ScalarType::kDouble, 999})
                           .ok(),
                      "missing slot should be rejected")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {1500, "WrongType",
                                          opcua::ScalarType::kBoolean,
                                          double_slot})
                           .ok(),
                      "slot type mismatch should be rejected")) return rc;

  if (int rc = Expect(address_space.AddNode(
                          server.get(), {1501, "Stable",
                                         opcua::ScalarType::kDouble,
                                         double_slot})
                          .ok(),
                      "valid node add failed")) return rc;
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {1501, "Duplicate",
                                          opcua::ScalarType::kBoolean,
                                          bool_slot})
                           .ok(),
                      "duplicate node ID should fail")) return rc;
  UA_DataValue diagnostics =
      Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  const UA_String connecting = UA_STRING(const_cast<char*>("connecting"));
  const bool diagnostics_preserved = HasString(diagnostics, connecting);
  UA_DataValue_clear(&diagnostics);
  if (int rc = Expect(diagnostics_preserved,
                      "later add failure should preserve diagnostics")) {
    return rc;
  }
  store.Update(double_slot, 64.0, 900);
  UA_DataValue stable = Read(server.get(), NumericNodeId(1501));
  const bool stable_context =
      HasScalar(stable, &UA_TYPES[UA_TYPES_DOUBLE], UA_Double{64.0}) &&
      stable.hasSourceTimestamp && stable.sourceTimestamp == 900;
  UA_DataValue_clear(&stable);
  return Expect(stable_context,
                "failed duplicate add should not invalidate existing context");
}

int TestFailedFirstAddRollsBackDiagnosticsAndCanRetry() {
  auto server = MakeServer();
  if (int rc = Expect(server != nullptr, "server creation failed")) return rc;
  if (int rc = Expect(UA_Server_addNamespace(server.get(),
                                             "urn:opcua:realtime") == 2,
                      "realtime namespace should be namespace 2")) return rc;

  constexpr std::uint32_t kConflictingNodeId = 1601;
  UA_ObjectAttributes conflict_attributes = UA_ObjectAttributes_default;
  conflict_attributes.displayName = UA_LOCALIZEDTEXT(
      const_cast<char*>("en-US"), const_cast<char*>("Conflict"));
  const UA_StatusCode conflict_status = UA_Server_addObjectNode(
      server.get(), NumericNodeId(kConflictingNodeId),
      UA_NS0ID(OBJECTSFOLDER), UA_NS0ID(ORGANIZES),
      UA_QUALIFIEDNAME(2, const_cast<char*>("Conflict")),
      UA_NS0ID(BASEOBJECTTYPE), conflict_attributes, nullptr, nullptr);
  if (int rc = Expect(conflict_status == UA_STATUSCODE_GOOD,
                      "conflicting node seed failed")) return rc;

  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  opcua::RealtimeAddressSpace address_space(&store);
  if (int rc = Expect(!address_space.AddNode(
                           server.get(), {kConflictingNodeId, "ConflictingData",
                                          opcua::ScalarType::kDouble, slot})
                           .ok(),
                      "first add with conflicting ID should fail")) return rc;

  const bool diagnostics_absent =
      NodeIsAbsent(server.get(), StringNodeId("MqttSource")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.ConnectionState")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.LastSuccessfulUpdate")) &&
      NodeIsAbsent(server.get(),
                   StringNodeId("MqttSource.ConsecutiveFailures"));
  if (int rc = Expect(diagnostics_absent,
                      "failed first add should roll back diagnostics")) {
    return rc;
  }

  if (int rc = Expect(UA_Server_deleteNode(
                          server.get(), NumericNodeId(kConflictingNodeId), true) ==
                          UA_STATUSCODE_GOOD,
                      "conflicting node cleanup failed")) return rc;
  if (int rc = Expect(address_space.AddNode(
                          server.get(), {kConflictingNodeId, "ConflictingData",
                                         opcua::ScalarType::kDouble, slot})
                          .ok(),
                      "retry after conflict cleanup should succeed")) return rc;

  UA_DataValue state =
      Read(server.get(), StringNodeId("MqttSource.ConnectionState"));
  UA_DataValue update =
      Read(server.get(), StringNodeId("MqttSource.LastSuccessfulUpdate"));
  UA_DataValue failures =
      Read(server.get(), StringNodeId("MqttSource.ConsecutiveFailures"));
  const UA_String connecting = UA_STRING(const_cast<char*>("connecting"));
  const bool diagnostics_readable =
      HasString(state, connecting) &&
      HasScalar(update, &UA_TYPES[UA_TYPES_DATETIME], UA_DateTime{0}) &&
      HasScalar(failures, &UA_TYPES[UA_TYPES_UINT32], UA_UInt32{0});
  UA_DataValue_clear(&state);
  UA_DataValue_clear(&update);
  UA_DataValue_clear(&failures);
  return Expect(diagnostics_readable,
                "retry should create one readable diagnostics set");
}

}  // namespace

int main() {
  if (int rc = TestQualityReviewRegressions()) return rc;
  if (int rc = TestTypedNodesReadLatestSnapshots()) return rc;
  if (int rc = TestReadOnlyAccessRejectsWrites()) return rc;
  if (int rc = TestSnapshotQualityAndValuePresence()) return rc;
  if (int rc = TestTimestampModesAndScalarRange()) return rc;
  if (int rc = TestEmptyGoodCallbackFallsBackToWaiting()) return rc;
  if (int rc = TestCallbackNodeAddReferenceFailureIsAtomic()) return rc;
  if (int rc = TestDiagnosticsTrackSourceHealthAndAreIdempotent()) return rc;
  if (int rc = TestValidationDuplicateFailureAndContextLifetime()) return rc;
  if (int rc = TestFailedFirstAddRollsBackDiagnosticsAndCanRetry()) return rc;
  return 0;
}
