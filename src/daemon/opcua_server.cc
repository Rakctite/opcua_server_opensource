#include "daemon/opcua_server.h"

#include <memory>
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

}  // namespace

OpcuaServer::OpcuaServer(ServerConfig config) : config_(std::move(config)) {}

Status OpcuaServer::Run(std::atomic_bool* running) {
  if (running == nullptr) {
    return Status::Error("running flag must not be null");
  }

  auto validate_status = config_.Validate();
  if (!validate_status.ok()) {
    return validate_status;
  }

  ServerPtr server(UA_Server_new());
  if (server == nullptr) {
    return Status::Error("UA_Server_new failed");
  }

  UA_ServerConfig* server_config = UA_Server_getConfig(server.get());
  if (server_config == nullptr) {
    return Status::Error("UA_Server_getConfig failed");
  }

  const auto port = static_cast<UA_UInt16>(config_.server_port);
  auto config_status = StatusFromOpen62541(
      UA_ServerConfig_setMinimal(server_config, port, nullptr),
      "UA_ServerConfig_setMinimal");
  if (!config_status.ok()) {
    return config_status;
  }

  while (running->load()) {
    auto iterate_status = StatusFromOpen62541(
        UA_Server_run_iterate(server.get(), true),
        "UA_Server_run_iterate");
    if (!iterate_status.ok()) {
      return iterate_status;
    }
  }

  return Status::Ok();
}

}  // namespace opcua
