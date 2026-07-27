#ifndef OPCUA_SERVER_SRC_DAEMON_REALTIME_VALUE_STORE_H_
#define OPCUA_SERVER_SRC_DAEMON_REALTIME_VALUE_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4819)
#endif
#include "open62541.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "common/result.h"

namespace opcua {

enum class ScalarType { kBoolean, kInt64, kDouble };

using ScalarValue = std::variant<bool, std::int64_t, double>;
using ValueSlotId = std::size_t;

struct ValueSnapshot {
  ScalarType type;
  ScalarValue value;
  UA_StatusCode status;
  UA_DateTime source_timestamp;
  std::uint64_t sequence;
  bool has_value;
};

enum class SourceConnectionState {
  kDisabled,
  kConnecting,
  kConnected,
  kDisconnected,
};

struct SourceHealthSnapshot {
  SourceConnectionState connection_state;
  UA_DateTime last_successful_update;
  std::uint32_t consecutive_failures;
};

class RealtimeValueStore {
 public:
  RealtimeValueStore();
  ~RealtimeValueStore();

  RealtimeValueStore(const RealtimeValueStore&) = delete;
  RealtimeValueStore& operator=(const RealtimeValueStore&) = delete;

  // Add slots during startup before concurrent ReadSnapshot/Update/MarkUnavailable
  // access begins.
  ValueSlotId AddSlot(ScalarType type, bool enabled = true);
  bool Update(ValueSlotId id, ScalarValue value, UA_DateTime timestamp);
  bool MarkUnavailable(ValueSlotId id);
  Result<ValueSnapshot> ReadSnapshot(ValueSlotId id) const;

  void SetSourceConnected();
  void SetSourceDisconnected();
  SourceHealthSnapshot ReadSourceHealth() const;

 private:
  struct ValueSlot;
  std::vector<std::unique_ptr<ValueSlot>> slots_;

  mutable std::mutex source_health_mutex_;
  // The enabled MVP source begins connecting before its adapter starts.
  SourceHealthSnapshot source_health_{SourceConnectionState::kConnecting, 0, 0};
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_REALTIME_VALUE_STORE_H_
