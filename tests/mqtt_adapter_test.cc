#include "daemon/mqtt_adapter.h"

#include <chrono>
#include <iostream>
#include <variant>

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

int main() {
  if (int rc = TestBrokerFreeStateTransitions()) return rc;
  return 0;
}
