#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4819)
#endif
#include "open62541.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201 4819)
#endif
#include "MQTTAsync.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "config/config_repository.h"
#include "config/mqtt_config.h"
#include "config/server_config.h"
#include "supervisor/process_controller.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kFirstMqttPort = 18883;
constexpr int kFirstOpcuaPort = 48450;
constexpr int kPortCount = 10;
constexpr std::uint32_t kDataNodeId = 1001;
constexpr char kTopic[] = "opcua/integration/value";
constexpr UA_StatusCode kUnavailableStatus =
    UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE;

opcua::Status Error(std::string message) {
  return opcua::Status::Error(std::move(message));
}

std::string StatusName(UA_StatusCode code) {
  return UA_StatusCode_name(code);
}

std::string MqttRc(const char* operation, int rc) {
  std::ostringstream stream;
  stream << operation << " failed with MQTTAsync rc=" << rc;
  return stream.str();
}

std::string DeadlineMessage(const char* operation) {
  std::ostringstream stream;
  stream << operation << " timed out";
  return stream.str();
}

void SleepUntilNextPoll(Clock::time_point deadline,
                        std::chrono::milliseconds interval) {
  const auto now = Clock::now();
  if (now >= deadline) {
    return;
  }
  const auto wake_at = std::min(now + interval, deadline);
  std::this_thread::sleep_until(wake_at);
}

class SocketRuntime {
 public:
  SocketRuntime() {
#if defined(_WIN32)
    WSADATA data;
    ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
  }

  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;

  ~SocketRuntime() {
#if defined(_WIN32)
    if (ok_) {
      WSACleanup();
    }
#endif
  }

  bool ok() const { return ok_; }

 private:
  bool ok_ = true;
};

bool PortCanBind(int port) {
#if defined(_WIN32)
  SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == INVALID_SOCKET) {
    return false;
  }
#else
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
#endif

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok =
      bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;

#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
  return ok;
}

void RemoveDatabaseFiles(const std::filesystem::path& db_path) {
  std::error_code ignored;
  std::filesystem::remove(db_path, ignored);
  std::filesystem::remove(db_path.string() + "-wal", ignored);
  std::filesystem::remove(db_path.string() + "-shm", ignored);
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto stamp = Clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("opcua_mqtt_integration_test_" + std::to_string(stamp));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class AsyncOperation {
 public:
  AsyncOperation() = default;

  AsyncOperation(const AsyncOperation&) = delete;
  AsyncOperation& operator=(const AsyncOperation&) = delete;

  static void OnSuccess(void* context, MQTTAsync_successData* /*response*/) {
    static_cast<AsyncOperation*>(context)->Complete(true, MQTTASYNC_SUCCESS);
  }

  static void OnFailure(void* context, MQTTAsync_failureData* response) {
    static_cast<AsyncOperation*>(context)->Complete(
        false, response == nullptr ? MQTTASYNC_FAILURE : response->code);
  }

  opcua::Status WaitUntil(Clock::time_point deadline, const char* operation) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!done_) {
      condition_.wait_until(lock, deadline, [this] { return done_; });
    }
    if (!done_) {
      return Error(DeadlineMessage(operation));
    }
    if (!succeeded_) {
      return Error(MqttRc(operation, rc_));
    }
    return opcua::Status::Ok();
  }

 private:
  void Complete(bool succeeded, int rc) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
      succeeded_ = succeeded;
      rc_ = rc;
    }
    condition_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  bool done_ = false;
  bool succeeded_ = false;
  int rc_ = MQTTASYNC_FAILURE;
};

class MqttClient {
 public:
  MqttClient() = default;

  MqttClient(const MqttClient&) = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  ~MqttClient() {
    if (client_ != nullptr) {
      if (MQTTAsync_isConnected(client_)) {
        MQTTAsync_disconnectOptions options =
            MQTTAsync_disconnectOptions_initializer;
        options.timeout = 100;
        (void)MQTTAsync_disconnect(client_, &options);
      }
      MQTTAsync_destroy(&client_);
    }
  }

  opcua::Status Connect(const std::string& broker_uri,
                        const std::string& client_id,
                        Clock::time_point deadline) {
    if (client_ != nullptr) {
      return Error("MQTT client already created");
    }
    int rc = MQTTAsync_create(&client_, broker_uri.c_str(), client_id.c_str(),
                              MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (rc != MQTTASYNC_SUCCESS) {
      return Error(MqttRc("MQTTAsync_create", rc));
    }

    AsyncOperation operation;
    MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
    options.cleansession = 1;
    options.keepAliveInterval = 5;
    options.connectTimeout = 2;
    options.context = &operation;
    options.onSuccess = &AsyncOperation::OnSuccess;
    options.onFailure = &AsyncOperation::OnFailure;
    rc = MQTTAsync_connect(client_, &options);
    if (rc != MQTTASYNC_SUCCESS) {
      return Error(MqttRc("MQTTAsync_connect", rc));
    }
    return operation.WaitUntil(deadline, "MQTT connect");
  }

  opcua::Status Publish(const std::string& topic, const std::string& payload,
                        Clock::time_point deadline) {
    if (client_ == nullptr || !MQTTAsync_isConnected(client_)) {
      return Error("MQTT client is not connected");
    }

    MQTTAsync_message message = MQTTAsync_message_initializer;
    message.payload = const_cast<char*>(payload.data());
    message.payloadlen = static_cast<int>(payload.size());
    message.qos = 1;
    message.retained = 0;

    AsyncOperation operation;
    MQTTAsync_responseOptions options = MQTTAsync_responseOptions_initializer;
    options.context = &operation;
    options.onSuccess = &AsyncOperation::OnSuccess;
    options.onFailure = &AsyncOperation::OnFailure;
    const int rc =
        MQTTAsync_sendMessage(client_, topic.c_str(), &message, &options);
    if (rc != MQTTASYNC_SUCCESS) {
      return Error(MqttRc("MQTTAsync_sendMessage", rc));
    }
    return operation.WaitUntil(deadline, "MQTT publish");
  }

  opcua::Status Disconnect(Clock::time_point deadline) {
    if (client_ == nullptr || !MQTTAsync_isConnected(client_)) {
      return opcua::Status::Ok();
    }

    AsyncOperation operation;
    MQTTAsync_disconnectOptions options =
        MQTTAsync_disconnectOptions_initializer;
    options.timeout = 1000;
    options.context = &operation;
    options.onSuccess = &AsyncOperation::OnSuccess;
    options.onFailure = &AsyncOperation::OnFailure;
    const int rc = MQTTAsync_disconnect(client_, &options);
    if (rc != MQTTASYNC_SUCCESS) {
      return Error(MqttRc("MQTTAsync_disconnect", rc));
    }
    return operation.WaitUntil(deadline, "MQTT disconnect");
  }

 private:
  MQTTAsync client_ = nullptr;
};

struct DataObservation {
  UA_StatusCode status = UA_STATUSCODE_BADINTERNALERROR;
  bool has_value = false;
  double value = 0.0;
  bool has_source_timestamp = false;
  UA_DateTime source_timestamp = 0;
};

struct Notification {
  UA_StatusCode status = UA_STATUSCODE_BADINTERNALERROR;
  bool has_value = false;
  double value = 0.0;
};

struct ClientDeleter {
  void operator()(UA_Client* client) const {
    if (client != nullptr) {
      UA_Client_disconnect(client);
      UA_Client_delete(client);
    }
  }
};

using ClientPtr = std::unique_ptr<UA_Client, ClientDeleter>;

class IntegrationFixture {
 public:
  static opcua::Result<std::unique_ptr<IntegrationFixture>> Create(
      std::string mosquitto_path, std::string daemon_path) {
    if (!std::filesystem::exists(mosquitto_path)) {
      return Error("Mosquitto executable does not exist: " + mosquitto_path);
    }
    if (!std::filesystem::exists(daemon_path)) {
      return Error("opcua-daemon executable does not exist: " + daemon_path);
    }

    SocketRuntime sockets;
    if (!sockets.ok()) {
      return Error("socket runtime initialization failed");
    }

    for (int offset = 0; offset < kPortCount; ++offset) {
      const int mqtt_port = kFirstMqttPort + offset;
      const int opcua_port = kFirstOpcuaPort + offset;
      if (PortCanBind(mqtt_port) && PortCanBind(opcua_port)) {
        return std::unique_ptr<IntegrationFixture>(
            new IntegrationFixture(std::move(mosquitto_path),
                                   std::move(daemon_path), mqtt_port,
                                   opcua_port));
      }
    }
    return Error("no free MQTT/OPC UA port pair in 18883..18892 and 48450..48459");
  }

  IntegrationFixture(const IntegrationFixture&) = delete;
  IntegrationFixture& operator=(const IntegrationFixture&) = delete;

  ~IntegrationFixture() {
    StopAll();
    RemoveDatabaseFiles(db_path_);
  }

  opcua::Status StartBroker() {
    WriteMosquittoConfig();
    broker_controller_ = std::make_unique<opcua::ProcessController>(
        mosquitto_path_, std::vector<std::string>{"-c", config_path_.string()});
    auto start_status = broker_controller_->Start();
    if (!start_status.ok()) {
      return start_status;
    }

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      auto status = ProbeBroker(deadline);
      if (status.ok()) {
        return status;
      }
      const auto broker_status = broker_controller_->status();
      if (broker_status.state != opcua::ProcessState::kRunning) {
        return Error("Mosquitto exited during startup: " +
                     broker_status.diagnostic);
      }
      SleepUntilNextPoll(deadline, std::chrono::milliseconds(100));
    }
    return Error(DeadlineMessage("Mosquitto startup"));
  }

  opcua::Status ConfigureDatabase() {
    auto repository_result = opcua::ConfigRepository::Open(db_path_.string());
    if (!repository_result.ok()) {
      return repository_result.status();
    }
    auto& repository = repository_result.value();

    auto initialize_status = repository.Initialize();
    if (!initialize_status.ok()) {
      return initialize_status;
    }

    opcua::ServerConfig server_config = opcua::ServerConfig::Default();
    server_config.server_bind_address = "127.0.0.1";
    server_config.server_port = opcua_port_;
    auto save_status = repository.Save(server_config);
    if (!save_status.ok()) {
      return save_status;
    }

    opcua::MqttConfig mqtt_config{true,
                                  BrokerUri(),
                                  ClientId("daemon"),
                                  kTopic,
                                  1,
                                  kDataNodeId,
                                  "IntegrationValue",
                                  "double",
                                  1000};
    return repository.SaveMqtt(mqtt_config);
  }

  opcua::Status StartDaemon() {
    daemon_controller_ = std::make_unique<opcua::ProcessController>(
        daemon_path_, std::vector<std::string>{db_path_.string()});
    auto start_status = daemon_controller_->Start();
    if (!start_status.ok()) {
      return start_status;
    }

    auto connect_status =
        ConnectOpcuaClient(Clock::now() + std::chrono::seconds(8));
    if (!connect_status.ok()) {
      return connect_status;
    }
    return CreateSubscription();
  }

  opcua::Status PublishDouble(double value) {
    std::ostringstream payload;
    payload.precision(17);
    payload << value;

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    MqttClient publisher;
    auto connect_status = publisher.Connect(BrokerUri(), ClientId("publisher"),
                                            deadline);
    if (!connect_status.ok()) {
      return connect_status;
    }
    auto publish_status = publisher.Publish(kTopic, payload.str(), deadline);
    if (!publish_status.ok()) {
      return publish_status;
    }
    return publisher.Disconnect(deadline);
  }

  opcua::Status WaitForDataValue(UA_StatusCode expected_status,
                                 double expected_value,
                                 bool require_source_timestamp) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < deadline) {
      auto observation = ReadDataNode();
      if (!observation.ok()) {
        return observation.status();
      }
      if (Matches(observation.value(), expected_status, expected_value,
                  require_source_timestamp)) {
        return opcua::Status::Ok();
      }
      UA_Client_run_iterate(client_.get(), 20);
      SleepUntilNextPoll(deadline, std::chrono::milliseconds(50));
    }

    auto final_observation = ReadDataNode();
    if (final_observation.ok()) {
      std::ostringstream stream;
      stream << "data value did not reach status "
             << StatusName(expected_status) << " value " << expected_value
             << "; last status "
             << StatusName(final_observation.value().status)
             << " has_value=" << final_observation.value().has_value
             << " value=" << final_observation.value().value
             << " has_source_timestamp="
             << final_observation.value().has_source_timestamp;
      return Error(stream.str());
    }
    return Error(DeadlineMessage("OPC UA data value wait"));
  }

  opcua::Status WaitForNotification(UA_StatusCode expected_status,
                                    double expected_value) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        for (std::size_t i = notification_cursor_; i < notifications_.size();
             ++i) {
          if (Matches(notifications_[i], expected_status, expected_value)) {
            notification_cursor_ = i + 1;
            return opcua::Status::Ok();
          }
        }
      }
      UA_Client_run_iterate(client_.get(), 50);
      SleepUntilNextPoll(deadline, std::chrono::milliseconds(50));
    }
    return Error(DeadlineMessage("OPC UA data-change notification wait"));
  }

  opcua::Status VerifyRemoteWriteRejected(double expected_value) {
    UA_Variant value;
    UA_Variant_init(&value);
    UA_Double scalar = 1234.5;
    UA_Variant_setScalar(&value, &scalar, &UA_TYPES[UA_TYPES_DOUBLE]);
    const UA_NodeId node_id =
        UA_NODEID_NUMERIC(2, static_cast<UA_UInt32>(kDataNodeId));
    const UA_StatusCode status =
        UA_Client_writeValueAttribute(client_.get(), node_id, &value);
    if (!UA_StatusCode_isBad(status)) {
      return Error("remote write unexpectedly succeeded");
    }
    return WaitForDataValue(UA_STATUSCODE_GOOD, expected_value, false);
  }

  opcua::Status StopBroker() {
    if (broker_controller_ == nullptr) {
      return opcua::Status::Ok();
    }
    auto status = broker_controller_->Stop(std::chrono::milliseconds(2000));
    broker_controller_.reset();
    return status;
  }

  opcua::Status RestartBroker() {
    auto stop_status = StopBroker();
    if (!stop_status.ok()) {
      return stop_status;
    }
    auto start_status = StartBroker();
    if (!start_status.ok()) {
      return start_status;
    }
    return WaitForConnectionState("connected");
  }

  void StopAll() {
    if (subscription_id_ != 0 && client_ != nullptr) {
      UA_Client_Subscriptions_deleteSingle(client_.get(), subscription_id_);
      subscription_id_ = 0;
    }
    client_.reset();
    if (daemon_controller_ != nullptr) {
      daemon_controller_->Stop(std::chrono::milliseconds(2000));
      daemon_controller_.reset();
    }
    if (broker_controller_ != nullptr) {
      broker_controller_->Stop(std::chrono::milliseconds(2000));
      broker_controller_.reset();
    }
  }

 private:
  IntegrationFixture(std::string mosquitto_path, std::string daemon_path,
                     int mqtt_port, int opcua_port)
      : mosquitto_path_(std::move(mosquitto_path)),
        daemon_path_(std::move(daemon_path)),
        mqtt_port_(mqtt_port),
        opcua_port_(opcua_port),
        db_path_(temp_dir_.path() / "integration.db"),
        config_path_(temp_dir_.path() / "mosquitto.conf") {}

  std::string BrokerUri() const {
    return "tcp://127.0.0.1:" + std::to_string(mqtt_port_);
  }

  std::string ClientId(const char* role) const {
    return std::string("opcua-integration-") + role + "-" +
           std::to_string(mqtt_port_) + "-" + std::to_string(opcua_port_);
  }

  std::string EndpointUrl() const {
    return "opc.tcp://127.0.0.1:" + std::to_string(opcua_port_);
  }

  void WriteMosquittoConfig() const {
    std::ofstream config(config_path_);
    config << "listener " << mqtt_port_ << " 127.0.0.1\n"
           << "allow_anonymous true\n"
           << "persistence false\n"
           << "log_type error\n";
  }

  opcua::Status ProbeBroker(Clock::time_point deadline) const {
    MqttClient client;
    auto connect_status = client.Connect(BrokerUri(), ClientId("probe"),
                                         deadline);
    if (!connect_status.ok()) {
      return connect_status;
    }
    return client.Disconnect(deadline);
  }

  opcua::Status ConnectOpcuaClient(Clock::time_point deadline) {
    while (Clock::now() < deadline) {
      auto status = daemon_controller_->status();
      if (status.state != opcua::ProcessState::kRunning) {
        return Error("opcua-daemon exited during startup: " +
                     status.diagnostic);
      }

      ClientPtr candidate(UA_Client_new());
      if (candidate == nullptr) {
        return Error("UA_Client_new failed");
      }
      const UA_StatusCode connect_status =
          UA_Client_connect(candidate.get(), EndpointUrl().c_str());
      if (!UA_StatusCode_isBad(connect_status)) {
        client_ = std::move(candidate);
        return opcua::Status::Ok();
      }
      SleepUntilNextPoll(deadline, std::chrono::milliseconds(100));
    }
    return Error(DeadlineMessage("OPC UA client connect"));
  }

  opcua::Status CreateSubscription() {
    UA_CreateSubscriptionRequest request =
        UA_CreateSubscriptionRequest_default();
    request.requestedPublishingInterval = 100.0;
    request.requestedMaxKeepAliveCount = 5;
    UA_CreateSubscriptionResponse response = UA_Client_Subscriptions_create(
        client_.get(), request, this, nullptr, nullptr);
    const UA_StatusCode create_status = response.responseHeader.serviceResult;
    subscription_id_ = response.subscriptionId;
    UA_CreateSubscriptionResponse_clear(&response);
    if (UA_StatusCode_isBad(create_status) || subscription_id_ == 0) {
      return Error("subscription create failed: " + StatusName(create_status));
    }

    const UA_NodeId node_id =
        UA_NODEID_NUMERIC(2, static_cast<UA_UInt32>(kDataNodeId));
    UA_MonitoredItemCreateRequest item =
        UA_MonitoredItemCreateRequest_default(node_id);
    item.requestedParameters.samplingInterval = 50.0;
    item.requestedParameters.queueSize = 10;
    UA_MonitoredItemCreateResult item_result =
        UA_Client_MonitoredItems_createDataChange(
            client_.get(), subscription_id_, UA_TIMESTAMPSTORETURN_SOURCE,
            item, this, &IntegrationFixture::OnDataChange, nullptr);
    const UA_StatusCode item_status = item_result.statusCode;
    UA_MonitoredItemCreateResult_clear(&item_result);
    if (UA_StatusCode_isBad(item_status)) {
      return Error("monitored item create failed: " + StatusName(item_status));
    }
    return opcua::Status::Ok();
  }

  static void OnDataChange(UA_Client* /*client*/, UA_UInt32 /*sub_id*/,
                           void* /*sub_context*/, UA_UInt32 /*mon_id*/,
                           void* mon_context, UA_DataValue* value) {
    auto* fixture = static_cast<IntegrationFixture*>(mon_context);
    if (fixture == nullptr || value == nullptr) {
      return;
    }

    Notification notification;
    notification.status =
        value->hasStatus ? value->status : UA_STATUSCODE_GOOD;
    notification.has_value =
        value->hasValue &&
        UA_Variant_hasScalarType(&value->value, &UA_TYPES[UA_TYPES_DOUBLE]);
    if (notification.has_value) {
      notification.value =
          *static_cast<const UA_Double*>(value->value.data);
    }

    std::lock_guard<std::mutex> lock(fixture->notification_mutex_);
    fixture->notifications_.push_back(notification);
  }

  opcua::Result<DataObservation> ReadDataNode() {
    const UA_NodeId node_id =
        UA_NODEID_NUMERIC(2, static_cast<UA_UInt32>(kDataNodeId));
    return ReadNode(node_id);
  }

  opcua::Result<DataObservation> ReadNode(UA_NodeId node_id) {
    UA_ReadValueId item;
    UA_ReadValueId_init(&item);
    item.nodeId = node_id;
    item.attributeId = UA_ATTRIBUTEID_VALUE;

    UA_ReadRequest request;
    UA_ReadRequest_init(&request);
    request.timestampsToReturn = UA_TIMESTAMPSTORETURN_SOURCE;
    request.nodesToRead = &item;
    request.nodesToReadSize = 1;

    UA_ReadResponse response = UA_Client_Service_read(client_.get(), request);
    const UA_StatusCode service_status = response.responseHeader.serviceResult;
    if (UA_StatusCode_isBad(service_status) || response.resultsSize != 1) {
      UA_ReadResponse_clear(&response);
      return Error("read failed: " + StatusName(service_status));
    }

    DataObservation observation;
    const UA_DataValue& value = response.results[0];
    observation.status =
        value.hasStatus ? value.status : UA_STATUSCODE_GOOD;
    observation.has_value =
        value.hasValue &&
        UA_Variant_hasScalarType(&value.value, &UA_TYPES[UA_TYPES_DOUBLE]);
    if (observation.has_value) {
      observation.value = *static_cast<const UA_Double*>(value.value.data);
    }
    observation.has_source_timestamp = value.hasSourceTimestamp;
    if (observation.has_source_timestamp) {
      observation.source_timestamp = value.sourceTimestamp;
    }
    UA_ReadResponse_clear(&response);
    return observation;
  }

  opcua::Status WaitForConnectionState(const char* expected) {
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < deadline) {
      const UA_NodeId node_id = UA_NODEID_STRING(
          2, const_cast<char*>("MqttSource.ConnectionState"));
      UA_ReadValueId item;
      UA_ReadValueId_init(&item);
      item.nodeId = node_id;
      item.attributeId = UA_ATTRIBUTEID_VALUE;

      UA_ReadRequest request;
      UA_ReadRequest_init(&request);
      request.nodesToRead = &item;
      request.nodesToReadSize = 1;

      UA_ReadResponse response = UA_Client_Service_read(client_.get(), request);
      bool matched = false;
      if (!UA_StatusCode_isBad(response.responseHeader.serviceResult) &&
          response.resultsSize == 1 && response.results[0].hasValue &&
          UA_Variant_hasScalarType(&response.results[0].value,
                                   &UA_TYPES[UA_TYPES_STRING])) {
        const UA_String* state =
            static_cast<const UA_String*>(response.results[0].value.data);
        const UA_String expected_string =
            UA_STRING(const_cast<char*>(expected));
        matched = UA_String_equal(state, &expected_string);
      }
      UA_ReadResponse_clear(&response);
      if (matched) {
        return opcua::Status::Ok();
      }

      UA_Client_run_iterate(client_.get(), 20);
      SleepUntilNextPoll(deadline, std::chrono::milliseconds(100));
    }
    return Error("MQTT diagnostics did not report connected after broker restart");
  }

  static bool Matches(const DataObservation& observation,
                      UA_StatusCode expected_status, double expected_value,
                      bool require_source_timestamp) {
    return observation.status == expected_status && observation.has_value &&
           std::fabs(observation.value - expected_value) < 0.000001 &&
           (!require_source_timestamp || observation.has_source_timestamp);
  }

  static bool Matches(const Notification& notification,
                      UA_StatusCode expected_status, double expected_value) {
    return notification.status == expected_status && notification.has_value &&
           std::fabs(notification.value - expected_value) < 0.000001;
  }

  TemporaryDirectory temp_dir_;
  std::string mosquitto_path_;
  std::string daemon_path_;
  int mqtt_port_;
  int opcua_port_;
  std::filesystem::path db_path_;
  std::filesystem::path config_path_;
  std::unique_ptr<opcua::ProcessController> broker_controller_;
  std::unique_ptr<opcua::ProcessController> daemon_controller_;
  ClientPtr client_;
  UA_UInt32 subscription_id_ = 0;
  std::mutex notification_mutex_;
  std::vector<Notification> notifications_;
  std::size_t notification_cursor_ = 0;
};

int RunScenario(IntegrationFixture& fixture) {
  auto run = [](const char* step, const opcua::Status& status) {
    if (!status.ok()) {
      std::cerr << step << ": " << status.message() << "\n";
      return 1;
    }
    return 0;
  };

  if (int rc = run("StartBroker", fixture.StartBroker())) return rc;
  if (int rc = run("ConfigureDatabase", fixture.ConfigureDatabase())) return rc;
  if (int rc = run("StartDaemon", fixture.StartDaemon())) return rc;
  if (int rc = run("PublishDouble initial", fixture.PublishDouble(37.5))) {
    return rc;
  }
  if (int rc = run("WaitForDataValue Good",
                   fixture.WaitForDataValue(UA_STATUSCODE_GOOD, 37.5, true))) {
    return rc;
  }
  if (int rc = run("WaitForNotification Good",
                   fixture.WaitForNotification(UA_STATUSCODE_GOOD, 37.5))) {
    return rc;
  }
  if (int rc = run("VerifyRemoteWriteRejected",
                   fixture.VerifyRemoteWriteRejected(37.5))) {
    return rc;
  }
  if (int rc = run("PublishDouble update", fixture.PublishDouble(38.0))) {
    return rc;
  }
  if (int rc = run("WaitForNotification Good update",
                   fixture.WaitForNotification(UA_STATUSCODE_GOOD, 38.0))) {
    return rc;
  }
  if (int rc = run("StopBroker", fixture.StopBroker())) return rc;
  if (int rc = run("WaitForDataValue Uncertain",
                   fixture.WaitForDataValue(kUnavailableStatus, 38.0, false))) {
    return rc;
  }
  if (int rc =
          run("WaitForNotification Uncertain",
              fixture.WaitForNotification(kUnavailableStatus, 38.0))) {
    return rc;
  }
  if (int rc = run("RestartBroker", fixture.RestartBroker())) return rc;
  if (int rc = run("PublishDouble after restart",
                   fixture.PublishDouble(38.0))) {
    return rc;
  }
  if (int rc = run("WaitForDataValue Good after restart",
                   fixture.WaitForDataValue(UA_STATUSCODE_GOOD, 38.0, false))) {
    return rc;
  }
  if (int rc = run("WaitForNotification Good after restart",
                   fixture.WaitForNotification(UA_STATUSCODE_GOOD, 38.0))) {
    return rc;
  }
  fixture.StopAll();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: mqtt_integration_test <mosquitto> <opcua-daemon>\n";
    return 1;
  }

  auto fixture_result = IntegrationFixture::Create(argv[1], argv[2]);
  if (!fixture_result.ok()) {
    std::cerr << fixture_result.status().message() << "\n";
    return 1;
  }
  return RunScenario(*fixture_result.value());
}
