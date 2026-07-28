#include "daemon/opcua_server.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4819)
#endif
#include "open62541.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "daemon/mqtt_adapter.h"
#include "daemon/realtime_address_space.h"

namespace opcua {

namespace {

struct ServerDeleter {
  void operator()(UA_Server* server) const {
    if (server != nullptr) {
      UA_Server_delete(server);
    }
  }
};

using ServerPtr = std::unique_ptr<UA_Server, ServerDeleter>;

Status StatusFromOpen62541(UA_StatusCode code, const char* operation) {
  if (!UA_StatusCode_isBad(code)) {
    return Status::Ok();
  }
  return Status::Error(std::string(operation) + " failed: " + UA_StatusCode_name(code));
}

Result<ScalarType> ScalarTypeFromMqttDataType(const std::string& data_type) {
  if (data_type == "boolean") {
    return ScalarType::kBoolean;
  }
  if (data_type == "int64") {
    return ScalarType::kInt64;
  }
  if (data_type == "double") {
    return ScalarType::kDouble;
  }
  return Result<ScalarType>::Error(
      Status::Error("mqtt.data_type must be boolean, int64, or double"));
}

class ServerShutdown {
 public:
  explicit ServerShutdown(UA_Server* server) : server_(server) {}

  ServerShutdown(const ServerShutdown&) = delete;
  ServerShutdown& operator=(const ServerShutdown&) = delete;

  ~ServerShutdown() { Shutdown(); }

  Status Shutdown() {
    if (server_ != nullptr) {
      shutdown_status_ = UA_Server_run_shutdown(server_);
      server_ = nullptr;
    }
    return StatusFromOpen62541(shutdown_status_, "UA_Server_run_shutdown");
  }

 private:
  UA_Server* server_;
  UA_StatusCode shutdown_status_ = UA_STATUSCODE_GOOD;
};

}  // namespace

OpcuaServer::OpcuaServer(ServerConfig server_config, MqttConfig mqtt_config)
    : server_config_(std::move(server_config)),
      mqtt_config_(std::move(mqtt_config)) {}

Status OpcuaServer::Run(std::atomic_bool* running) {
  if (running == nullptr) {
    return Status::Error("running flag must not be null");
  }

  auto validate_status = server_config_.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }
  validate_status = mqtt_config_.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }
  auto type_result = ScalarTypeFromMqttDataType(mqtt_config_.data_type);
  if (!type_result.ok()) {
    return type_result.status();
  }
  const ScalarType type = type_result.value();

  ServerPtr server(UA_Server_new());
  if (server == nullptr) {
    return Status::Error("UA_Server_new failed");
  }

  UA_ServerConfig* server_config = UA_Server_getConfig(server.get());
  if (server_config == nullptr) {
    return Status::Error("UA_Server_getConfig failed");
  }

  const auto port = static_cast<UA_UInt16>(server_config_.server_port);
  auto config_status = StatusFromOpen62541(
      UA_ServerConfig_setMinimal(server_config, port, nullptr),
      "UA_ServerConfig_setMinimal");
  if (!config_status.ok()) {
    return config_status;
  }

  {
    RealtimeValueStore value_store;
    const ValueSlotId slot = value_store.AddSlot(type, mqtt_config_.enabled);
    if (!mqtt_config_.enabled) {
      value_store.SetSourceDisabled();
    }
    RealtimeAddressSpace address_space(&value_store);

    const RealtimeNodeConfig node_config{mqtt_config_.node_id,
                                         mqtt_config_.browse_name, type, slot};
    auto node_status = address_space.AddNode(server.get(), node_config);
    if (!node_status.ok()) {
      return node_status;
    }

    auto startup_status = StatusFromOpen62541(
        UA_Server_run_startup(server.get()), "UA_Server_run_startup");
    if (!startup_status.ok()) {
      return startup_status;
    }

    ServerShutdown shutdown(server.get());
    {
      std::optional<MqttAdapter> mqtt_adapter;
      if (mqtt_config_.enabled) {
        mqtt_adapter.emplace(mqtt_config_, type, &value_store, slot);
        auto mqtt_status = mqtt_adapter->Start();
        if (!mqtt_status.ok()) {
          return mqtt_status;
        }
      }

      while (running->load()) {
        UA_Server_run_iterate(server.get(), true);
      }
    }

    return shutdown.Shutdown();
  }
}

}  // namespace opcua
