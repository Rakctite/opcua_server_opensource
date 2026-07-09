#include "config/server_config.h"

namespace opcua {

ServerConfig ServerConfig::Default() {
  return ServerConfig{
      "Open62541 C++ Server",
      "urn:rakctite:opcua-server-opensource",
      "0.0.0.0",
      4840,
      "/",
      "none",
      "none",
      100,
      100,
      "info",
      "stdout",
      "builtin",
      "",
  };
}

Status ServerConfig::Validate() const {
  if (server_application_name.empty()) {
    return Status::Error("server.application_name must not be empty");
  }
  if (server_product_uri.empty()) {
    return Status::Error("server.product_uri must not be empty");
  }
  if (server_bind_address.empty()) {
    return Status::Error("server.bind_address must not be empty");
  }
  if (server_port < 1 || server_port > 65535) {
    return Status::Error("server.port must be between 1 and 65535");
  }
  if (server_endpoint_path.empty() || server_endpoint_path[0] != '/') {
    return Status::Error("server.endpoint_path must start with /");
  }
  if (security_mode != "none") {
    return Status::Error("only security.mode=none is supported in v1");
  }
  if (security_policy != "none") {
    return Status::Error("only security.policy=none is supported in v1");
  }
  if (max_sessions <= 0) {
    return Status::Error("limits.max_sessions must be positive");
  }
  if (max_subscriptions <= 0) {
    return Status::Error("limits.max_subscriptions must be positive");
  }
  if (logging_level != "trace" && logging_level != "debug" &&
      logging_level != "info" && logging_level != "warn" &&
      logging_level != "error") {
    return Status::Error("logging.level is invalid");
  }
  if (logging_target != "stdout" &&
      (logging_target.rfind("file:", 0) != 0 ||
       logging_target.size() == std::string("file:").size())) {
    return Status::Error("logging.target must be stdout or file:<path>");
  }
  if (address_space_mode != "builtin") {
    return Status::Error("only address_space.mode=builtin is supported in v1");
  }
  return Status::Ok();
}

}  // namespace opcua
