#include "daemon/realtime_value_store.h"

#include <cmath>
#include <utility>

namespace opcua {

namespace {

ScalarValue DefaultValue(ScalarType type) {
  switch (type) {
    case ScalarType::kBoolean:
      return false;
    case ScalarType::kInt64:
      return std::int64_t{0};
    case ScalarType::kDouble:
      return 0.0;
  }
  return false;
}

bool ValueMatchesType(ScalarType type, const ScalarValue& value) {
  switch (type) {
    case ScalarType::kBoolean:
      return std::holds_alternative<bool>(value);
    case ScalarType::kInt64:
      return std::holds_alternative<std::int64_t>(value);
    case ScalarType::kDouble:
      return std::holds_alternative<double>(value);
  }
  return false;
}

bool ScalarValuesEqual(const ScalarValue& lhs, const ScalarValue& rhs) {
  if (lhs.index() != rhs.index()) {
    return false;
  }
  if (std::holds_alternative<bool>(lhs)) {
    return std::get<bool>(lhs) == std::get<bool>(rhs);
  }
  if (std::holds_alternative<std::int64_t>(lhs)) {
    return std::get<std::int64_t>(lhs) == std::get<std::int64_t>(rhs);
  }

  const double lhs_double = std::get<double>(lhs);
  const double rhs_double = std::get<double>(rhs);
  return lhs_double == rhs_double ||
         (std::isnan(lhs_double) && std::isnan(rhs_double));
}

}  // namespace

struct RealtimeValueStore::ValueSlot {
  ValueSlot(ScalarType type, bool enabled)
      : snapshot{type,
                 DefaultValue(type),
                 enabled ? UA_STATUSCODE_BADWAITINGFORINITIALDATA
                         : UA_STATUSCODE_BADOUTOFSERVICE,
                 0,
                 0,
                 false},
        enabled(enabled) {}

  mutable std::mutex mutex;
  ValueSnapshot snapshot;
  bool enabled;
};

RealtimeValueStore::RealtimeValueStore() = default;

RealtimeValueStore::~RealtimeValueStore() = default;

ValueSlotId RealtimeValueStore::AddSlot(ScalarType type, bool enabled) {
  const ValueSlotId id = slots_.size();
  slots_.push_back(std::make_unique<ValueSlot>(type, enabled));
  return id;
}

bool RealtimeValueStore::Update(ValueSlotId id, ScalarValue value,
                                UA_DateTime timestamp) {
  if (id >= slots_.size()) {
    return false;
  }

  ValueSlot& slot = *slots_[id];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!slot.enabled || !ValueMatchesType(slot.snapshot.type, value)) {
    return false;
  }

  const bool value_changed =
      !slot.snapshot.has_value || !ScalarValuesEqual(slot.snapshot.value, value);
  const bool changed =
      value_changed || slot.snapshot.status != UA_STATUSCODE_GOOD;
  if (value_changed) {
    slot.snapshot.value = std::move(value);
  }
  slot.snapshot.status = UA_STATUSCODE_GOOD;
  slot.snapshot.source_timestamp = timestamp;
  slot.snapshot.has_value = true;
  if (changed) {
    ++slot.snapshot.sequence;
  }

  std::lock_guard<std::mutex> health_lock(source_health_mutex_);
  source_health_.last_successful_update = timestamp;
  source_health_.consecutive_failures = 0;
  return changed;
}

bool RealtimeValueStore::MarkUnavailable(ValueSlotId id) {
  if (id >= slots_.size()) {
    return false;
  }

  ValueSlot& slot = *slots_[id];
  std::lock_guard<std::mutex> lock(slot.mutex);
  if (!slot.enabled || !slot.snapshot.has_value) {
    return false;
  }

  constexpr UA_StatusCode unavailable_status =
      UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE;
  if (slot.snapshot.status == unavailable_status) {
    return false;
  }

  slot.snapshot.status = unavailable_status;
  ++slot.snapshot.sequence;
  return true;
}

Result<ValueSnapshot> RealtimeValueStore::ReadSnapshot(ValueSlotId id) const {
  if (id >= slots_.size()) {
    return Result<ValueSnapshot>::Error(Status::Error("invalid value slot ID"));
  }

  const ValueSlot& slot = *slots_[id];
  std::lock_guard<std::mutex> lock(slot.mutex);
  return slot.snapshot;
}

void RealtimeValueStore::SetSourceConnected() {
  std::lock_guard<std::mutex> lock(source_health_mutex_);
  source_health_.connection_state = SourceConnectionState::kConnected;
  source_health_.consecutive_failures = 0;
}

void RealtimeValueStore::SetSourceDisconnected() {
  std::lock_guard<std::mutex> lock(source_health_mutex_);
  if (source_health_.connection_state != SourceConnectionState::kDisconnected) {
    source_health_.connection_state = SourceConnectionState::kDisconnected;
    ++source_health_.consecutive_failures;
  }
}

void RealtimeValueStore::SetSourceDisabled() {
  std::lock_guard<std::mutex> lock(source_health_mutex_);
  source_health_.connection_state = SourceConnectionState::kDisabled;
  source_health_.consecutive_failures = 0;
}

SourceHealthSnapshot RealtimeValueStore::ReadSourceHealth() const {
  std::lock_guard<std::mutex> lock(source_health_mutex_);
  return source_health_;
}

}  // namespace opcua
