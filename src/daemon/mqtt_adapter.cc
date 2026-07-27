#include "daemon/mqtt_adapter.h"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "daemon/mqtt_payload_parser.h"

namespace opcua {
namespace {

constexpr int kMqttQos = 1;
constexpr std::uint32_t kParseFailureLogInterval = 100;

Status PahoError(const char* operation, int rc) {
  return Status::Error(std::string(operation) + " failed with MQTTAsync rc=" +
                       std::to_string(rc));
}

int FailureCode(const MQTTAsync_failureData* response) {
  return response == nullptr ? 0 : response->code;
}

std::string_view TopicView(char* topic_name, int topic_length) {
  if (topic_name == nullptr) {
    return {};
  }
  if (topic_length > 0) {
    return std::string_view(topic_name, static_cast<std::size_t>(topic_length));
  }
  return std::string_view(topic_name, std::strlen(topic_name));
}

std::string_view PayloadView(MQTTAsync_message* message) {
  if (message == nullptr || message->payload == nullptr ||
      message->payloadlen < 0) {
    return {};
  }
  return std::string_view(static_cast<const char*>(message->payload),
                          static_cast<std::size_t>(message->payloadlen));
}

bool IsParseFailure(const Status& status) {
  return status.message() == "invalid MQTT scalar payload";
}

}  // namespace

MqttAdapter::MqttAdapter(MqttConfig config, ScalarType type,
                         RealtimeValueStore* store, ValueSlotId slot)
    : config_(std::move(config)), type_(type), store_(store), slot_(slot) {}

MqttAdapter::~MqttAdapter() { Stop(); }

Status MqttAdapter::Start() {
  if (!config_.enabled) {
    return Status::Ok();
  }
  if (store_ == nullptr) {
    return Status::Error("MQTT adapter requires a value store");
  }
  if (client_ != nullptr) {
    return Status::Ok();
  }

  Status validation = config_.Validate();
  if (!validation.ok()) {
    return validation;
  }

  MQTTAsync created_client = nullptr;
  int rc = MQTTAsync_create(&created_client, config_.broker_uri.c_str(),
                            config_.client_id.c_str(),
                            MQTTCLIENT_PERSISTENCE_NONE, nullptr);
  if (rc != MQTTASYNC_SUCCESS) {
    return PahoError("MQTTAsync_create", rc);
  }
  client_ = created_client;

  rc = MQTTAsync_setCallbacks(client_, this, &MqttAdapter::ConnectionLost,
                              &MqttAdapter::MessageArrived, nullptr);
  if (rc != MQTTASYNC_SUCCESS) {
    Stop();
    return PahoError("MQTTAsync_setCallbacks", rc);
  }

  rc = MQTTAsync_setConnected(client_, this, &MqttAdapter::Connected);
  if (rc != MQTTASYNC_SUCCESS) {
    Stop();
    return PahoError("MQTTAsync_setConnected", rc);
  }

  MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
  options.cleansession = 1;
  options.automaticReconnect = 1;
  options.minRetryInterval = 1;
  options.maxRetryInterval = 60;
  options.context = this;
  options.onSuccess = &MqttAdapter::ConnectSucceeded;
  options.onFailure = &MqttAdapter::ConnectFailed;

  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    watchdog_stop_ = false;
  }
  skip_next_connected_subscribe_.store(false);
  accepting_.store(true);

  rc = MQTTAsync_connect(client_, &options);
  if (rc != MQTTASYNC_SUCCESS) {
    Stop();
    return PahoError("MQTTAsync_connect", rc);
  }

  watchdog_thread_ = std::thread(&MqttAdapter::WatchdogLoop, this);
  return Status::Ok();
}

void MqttAdapter::Stop() {
  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    watchdog_stop_ = true;
  }
  watchdog_wakeup_.notify_all();
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }

  if (client_ != nullptr) {
    int rc = MQTTAsync_setCallbacks(client_, nullptr,
                                    &MqttAdapter::NoopConnectionLost,
                                    &MqttAdapter::NoopMessageArrived, nullptr);
    if (rc != MQTTASYNC_SUCCESS) {
      std::cerr << "MQTT callback teardown failed with MQTTAsync rc=" << rc
                << "\n";
    }
    rc = MQTTAsync_setConnected(client_, nullptr, &MqttAdapter::NoopConnected);
    if (rc != MQTTASYNC_SUCCESS) {
      std::cerr << "MQTT connected callback teardown failed with MQTTAsync rc="
                << rc << "\n";
    }
  }

  accepting_.store(false);

  {
    std::unique_lock<std::mutex> lock(callback_mutex_);
    callback_drained_.wait(lock, [this] { return active_callbacks_ == 0; });
  }

  if (client_ != nullptr) {
    if (MQTTAsync_isConnected(client_)) {
      MQTTAsync_disconnectOptions options =
          MQTTAsync_disconnectOptions_initializer;
      const int rc = MQTTAsync_disconnect(client_, &options);
      if (rc != MQTTASYNC_SUCCESS) {
        std::cerr << "MQTT disconnect failed with MQTTAsync rc=" << rc << "\n";
      }
    }
    MQTTAsync_destroy(&client_);
    client_ = nullptr;
  }
}

Status MqttAdapter::AcceptMessage(std::string_view topic,
                                  std::string_view payload,
                                  UA_DateTime source_timestamp) {
  if (topic != config_.topic) {
    return Status::Error("MQTT message topic does not match configured topic");
  }
  if (store_ == nullptr) {
    return Status::Error("MQTT adapter requires a value store");
  }

  auto value = ParseMqttScalar(payload, type_);
  if (!value.ok()) {
    ReportParseFailure();
    return value.status();
  }

  if (!store_->Update(slot_, value.value(), source_timestamp)) {
    const auto snapshot = store_->ReadSnapshot(slot_);
    if (!snapshot.ok()) {
      return snapshot.status();
    }
    const auto& current = snapshot.value();
    const bool accepted_no_change =
        current.has_value && current.status == UA_STATUSCODE_GOOD &&
        current.type == type_ && current.value == value.value();
    if (!accepted_no_change &&
        current.status == UA_STATUSCODE_BADOUTOFSERVICE) {
      return Status::Error("MQTT value store update rejected: slot disabled");
    }
    if (!accepted_no_change && current.type != type_) {
      return Status::Error(
          "MQTT value store update rejected: value type does not match slot");
    }
    if (!accepted_no_change) {
      return Status::Error("MQTT value store update rejected");
    }
  }
  store_->SetSourceConnected();

  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    last_valid_message_ = std::chrono::steady_clock::now();
  }
  watchdog_wakeup_.notify_all();
  parse_failures_.store(0);
  return Status::Ok();
}

void MqttAdapter::NotifyConnectionLost() {
  if (store_ == nullptr) {
    return;
  }
  store_->SetSourceDisconnected();
  store_->MarkUnavailable(slot_);
}

void MqttAdapter::PollHealth(std::chrono::steady_clock::time_point now) {
  std::chrono::steady_clock::time_point last_valid_message;
  {
    std::lock_guard<std::mutex> lock(health_mutex_);
    last_valid_message = last_valid_message_;
  }

  if (last_valid_message == std::chrono::steady_clock::time_point{}) {
    return;
  }

  const auto stale_timeout =
      std::chrono::milliseconds(config_.stale_timeout_ms);
  if (now - last_valid_message >= stale_timeout) {
    NotifyConnectionLost();
  }
}

void MqttAdapter::Connected(void* context, char* /*cause*/) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }

  if (!adapter->skip_next_connected_subscribe_.exchange(false)) {
    adapter->StartSubscribe();
  }
  adapter->LeaveCallback();
}

void MqttAdapter::ConnectionLost(void* context, char* /*cause*/) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }
  adapter->NotifyConnectionLost();
  adapter->LeaveCallback();
}

int MqttAdapter::MessageArrived(void* context, char* topic_name,
                                int topic_length,
                                MQTTAsync_message* message) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter != nullptr && adapter->EnterCallback()) {
    const Status status =
        adapter->AcceptMessage(TopicView(topic_name, topic_length),
                               PayloadView(message), UA_DateTime_now());
    if (!status.ok() && !IsParseFailure(status)) {
      std::cerr << "MQTT message rejected: " << status.message() << "\n";
    }
    adapter->LeaveCallback();
  }

  MQTTAsync_freeMessage(&message);
  MQTTAsync_free(topic_name);
  return 1;
}

void MqttAdapter::ConnectSucceeded(void* context,
                                   MQTTAsync_successData* /*response*/) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }
  adapter->skip_next_connected_subscribe_.store(true);
  adapter->StartSubscribe();
  adapter->LeaveCallback();
}

void MqttAdapter::ConnectFailed(void* context, MQTTAsync_failureData* response) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }
  adapter->NotifyConnectionLost();
  std::cerr << "MQTT connect failed with MQTTAsync rc=" << FailureCode(response)
            << "\n";
  adapter->LeaveCallback();
}

void MqttAdapter::SubscribeSucceeded(void* context,
                                     MQTTAsync_successData* /*response*/) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }
  adapter->store_->SetSourceConnected();
  adapter->LeaveCallback();
}

void MqttAdapter::SubscribeFailed(void* context,
                                  MQTTAsync_failureData* response) {
  MqttAdapter* adapter = static_cast<MqttAdapter*>(context);
  if (adapter == nullptr || !adapter->EnterCallback()) {
    return;
  }
  adapter->NotifyConnectionLost();
  std::cerr << "MQTT subscribe failed with MQTTAsync rc="
            << FailureCode(response) << "\n";
  adapter->LeaveCallback();
}

void MqttAdapter::NoopConnected(void*, char*) {}

void MqttAdapter::NoopConnectionLost(void*, char*) {}

int MqttAdapter::NoopMessageArrived(void*, char* topic_name, int,
                                    MQTTAsync_message* message) {
  MQTTAsync_freeMessage(&message);
  MQTTAsync_free(topic_name);
  return 1;
}

bool MqttAdapter::EnterCallback() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!accepting_.load()) {
    return false;
  }
  ++active_callbacks_;
  return true;
}

void MqttAdapter::LeaveCallback() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  --active_callbacks_;
  if (active_callbacks_ == 0) {
    callback_drained_.notify_all();
  }
}

void MqttAdapter::StartSubscribe() {
  ++subscribe_attempts_for_test_;

  MQTTAsync_responseOptions options = MQTTAsync_responseOptions_initializer;
  options.context = this;
  options.onSuccess = &MqttAdapter::SubscribeSucceeded;
  options.onFailure = &MqttAdapter::SubscribeFailed;

  const int rc = MQTTAsync_subscribe(client_, config_.topic.c_str(), kMqttQos,
                                     &options);
  if (rc != MQTTASYNC_SUCCESS) {
    NotifyConnectionLost();
    std::cerr << "MQTT subscribe failed with MQTTAsync rc=" << rc << "\n";
  }
}

void MqttAdapter::WatchdogLoop() {
  std::unique_lock<std::mutex> lock(health_mutex_);
  while (!watchdog_stop_) {
    const auto timeout = std::chrono::milliseconds(config_.stale_timeout_ms);
    watchdog_wakeup_.wait_for(lock, timeout,
                              [this] { return watchdog_stop_; });
    if (watchdog_stop_) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    lock.unlock();
    PollHealth(now);
    lock.lock();
  }
}

void MqttAdapter::ReportParseFailure() {
  const std::uint32_t failures = parse_failures_.fetch_add(1) + 1;
  if (failures == 1 || failures % kParseFailureLogInterval == 0) {
    std::cerr << "MQTT payload parse failures: " << failures << "\n";
  }
}

}  // namespace opcua
