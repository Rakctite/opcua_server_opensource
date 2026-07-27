#include "config/mqtt_config.h"

#include <iostream>
#include <string>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

int ExpectInvalidBrokerUri(const std::string& broker_uri,
                           const char* message) {
  auto config = opcua::MqttConfig::Default();
  config.broker_uri = broker_uri;
  return Expect(!config.Validate().ok(), message);
}

int TestDefaults() {
  const auto config = opcua::MqttConfig::Default();
  if (int rc = Expect(config.Validate().ok(), "default config should validate")) {
    return rc;
  }
  if (int rc = Expect(!config.enabled, "MQTT should be disabled by default")) {
    return rc;
  }

  auto enabled = config;
  enabled.enabled = true;
  return Expect(enabled.Validate().ok(), "enabled default config should validate");
}

int TestReviewRegressions() {
  int failures = 0;
  failures += ExpectInvalidBrokerUri("tcp://@:1883",
                                     "broker userinfo should fail");
  failures += ExpectInvalidBrokerUri(
      "tcp://[not-an-ipv6-address]:1883",
      "non-IPv6 bracketed host should fail");

  auto config = opcua::MqttConfig::Default();
  config.client_id = std::string("a\0b", 3);
  failures += Expect(!config.Validate().ok(),
                     "embedded NUL in client_id should fail");

  config = opcua::MqttConfig::Default();
  config.topic = std::string("a\0b", 3);
  failures +=
      Expect(!config.Validate().ok(), "embedded NUL in topic should fail");
  return failures;
}

int TestIpv4BrokerUris() {
  int failures = 0;
  auto config = opcua::MqttConfig::Default();
  config.broker_uri = "tcp://0.0.0.0:1883";
  failures += Expect(config.Validate().ok(),
                     "minimum IPv4 address should validate");

  config.broker_uri = "tcp://255.255.255.255:1883";
  failures += Expect(config.Validate().ok(),
                     "maximum IPv4 address should validate");

  failures += ExpectInvalidBrokerUri("tcp://256.1.1.1:1883",
                                     "out-of-range IPv4 octet should fail");
  failures += ExpectInvalidBrokerUri(
      "tcp://010.0.0.1:1883",
      "multi-digit IPv4 octet with leading zero should fail");
  failures += ExpectInvalidBrokerUri("tcp://1.2.3:1883",
                                     "IPv4 address with three groups should fail");
  return failures;
}

int TestBrokerUris() {
  auto ipv6 = opcua::MqttConfig::Default();
  ipv6.broker_uri = "tcp://[::1]:1883";
  if (int rc = Expect(ipv6.Validate().ok(),
                      "bracketed IPv6 broker URI should validate")) {
    return rc;
  }

  if (int rc = ExpectInvalidBrokerUri("http://localhost:1883",
                                      "non-TCP scheme should fail validation")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost",
                                      "broker URI without port should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost:70000",
                                      "out-of-range broker port should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://:1883",
                                      "empty broker host should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost:0",
                                      "broker port zero should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost:1883/path",
                                      "broker URI path should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost:1883?keepalive=10",
                                      "broker URI query should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://local host:1883",
                                      "broker URI whitespace should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://[::1:1883",
                                      "unclosed IPv6 bracket should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri(
          "tcp://[1:2:3:4:5:6:7:8:]:1883",
          "IPv6 address with trailing colon should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri(
          "tcp://[::ffff:192.0.2.1.]:1883",
          "IPv6 address with malformed IPv4 tail should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://::1:1883",
                                      "unbracketed IPv6 should fail")) {
    return rc;
  }
  if (int rc = ExpectInvalidBrokerUri("tcp://localhost:1883x",
                                      "non-decimal broker port should fail")) {
    return rc;
  }

  std::string embedded_nul = "tcp://localhost:1883";
  embedded_nul.push_back('\0');
  embedded_nul += "ignored";
  return ExpectInvalidBrokerUri(embedded_nul,
                                "embedded NUL in broker URI should fail");
}

int TestFields() {
  auto config = opcua::MqttConfig::Default();
  config.client_id.clear();
  if (int rc = Expect(!config.Validate().ok(), "empty client_id should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.topic = "test/#";
  if (int rc = Expect(!config.Validate().ok(), "wildcard topic should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.topic = "test/+";
  if (int rc = Expect(!config.Validate().ok(), "single-level wildcard should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.qos = 2;
  if (int rc = Expect(!config.Validate().ok(), "qos 2 should fail")) return rc;

  config = opcua::MqttConfig::Default();
  config.node_id = 0;
  if (int rc = Expect(!config.Validate().ok(), "node_id 0 should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.data_type = "string";
  if (int rc = Expect(!config.Validate().ok(), "string data_type should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.stale_timeout_ms = 0;
  if (int rc = Expect(!config.Validate().ok(),
                      "stale_timeout_ms 0 should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.topic.clear();
  if (int rc = Expect(!config.Validate().ok(), "empty topic should fail")) {
    return rc;
  }

  config = opcua::MqttConfig::Default();
  config.browse_name.clear();
  if (int rc = Expect(!config.Validate().ok(), "empty browse_name should fail")) {
    return rc;
  }

  for (const std::string& data_type : {"boolean", "int64", "double"}) {
    config = opcua::MqttConfig::Default();
    config.data_type = data_type;
    if (int rc = Expect(config.Validate().ok(),
                        "supported data_type should validate")) {
      return rc;
    }
  }
  return 0;
}

}  // namespace

int main() {
  if (int rc = TestDefaults()) return rc;
  if (int rc = TestReviewRegressions()) return rc;
  if (int rc = TestIpv4BrokerUris()) return rc;
  if (int rc = TestBrokerUris()) return rc;
  if (int rc = TestFields()) return rc;
  return 0;
}
