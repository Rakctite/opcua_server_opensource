#include "config/server_config.h"

#include <iostream>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  const auto defaults = opcua::ServerConfig::Default();
  if (int rc = Expect(defaults.Validate().ok(), "default config should validate")) return rc;
  if (int rc = Expect(defaults.server_port == 4840, "default OPC UA port should be 4840")) return rc;
  if (int rc = Expect(defaults.server_bind_address == "0.0.0.0", "default bind address should be 0.0.0.0")) return rc;
  if (int rc = Expect(defaults.logging_target == "stdout", "default logging target should be stdout")) return rc;

  auto invalid_port = defaults;
  invalid_port.server_port = 0;
  if (int rc = Expect(!invalid_port.Validate().ok(), "port 0 should fail validation")) return rc;

  invalid_port.server_port = 70000;
  if (int rc = Expect(!invalid_port.Validate().ok(), "port 70000 should fail validation")) return rc;

  auto invalid_address_space = defaults;
  invalid_address_space.address_space_mode = "nodeset";
  if (int rc = Expect(!invalid_address_space.Validate().ok(), "nodeset mode should not be enabled in v1")) return rc;

  auto invalid_logging = defaults;
  invalid_logging.logging_target = "network";
  if (int rc = Expect(!invalid_logging.Validate().ok(), "invalid logging target should fail")) return rc;

  invalid_logging.logging_target = "file:";
  if (int rc = Expect(!invalid_logging.Validate().ok(), "empty file logging target should fail")) return rc;

  return 0;
}
