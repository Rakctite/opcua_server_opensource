#include "daemon/realtime_value_store.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <variant>
#include <vector>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

bool SnapshotsEqual(const opcua::ValueSnapshot& lhs,
                    const opcua::ValueSnapshot& rhs) {
  return lhs.type == rhs.type && lhs.value == rhs.value &&
         lhs.status == rhs.status &&
         lhs.source_timestamp == rhs.source_timestamp &&
         lhs.sequence == rhs.sequence && lhs.has_value == rhs.has_value;
}

int TestSlotIdsAreDeterministicAndDistinct() {
  opcua::RealtimeValueStore store;
  const auto first = store.AddSlot(opcua::ScalarType::kBoolean);
  const auto second = store.AddSlot(opcua::ScalarType::kInt64);
  const auto third = store.AddSlot(opcua::ScalarType::kDouble);

  if (int rc = Expect(first == 0, "first slot ID should be zero")) return rc;
  if (int rc = Expect(second == 1, "second slot ID should be one")) return rc;
  return Expect(third == 2, "third slot ID should be two");
}

int TestDoubleLifecycle() {
  opcua::RealtimeValueStore store;
  const auto id = store.AddSlot(opcua::ScalarType::kDouble);

  auto snapshot = store.ReadSnapshot(id);
  if (int rc = Expect(snapshot.ok(), "initial double snapshot should exist")) return rc;
  if (int rc = Expect(snapshot.value().type == opcua::ScalarType::kDouble,
                      "double snapshot type mismatch")) return rc;
  if (int rc = Expect(std::holds_alternative<double>(snapshot.value().value),
                      "double snapshot should have typed default")) return rc;
  if (int rc = Expect(std::get<double>(snapshot.value().value) == 0.0,
                      "double default should be zero")) return rc;
  if (int rc = Expect(snapshot.value().status ==
                          UA_STATUSCODE_BADWAITINGFORINITIALDATA,
                      "initial double status should be waiting")) return rc;
  if (int rc = Expect(!snapshot.value().has_value,
                      "initial double should have no value")) return rc;
  if (int rc = Expect(snapshot.value().sequence == 0,
                      "initial double sequence should be zero")) return rc;
  if (int rc = Expect(snapshot.value().source_timestamp == 0,
                      "initial double timestamp should be zero")) return rc;

  if (int rc = Expect(store.Update(id, 37.5, 100),
                      "first double update should change snapshot")) return rc;
  snapshot = store.ReadSnapshot(id);
  if (int rc = Expect(snapshot.value().value == opcua::ScalarValue(37.5),
                      "updated double value mismatch")) return rc;
  if (int rc = Expect(snapshot.value().status == UA_STATUSCODE_GOOD,
                      "updated double status should be good")) return rc;
  if (int rc = Expect(snapshot.value().has_value,
                      "updated double should have a value")) return rc;
  if (int rc = Expect(snapshot.value().sequence == 1,
                      "first update should set sequence one")) return rc;
  if (int rc = Expect(snapshot.value().source_timestamp == 100,
                      "first update timestamp mismatch")) return rc;

  if (int rc = Expect(!store.Update(id, 37.5, 200),
                      "same good value should not change sequence")) return rc;
  snapshot = store.ReadSnapshot(id);
  if (int rc = Expect(snapshot.value().sequence == 1,
                      "same update should preserve sequence")) return rc;
  if (int rc = Expect(snapshot.value().source_timestamp == 200,
                      "same update should refresh timestamp")) return rc;

  if (int rc = Expect(store.MarkUnavailable(id),
                      "first unavailable mark should change quality")) return rc;
  snapshot = store.ReadSnapshot(id);
  if (int rc = Expect(snapshot.value().value == opcua::ScalarValue(37.5),
                      "unavailable should preserve last value")) return rc;
  if (int rc = Expect(snapshot.value().status ==
                          UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE,
                      "unavailable status mismatch")) return rc;
  if (int rc = Expect(snapshot.value().sequence == 2,
                      "unavailable should increment sequence")) return rc;
  if (int rc = Expect(snapshot.value().source_timestamp == 200,
                      "unavailable should preserve timestamp")) return rc;

  if (int rc = Expect(store.Update(id, 37.5, 300),
                      "same value should recover uncertain quality")) return rc;
  snapshot = store.ReadSnapshot(id);
  if (int rc = Expect(snapshot.value().status == UA_STATUSCODE_GOOD,
                      "recovered status should be good")) return rc;
  if (int rc = Expect(snapshot.value().sequence == 3,
                      "quality recovery should increment sequence")) return rc;
  return Expect(snapshot.value().source_timestamp == 300,
                "quality recovery timestamp mismatch");
}

int TestRepeatedNanIsSemanticallyEqual() {
  opcua::RealtimeValueStore store;
  const auto id = store.AddSlot(opcua::ScalarType::kDouble);
  const double nan = std::numeric_limits<double>::quiet_NaN();

  if (int rc = Expect(store.Update(id, nan, 100),
                      "first NaN update should change snapshot")) return rc;
  if (int rc = Expect(!store.Update(id, nan, 200),
                      "repeated NaN should be semantically equal")) return rc;

  const auto snapshot = store.ReadSnapshot(id).value();
  if (int rc = Expect(std::isnan(std::get<double>(snapshot.value)),
                      "repeated NaN should preserve a NaN value")) return rc;
  if (int rc = Expect(snapshot.sequence == 1,
                      "repeated NaN should preserve sequence")) return rc;
  if (int rc = Expect(snapshot.source_timestamp == 200,
                      "repeated NaN should refresh timestamp")) return rc;
  return Expect(store.ReadSourceHealth().last_successful_update == 200,
                "repeated NaN should refresh source health");
}

int TestSignedZerosAreSemanticallyEqualAndPreserveRepresentation() {
  opcua::RealtimeValueStore store;
  const auto negative_id = store.AddSlot(opcua::ScalarType::kDouble);
  const auto positive_id = store.AddSlot(opcua::ScalarType::kDouble);

  if (int rc = Expect(store.Update(negative_id, -0.0, 100),
                      "first negative zero update should change snapshot")) return rc;
  if (int rc = Expect(!store.Update(negative_id, 0.0, 200),
                      "positive zero should equal stored negative zero")) return rc;
  auto snapshot = store.ReadSnapshot(negative_id).value();
  if (int rc = Expect(snapshot.sequence == 1 &&
                          std::signbit(std::get<double>(snapshot.value)),
                      "equal positive zero should preserve negative zero")) return rc;
  if (int rc = Expect(snapshot.source_timestamp == 200,
                      "equal positive zero should refresh timestamp")) return rc;

  if (int rc = Expect(store.Update(positive_id, 0.0, 300),
                      "first positive zero update should change snapshot")) return rc;
  if (int rc = Expect(!store.Update(positive_id, -0.0, 400),
                      "negative zero should equal stored positive zero")) return rc;
  snapshot = store.ReadSnapshot(positive_id).value();
  if (int rc = Expect(snapshot.sequence == 1 &&
                          !std::signbit(std::get<double>(snapshot.value)),
                      "equal negative zero should preserve positive zero")) return rc;

  const double infinity = std::numeric_limits<double>::infinity();
  if (int rc = Expect(store.Update(positive_id, infinity, 500),
                      "positive infinity should differ from zero")) return rc;
  if (int rc = Expect(store.Update(positive_id, -infinity, 600),
                      "negative infinity should differ from positive infinity")) return rc;
  snapshot = store.ReadSnapshot(positive_id).value();
  return Expect(snapshot.sequence == 3 &&
                    std::get<double>(snapshot.value) == -infinity,
                "distinct infinities should update value and sequence");
}

int TestBooleanAndInt64Slots() {
  opcua::RealtimeValueStore store;
  const auto boolean_id = store.AddSlot(opcua::ScalarType::kBoolean);
  const auto int64_id = store.AddSlot(opcua::ScalarType::kInt64);

  if (int rc = Expect(store.Update(boolean_id, true, 10),
                      "boolean update should succeed")) return rc;
  if (int rc = Expect(store.Update(int64_id, std::int64_t{-42}, 20),
                      "int64 update should succeed")) return rc;

  const auto boolean_snapshot = store.ReadSnapshot(boolean_id);
  const auto int64_snapshot = store.ReadSnapshot(int64_id);
  if (int rc = Expect(boolean_snapshot.ok() && int64_snapshot.ok(),
                      "typed snapshots should exist")) return rc;
  if (int rc = Expect(boolean_snapshot.value().type == opcua::ScalarType::kBoolean &&
                          std::holds_alternative<bool>(boolean_snapshot.value().value) &&
                          std::get<bool>(boolean_snapshot.value().value),
                      "boolean snapshot value/type mismatch")) return rc;
  return Expect(int64_snapshot.value().type == opcua::ScalarType::kInt64 &&
                    std::holds_alternative<std::int64_t>(int64_snapshot.value().value) &&
                    std::get<std::int64_t>(int64_snapshot.value().value) == -42,
                "int64 snapshot value/type mismatch");
}

int TestTypeMismatchesLeaveSnapshotsUnchanged() {
  opcua::RealtimeValueStore store;
  const auto boolean_id = store.AddSlot(opcua::ScalarType::kBoolean);
  const auto int64_id = store.AddSlot(opcua::ScalarType::kInt64);
  const auto double_id = store.AddSlot(opcua::ScalarType::kDouble);

  const auto original_boolean = store.ReadSnapshot(boolean_id).value();
  const auto original_int64 = store.ReadSnapshot(int64_id).value();
  const auto original_double = store.ReadSnapshot(double_id).value();

  if (int rc = Expect(!store.Update(boolean_id, std::int64_t{1}, 1),
                      "boolean slot should reject int64")) return rc;
  if (int rc = Expect(!store.Update(boolean_id, 1.0, 2),
                      "boolean slot should reject double")) return rc;
  if (int rc = Expect(!store.Update(int64_id, true, 3),
                      "int64 slot should reject boolean")) return rc;
  if (int rc = Expect(!store.Update(int64_id, 1.0, 4),
                      "int64 slot should reject double")) return rc;
  if (int rc = Expect(!store.Update(double_id, true, 5),
                      "double slot should reject boolean")) return rc;
  if (int rc = Expect(!store.Update(double_id, std::int64_t{1}, 6),
                      "double slot should reject int64")) return rc;

  if (int rc = Expect(SnapshotsEqual(store.ReadSnapshot(boolean_id).value(),
                                     original_boolean),
                      "rejected boolean updates changed snapshot")) return rc;
  if (int rc = Expect(SnapshotsEqual(store.ReadSnapshot(int64_id).value(),
                                     original_int64),
                      "rejected int64 updates changed snapshot")) return rc;
  return Expect(SnapshotsEqual(store.ReadSnapshot(double_id).value(), original_double),
                "rejected double updates changed snapshot");
}

int TestDisabledSlotRemainsOutOfService() {
  opcua::RealtimeValueStore store;
  const auto id = store.AddSlot(opcua::ScalarType::kDouble, false);
  const auto initial = store.ReadSnapshot(id).value();
  if (int rc = Expect(initial.status == UA_STATUSCODE_BADOUTOFSERVICE &&
                          !initial.has_value && initial.sequence == 0,
                      "disabled slot initial state mismatch")) return rc;
  if (int rc = Expect(!store.Update(id, 12.0, 50),
                      "disabled slot should reject update")) return rc;
  if (int rc = Expect(!store.MarkUnavailable(id),
                      "disabled slot should reject unavailable mark")) return rc;
  return Expect(SnapshotsEqual(store.ReadSnapshot(id).value(), initial),
                "disabled slot should remain unchanged");
}

int TestInvalidIds() {
  opcua::RealtimeValueStore store;
  store.AddSlot(opcua::ScalarType::kBoolean);
  constexpr opcua::ValueSlotId invalid_id = 99;

  if (int rc = Expect(!store.ReadSnapshot(invalid_id).ok(),
                      "invalid read should return error")) return rc;
  if (int rc = Expect(!store.Update(invalid_id, true, 1),
                      "invalid update should return false")) return rc;
  return Expect(!store.MarkUnavailable(invalid_id),
                "invalid unavailable mark should return false");
}

int TestUnavailableBeforeValueAndRepeatedUnavailable() {
  opcua::RealtimeValueStore store;
  const auto waiting_id = store.AddSlot(opcua::ScalarType::kInt64);
  if (int rc = Expect(!store.MarkUnavailable(waiting_id),
                      "waiting slot unavailable should not change quality")) return rc;
  const auto waiting = store.ReadSnapshot(waiting_id).value();
  if (int rc = Expect(waiting.status == UA_STATUSCODE_BADWAITINGFORINITIALDATA &&
                          !waiting.has_value && waiting.sequence == 0,
                      "waiting slot should remain waiting")) return rc;

  const auto valued_id = store.AddSlot(opcua::ScalarType::kBoolean);
  store.Update(valued_id, true, 25);
  if (int rc = Expect(store.MarkUnavailable(valued_id),
                      "first unavailable should change quality")) return rc;
  const auto unavailable = store.ReadSnapshot(valued_id).value();
  if (int rc = Expect(!store.MarkUnavailable(valued_id),
                      "repeated unavailable should not change quality")) return rc;
  return Expect(SnapshotsEqual(store.ReadSnapshot(valued_id).value(), unavailable),
                "repeated unavailable should preserve snapshot");
}

int TestSourceHealth() {
  opcua::RealtimeValueStore store;
  auto health = store.ReadSourceHealth();
  if (int rc = Expect(
          health.connection_state == opcua::SourceConnectionState::kConnecting &&
              health.last_successful_update == 0 && health.consecutive_failures == 0,
          "initial source health mismatch")) return rc;

  store.SetSourceDisconnected();
  health = store.ReadSourceHealth();
  if (int rc = Expect(
          health.connection_state == opcua::SourceConnectionState::kDisconnected &&
              health.consecutive_failures == 1,
          "first disconnect should increment failures")) return rc;
  store.SetSourceDisconnected();
  if (int rc = Expect(store.ReadSourceHealth().consecutive_failures == 1,
                      "repeated disconnect should not increment failures")) return rc;

  store.SetSourceConnected();
  health = store.ReadSourceHealth();
  if (int rc = Expect(
          health.connection_state == opcua::SourceConnectionState::kConnected &&
              health.consecutive_failures == 0 && health.last_successful_update == 0,
          "connect should reset failures without timestamp")) return rc;

  store.SetSourceDisconnected();
  if (int rc = Expect(store.ReadSourceHealth().consecutive_failures == 1,
                      "disconnect after connect should increment failures")) return rc;

  const auto id = store.AddSlot(opcua::ScalarType::kDouble);
  if (int rc = Expect(store.Update(id, 4.0, 1234),
                      "health test update should change slot")) return rc;
  health = store.ReadSourceHealth();
  if (int rc = Expect(health.last_successful_update == 1234 &&
                          health.consecutive_failures == 0,
                      "accepted update should refresh health")) return rc;

  store.SetSourceDisconnected();
  if (int rc = Expect(!store.Update(id, 4.0, 2345),
                      "same value update should not change slot")) return rc;
  health = store.ReadSourceHealth();
  return Expect(health.last_successful_update == 2345 &&
                    health.consecutive_failures == 0,
                "accepted no-change update should refresh health");
}

int TestDisabledSourceHealth() {
  opcua::RealtimeValueStore store;
  const auto id = store.AddSlot(opcua::ScalarType::kDouble, false);

  store.SetSourceDisconnected();
  if (int rc = Expect(store.ReadSourceHealth().consecutive_failures == 1,
                      "setup disconnect should increment failures")) return rc;

  store.SetSourceDisabled();
  const auto health = store.ReadSourceHealth();
  if (int rc = Expect(
          health.connection_state == opcua::SourceConnectionState::kDisabled,
          "disabled source state mismatch")) return rc;
  if (int rc = Expect(health.consecutive_failures == 0,
                      "disabled source should reset failures")) return rc;
  if (int rc = Expect(health.last_successful_update == 0,
                      "disabled source should not invent an update timestamp")) {
    return rc;
  }

  return Expect(!store.Update(id, 12.0, 50),
                "disabled slot should still reject updates");
}

int TestConcurrentReadAndWrite() {
  opcua::RealtimeValueStore store;
  const auto id = store.AddSlot(opcua::ScalarType::kDouble);
  std::atomic_bool writer_done{false};
  std::atomic_int failures{0};

  std::thread writer([&] {
    for (std::int64_t i = 0; i < 10000; ++i) {
      store.Update(id, static_cast<double>(i), static_cast<UA_DateTime>(i));
    }
    writer_done.store(true);
  });

  std::thread reader([&] {
    do {
      const auto snapshot = store.ReadSnapshot(id);
      if (!snapshot.ok() || snapshot.value().type != opcua::ScalarType::kDouble ||
          !std::holds_alternative<double>(snapshot.value().value)) {
        failures.fetch_add(1);
      }
    } while (!writer_done.load());
  });

  writer.join();
  reader.join();
  return Expect(failures.load() == 0,
                "concurrent snapshots should remain valid and typed");
}

int TestConcurrentWritersKeepValueAndHealthTimestampCorrelated() {
  constexpr int kRounds = 100;
  constexpr int kWriterCount = 8;
  constexpr int kUpdatesPerWriter = 500;

  for (int round = 0; round < kRounds; ++round) {
    opcua::RealtimeValueStore store;
    const auto id = store.AddSlot(opcua::ScalarType::kDouble);
    std::atomic_bool start{false};
    std::atomic_int ready{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);

    for (int writer = 0; writer < kWriterCount; ++writer) {
      writers.emplace_back([&, writer] {
        ready.fetch_add(1);
        while (!start.load()) {
          std::this_thread::yield();
        }
        for (int update = 0; update < kUpdatesPerWriter; ++update) {
          const auto timestamp = static_cast<UA_DateTime>(
              (round + 1) * 1000000 + writer * kUpdatesPerWriter + update + 1);
          store.Update(id, static_cast<double>(timestamp), timestamp);
        }
      });
    }

    while (ready.load() != kWriterCount) {
      std::this_thread::yield();
    }
    start.store(true);
    for (auto& writer : writers) {
      writer.join();
    }

    const auto snapshot = store.ReadSnapshot(id);
    const auto health = store.ReadSourceHealth();
    if (!snapshot.ok() ||
        std::get<double>(snapshot.value().value) !=
            static_cast<double>(health.last_successful_update) ||
        snapshot.value().source_timestamp != health.last_successful_update) {
      return Expect(false,
                    "final slot value and health timestamp should correspond");
    }
  }

  return 0;
}

}  // namespace

int main() {
  if (int rc = TestSlotIdsAreDeterministicAndDistinct()) return rc;
  if (int rc = TestDoubleLifecycle()) return rc;
  if (int rc = TestConcurrentWritersKeepValueAndHealthTimestampCorrelated()) return rc;
  if (int rc = TestSignedZerosAreSemanticallyEqualAndPreserveRepresentation()) return rc;
  if (int rc = TestRepeatedNanIsSemanticallyEqual()) return rc;
  if (int rc = TestBooleanAndInt64Slots()) return rc;
  if (int rc = TestTypeMismatchesLeaveSnapshotsUnchanged()) return rc;
  if (int rc = TestDisabledSlotRemainsOutOfService()) return rc;
  if (int rc = TestInvalidIds()) return rc;
  if (int rc = TestUnavailableBeforeValueAndRepeatedUnavailable()) return rc;
  if (int rc = TestSourceHealth()) return rc;
  if (int rc = TestDisabledSourceHealth()) return rc;
  if (int rc = TestConcurrentReadAndWrite()) return rc;
  return 0;
}
