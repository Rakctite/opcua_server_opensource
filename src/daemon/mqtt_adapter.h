#ifndef OPCUA_SERVER_SRC_DAEMON_MQTT_ADAPTER_H_
#define OPCUA_SERVER_SRC_DAEMON_MQTT_ADAPTER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <thread>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201 4819)
#endif
#include "MQTTAsync.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include "common/result.h"
#include "config/mqtt_config.h"
#include "daemon/realtime_value_store.h"

namespace opcua {

class MqttAdapter {
 public:
  MqttAdapter(MqttConfig config, ScalarType type, RealtimeValueStore* store,
              ValueSlotId slot);
  ~MqttAdapter();

  MqttAdapter(const MqttAdapter&) = delete;
  MqttAdapter& operator=(const MqttAdapter&) = delete;

  Status Start();
  void Stop();
  Status AcceptMessage(std::string_view topic, std::string_view payload,
                       UA_DateTime source_timestamp);
  void NotifyConnectionLost();
  void PollHealth(std::chrono::steady_clock::time_point now);

 private:
  static void Connected(void* context, char* cause);
  static void ConnectionLost(void* context, char* cause);
  static int MessageArrived(void* context, char* topic_name, int topic_length,
                            MQTTAsync_message* message);

  bool EnterCallback();
  void LeaveCallback();
  void WatchdogLoop();
  void ReportParseFailure();

  MqttConfig config_;
  ScalarType type_;
  RealtimeValueStore* store_;
  ValueSlotId slot_;
  MQTTAsync client_ = nullptr;

  std::atomic_bool accepting_{false};

  std::mutex health_mutex_;
  std::condition_variable watchdog_wakeup_;
  bool watchdog_stop_ = false;
  std::thread watchdog_thread_;
  std::chrono::steady_clock::time_point last_valid_message_{};

  std::mutex callback_mutex_;
  std::condition_variable callback_drained_;
  int active_callbacks_ = 0;

  std::atomic_uint32_t parse_failures_{0};
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_MQTT_ADAPTER_H_
