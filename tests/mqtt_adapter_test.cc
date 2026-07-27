#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <variant>

#include "daemon/mqtt_adapter.h"

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

int TestBrokerFreeStateTransitions() {
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  auto config = opcua::MqttConfig::Default();
  config.enabled = true;
  config.stale_timeout_ms = 50;

  opcua::MqttAdapter adapter(config, opcua::ScalarType::kDouble, &store, slot);
  if (int rc = Expect(adapter.AcceptMessage("test/temperature", "37.5",
                                            UA_DateTime_now())
                          .ok(),
                      "valid message should be accepted")) {
    return rc;
  }

  auto snapshot = store.ReadSnapshot(slot).value();
  if (int rc = Expect(snapshot.status == UA_STATUSCODE_GOOD,
                      "accepted message should mark value good")) {
    return rc;
  }
  if (int rc = Expect(std::holds_alternative<double>(snapshot.value) &&
                          std::get<double>(snapshot.value) == 37.5,
                      "accepted message should update double value")) {
    return rc;
  }

  adapter.NotifyConnectionLost();
  snapshot = store.ReadSnapshot(slot).value();
  if (int rc = Expect(
          snapshot.status ==
              UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE,
          "connection loss should mark last value uncertain")) {
    return rc;
  }
  if (int rc = Expect(std::holds_alternative<double>(snapshot.value) &&
                          std::get<double>(snapshot.value) == 37.5,
                      "connection loss should retain last value")) {
    return rc;
  }

  if (int rc = Expect(adapter.AcceptMessage("test/temperature", "38.25",
                                            UA_DateTime_now())
                          .ok(),
                      "valid message after loss should be accepted")) {
    return rc;
  }
  snapshot = store.ReadSnapshot(slot).value();
  if (int rc = Expect(snapshot.status == UA_STATUSCODE_GOOD,
                      "valid message should restore good quality")) {
    return rc;
  }
  if (int rc = Expect(std::holds_alternative<double>(snapshot.value) &&
                          std::get<double>(snapshot.value) == 38.25,
                      "valid message should replace retained value")) {
    return rc;
  }

  adapter.PollHealth(std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(100));
  snapshot = store.ReadSnapshot(slot).value();
  if (int rc = Expect(
          snapshot.status ==
              UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE,
          "stale health poll should mark last value uncertain")) {
    return rc;
  }
  if (int rc = Expect(std::holds_alternative<double>(snapshot.value) &&
                          std::get<double>(snapshot.value) == 38.25,
                      "stale health poll should retain last value")) {
    return rc;
  }

  if (int rc =
          Expect(!adapter.AcceptMessage("other/topic", "99", UA_DateTime_now())
                      .ok(),
                 "wrong topic should be rejected")) {
    return rc;
  }
  return Expect(
      !adapter.AcceptMessage("test/temperature", "bad", UA_DateTime_now()).ok(),
      "bad payload should be rejected");
}

}  // namespace

namespace opcua {

int MqttAdapterCallbackParseFailuresUseAggregateLogOnlyForTest() {
  opcua::RealtimeValueStore store;
  const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
  auto config = opcua::MqttConfig::Default();
  config.enabled = true;

  opcua::MqttAdapter adapter(config, opcua::ScalarType::kDouble, &store, slot);
  adapter.accepting_.store(true);

  auto message = static_cast<MQTTAsync_message*>(
      MQTTAsync_malloc(sizeof(MQTTAsync_message)));
  *message = MQTTAsync_message_initializer;
  constexpr char payload[] = "bad";
  char* payload_copy = static_cast<char*>(MQTTAsync_malloc(sizeof(payload)));
  std::memcpy(payload_copy, payload, sizeof(payload));
  message->payload = payload_copy;
  message->payloadlen = static_cast<int>(sizeof(payload) - 1);

  char* topic = static_cast<char*>(MQTTAsync_malloc(config.topic.size() + 1));
  std::memcpy(topic, config.topic.c_str(), config.topic.size() + 1);

  std::ostringstream captured;
  auto* const old_cerr = std::cerr.rdbuf(captured.rdbuf());
  const int arrived =
      opcua::MqttAdapter::MessageArrived(&adapter, topic, 0, message);
  std::cerr.rdbuf(old_cerr);

  if (int rc = Expect(arrived == 1, "callback should consume message")) {
    return rc;
  }

  const std::string logs = captured.str();
  if (int rc = Expect(logs.find("MQTT payload parse failures: 1") !=
                          std::string::npos,
                      "parse failure should use aggregate log")) {
    return rc;
  }
  if (int rc = Expect(logs.find("MQTT message rejected") == std::string::npos,
                      "parse failure should not emit rejection log")) {
    return rc;
  }
  return Expect(logs.find(payload) == std::string::npos,
                "parse failure log should not include payload contents");
}

}  // namespace opcua

int main() {
  if (int rc = TestBrokerFreeStateTransitions()) return rc;
  if (int rc =
          opcua::MqttAdapterCallbackParseFailuresUseAggregateLogOnlyForTest()) {
    return rc;
  }
  return 0;
}
