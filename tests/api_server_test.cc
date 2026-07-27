#include "config/config_repository.h"
#include "supervisor/api_server.h"
#include "supervisor/config_json_codec.h"
#include "supervisor/process_controller.h"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr int kTestPortRange = 10;

int ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

int FirstTestPort() { return 30000 + ProcessId() % 20000; }

int Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

int TestMqttConfigJsonCodecSupportsUint32MaxNodeId() {
  const std::string json =
      "{\"enabled\":true,\"broker_uri\":\"tcp://127.0.0.1:1883\","
      "\"client_id\":\"codec-test\",\"topic\":\"test/temperature\","
      "\"qos\":1,\"node_id\":4294967295,\"browse_name\":\"Temperature\","
      "\"data_type\":\"double\",\"stale_timeout_ms\":5000}";
  auto config_result = opcua::ParseMqttConfigJson(json);
  if (int rc = Expect(config_result.ok(),
                      "MQTT codec should parse UINT32_MAX node_id")) {
    return rc;
  }
  if (int rc = Expect(config_result.value().node_id ==
                          std::numeric_limits<std::uint32_t>::max(),
                      "MQTT codec should preserve UINT32_MAX node_id")) {
    return rc;
  }
  if (int rc = Expect(opcua::MqttConfigToJson(config_result.value()) == json,
                      "MQTT codec serializer field order mismatch")) {
    return rc;
  }

  std::string false_json = json;
  false_json.replace(false_json.find("true"), 4, "false");
  auto false_result = opcua::ParseMqttConfigJson(false_json);
  if (int rc = Expect(false_result.ok() && !false_result.value().enabled,
                      "MQTT codec should parse false")) {
    return rc;
  }

  std::string overflow_json = json;
  overflow_json.replace(overflow_json.find("4294967295"), 10, "4294967296");
  return Expect(!opcua::ParseMqttConfigJson(overflow_json).ok(),
                "MQTT codec should reject node_id above UINT32_MAX");
}

void RemoveDatabaseFiles(const std::string& db_path) {
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());
}

class DatabaseFilesCleanup {
 public:
  explicit DatabaseFilesCleanup(std::string db_path)
      : db_path_(std::move(db_path)) {}
  ~DatabaseFilesCleanup() { RemoveDatabaseFiles(db_path_); }

  DatabaseFilesCleanup(const DatabaseFilesCleanup&) = delete;
  DatabaseFilesCleanup& operator=(const DatabaseFilesCleanup&) = delete;

 private:
  std::string db_path_;
};

bool HasJsonContentType(const httplib::Result& response) {
  if (!response) {
    return false;
  }
  return response->get_header_value("Content-Type").find("application/json") ==
         0;
}

std::string CompleteConfigJson(const std::string& port = "4850") {
  return "{"
         "\"server_application_name\":\"Test \\\"Server\\\"\\nLine\","
         "\"server_product_uri\":\"urn:test:server\","
         "\"server_bind_address\":\"127.0.0.1\","
         "\"server_port\":" +
         port +
         ",\"server_endpoint_path\":\"/test\","
         "\"security_mode\":\"none\","
         "\"security_policy\":\"none\","
         "\"max_sessions\":25,"
         "\"max_subscriptions\":50,"
         "\"logging_level\":\"debug\","
         "\"logging_target\":\"file:C:\\\\logs\\\\opcua.log\","
         "\"address_space_mode\":\"builtin\","
         "\"address_space_path\":\"path\\u0020with\\u0020spaces\"}";
}

std::string CompleteMqttJson(const std::string& enabled = "true",
                             const std::string& node_id = "2001") {
  return "{\"enabled\":" + enabled +
         ",\"broker_uri\":\"tcp://127.0.0.1:2883\","
         "\"client_id\":\"test-client\","
         "\"topic\":\"test/temperature\",\"qos\":1,\"node_id\":" +
         node_id +
         ",\"browse_name\":\"Temperature\",\"data_type\":\"double\","
         "\"stale_timeout_ms\":2500}";
}

class ApiFixture {
 public:
  static std::unique_ptr<ApiFixture> Create(const std::string& child_path) {
    const std::string db_path =
        "api_server_test_" + std::to_string(ProcessId()) + ".db";
    RemoveDatabaseFiles(db_path);
    auto repository_result = opcua::ConfigRepository::Open(db_path);
    if (!repository_result.ok()) {
      std::cerr << repository_result.status().message() << "\n";
      return nullptr;
    }
    if (auto status = repository_result.value().Initialize(); !status.ok()) {
      std::cerr << status.message() << "\n";
      return nullptr;
    }
    return std::unique_ptr<ApiFixture>(new ApiFixture(
        db_path, std::move(repository_result.value()), child_path));
  }

  ~ApiFixture() {
    if (server_ != nullptr) {
      server_->Stop();
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    (void)controller_.Stop(std::chrono::milliseconds(2000));
  }

  ApiFixture(const ApiFixture&) = delete;
  ApiFixture& operator=(const ApiFixture&) = delete;

  bool Start() {
    for (int candidate = FirstTestPort();
         candidate < FirstTestPort() + kTestPortRange;
         ++candidate) {
      server_ = std::make_unique<opcua::ApiServer>(&repository_, &controller_);
      std::promise<opcua::Status> completion;
      auto future = completion.get_future();
      server_thread_ = std::thread(
          [this, candidate, completion = std::move(completion)]() mutable {
            completion.set_value(server_->Run("127.0.0.1", candidate));
          });

      for (int attempt = 0; attempt < 50; ++attempt) {
        if (future.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready) {
          break;
        }
        httplib::Client client("127.0.0.1", candidate);
        client.set_connection_timeout(0, 100000);
        auto response = client.Get("/health");
        if (response && response->status == 200 &&
            response->body == "{\"status\":\"ok\"}") {
          port_ = candidate;
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }

      server_->Stop();
      server_thread_.join();
      server_.reset();
    }
    return false;
  }

  httplib::Client Client() const { return httplib::Client("127.0.0.1", port_); }
  opcua::ConfigRepository& repository() { return repository_; }
  opcua::ProcessController& controller() { return controller_; }

 private:
  ApiFixture(std::string db_path, opcua::ConfigRepository repository,
             const std::string& child_path)
      : database_cleanup_(std::move(db_path)),
        repository_(std::move(repository)),
        controller_(child_path, {}) {}

  DatabaseFilesCleanup database_cleanup_;
  opcua::ConfigRepository repository_;
  opcua::ProcessController controller_;
  std::unique_ptr<opcua::ApiServer> server_;
  std::thread server_thread_;
  int port_ = 0;
};

int TestHealth(ApiFixture* fixture) {
  auto response = fixture->Client().Get("/health");
  if (int rc = Expect(response && response->status == 200,
                      "GET /health should return 200")) {
    return rc;
  }
  if (int rc = Expect(response->body == "{\"status\":\"ok\"}",
                      "GET /health body mismatch")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                "GET /health should return application/json");
}

int TestStatus(ApiFixture* fixture) {
  auto response = fixture->Client().Get("/api/v1/status");
  if (int rc = Expect(response && response->status == 200,
                      "GET /api/v1/status should return 200")) {
    return rc;
  }
  if (int rc = Expect(
          response->body ==
              "{\"state\":\"stopped\",\"exit_code\":0,\"diagnostic\":\"\"}",
          "initial daemon status body mismatch")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                "GET /api/v1/status should return application/json");
}

int TestGetConfig(ApiFixture* fixture) {
  auto response = fixture->Client().Get("/api/v1/config");
  if (int rc = Expect(response && response->status == 200,
                      "GET /api/v1/config should return 200")) {
    return rc;
  }
  const std::string expected =
      "{\"server_application_name\":\"Open62541 C++ Server\","
      "\"server_product_uri\":\"urn:rakctite:opcua-server-opensource\","
      "\"server_bind_address\":\"0.0.0.0\",\"server_port\":4840,"
      "\"server_endpoint_path\":\"/\",\"security_mode\":\"none\","
      "\"security_policy\":\"none\",\"max_sessions\":100,"
      "\"max_subscriptions\":100,\"logging_level\":\"info\","
      "\"logging_target\":\"stdout\",\"address_space_mode\":\"builtin\","
      "\"address_space_path\":\"\"}";
  if (int rc = Expect(response->body == expected,
                      "GET config should return every current field")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                "GET /api/v1/config should return application/json");
}

int TestPutConfigPersistsCompleteObject(ApiFixture* fixture) {
  auto response = fixture->Client().Put("/api/v1/config", CompleteConfigJson(),
                                        "application/json");
  if (int rc = Expect(response && response->status == 200,
                      "valid PUT config should return 200")) {
    return rc;
  }
  if (int rc = Expect(HasJsonContentType(response),
                      "valid PUT config should return application/json")) {
    return rc;
  }

  auto loaded = fixture->repository().Load();
  if (int rc = Expect(loaded.ok(), "persisted config should load")) return rc;
  if (int rc = Expect(loaded.value().server_application_name ==
                          "Test \"Server\"\nLine",
                      "PUT should decode escaped JSON strings")) {
    return rc;
  }
  if (int rc = Expect(loaded.value().logging_target ==
                          "file:C:\\logs\\opcua.log",
                      "PUT should persist backslashes")) {
    return rc;
  }
  if (int rc = Expect(loaded.value().address_space_path == "path with spaces",
                      "PUT should decode Unicode escapes")) {
    return rc;
  }
  if (int rc = Expect(loaded.value().server_port == 4850,
                      "PUT should persist integer fields")) {
    return rc;
  }

  auto get_response = fixture->Client().Get("/api/v1/config");
  if (int rc = Expect(get_response && get_response->status == 200,
                      "GET should return persisted config")) {
    return rc;
  }
  if (int rc = Expect(
          get_response->body.find(
              "\"server_application_name\":\"Test \\\"Server\\\"\\nLine\"") !=
              std::string::npos,
          "GET config should escape quotes and control characters")) {
    return rc;
  }
  return Expect(
      get_response->body.find(
          "\"logging_target\":\"file:C:\\\\logs\\\\opcua.log\"") !=
          std::string::npos,
      "GET config should escape backslashes");
}

int ExpectBadPut(ApiFixture* fixture, const std::string& body,
                 const std::string& description) {
  auto response = fixture->Client().Put("/api/v1/config", body,
                                        "application/json");
  if (int rc = Expect(response && response->status == 400,
                      description + " should return 400")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                description + " should return application/json");
}

int TestPutConfigRejectsNul(ApiFixture* fixture) {
  std::string body = CompleteConfigJson();
  const std::string application_name = "Test \\\"Server\\\"\\nLine";
  body.replace(body.find(application_name), application_name.size(),
               "A\\u0000B");
  return ExpectBadPut(fixture, body, "JSON string containing NUL");
}

int TestOversizedPayloadRejectedByTransport(ApiFixture* fixture) {
  auto response = fixture->Client().Put("/api/v1/config",
                                        std::string(65538, ' '),
                                        "application/json");
  if (int rc = Expect(response && response->status == 400,
                      "transport-oversized config should return 400")) {
    return rc;
  }
  if (int rc = Expect(HasJsonContentType(response),
                      "transport-oversized config should return JSON")) {
    return rc;
  }
  return Expect(
      response->body ==
          "{\"error\":\"configuration request exceeds 65536 bytes\"}",
      "transport-oversized config should use the transport-limit error");
}

int TestMaximumPayloadSizeAccepted(ApiFixture* fixture) {
  std::string body = CompleteConfigJson();
  body.resize(65536, ' ');
  auto response = fixture->Client().Put("/api/v1/config", body,
                                        "application/json");
  return Expect(response && response->status == 200,
                "exactly 65536 config bytes should be accepted");
}

int TestPutConfigRejectsInvalidJson(ApiFixture* fixture) {
  if (int rc = ExpectBadPut(fixture, "{", "malformed JSON")) return rc;
  if (int rc = ExpectBadPut(fixture, CompleteConfigJson("70000"),
                            "invalid config")) {
    return rc;
  }
  if (int rc = ExpectBadPut(fixture, CompleteConfigJson("\"4850\""),
                            "integer type mismatch")) {
    return rc;
  }
  if (int rc = ExpectBadPut(fixture, CompleteConfigJson("2147483648"),
                            "integer overflow")) {
    return rc;
  }

  std::string missing = CompleteConfigJson();
  const std::string field = "\"address_space_path\":\"path\\u0020with\\u0020spaces\"";
  missing.erase(missing.find("," + field), field.size() + 1);
  if (int rc = ExpectBadPut(fixture, missing, "missing field")) return rc;

  std::string duplicate = CompleteConfigJson();
  duplicate.insert(1, "\"server_port\":4840,");
  if (int rc = ExpectBadPut(fixture, duplicate, "duplicate field")) return rc;

  std::string unknown = CompleteConfigJson();
  unknown.insert(1, "\"unknown\":1,");
  if (int rc = ExpectBadPut(fixture, unknown, "unknown field")) return rc;

  if (int rc = ExpectBadPut(fixture, CompleteConfigJson() + " trailing",
                            "trailing data")) {
    return rc;
  }
  return ExpectBadPut(fixture, std::string(65537, ' '), "oversized JSON");
}

int TestGetDefaultMqttConfig(ApiFixture* fixture) {
  auto response = fixture->Client().Get("/api/v1/mqtt-config");
  if (int rc = Expect(response && response->status == 200,
                      "GET default MQTT config should return 200, got " +
                          (response ? std::to_string(response->status)
                                    : "no response"))) {
    return rc;
  }
  const std::string expected =
      "{\"enabled\":false,\"broker_uri\":\"tcp://127.0.0.1:1883\","
      "\"client_id\":\"opcua-server\",\"topic\":\"test/temperature\","
      "\"qos\":1,\"node_id\":1001,\"browse_name\":\"Temperature\","
      "\"data_type\":\"double\",\"stale_timeout_ms\":5000}";
  if (int rc = Expect(response->body == expected,
                      "GET should return exact default MQTT config JSON")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                "GET default MQTT config should return application/json");
}

int TestGetDefaultMqttConfigOnFreshRepository(const std::string& child_path) {
  auto fixture = ApiFixture::Create(child_path);
  if (int rc = Expect(fixture != nullptr,
                      "failed to create default MQTT API fixture")) {
    return rc;
  }
  if (int rc = Expect(fixture->Start(),
                      "failed to bind default MQTT API test server")) {
    return rc;
  }
  return TestGetDefaultMqttConfig(fixture.get());
}

int TestPutMqttConfigPersistsCompleteObject(ApiFixture* fixture) {
  const std::string mqtt_json = CompleteMqttJson();
  auto response = fixture->Client().Put("/api/v1/mqtt-config", mqtt_json,
                                        "application/json");
  if (int rc = Expect(response && response->status == 200,
                      "valid PUT MQTT config should return 200")) {
    return rc;
  }
  if (int rc = Expect(response->body == "{\"status\":\"ok\"}",
                      "valid PUT MQTT config body mismatch")) {
    return rc;
  }
  if (int rc = Expect(HasJsonContentType(response),
                      "valid PUT MQTT config should return application/json")) {
    return rc;
  }

  auto get_response = fixture->Client().Get("/api/v1/mqtt-config");
  if (int rc = Expect(get_response && get_response->status == 200,
                      "GET persisted MQTT config should return 200")) {
    return rc;
  }
  return Expect(get_response->body == mqtt_json,
                "GET should return byte-exact persisted MQTT config JSON");
}

int ExpectBadMqttPut(ApiFixture* fixture, const std::string& body,
                     const std::string& description) {
  auto response = fixture->Client().Put("/api/v1/mqtt-config", body,
                                        "application/json");
  if (int rc = Expect(response && response->status == 400,
                      description + " should return 400")) {
    return rc;
  }
  return Expect(HasJsonContentType(response),
                description + " should return application/json");
}

int TestPutMqttConfigRejectsInvalidInput(ApiFixture* fixture) {
  const std::string persisted = CompleteMqttJson();
  auto seed_response = fixture->Client().Put(
      "/api/v1/mqtt-config", persisted, "application/json");
  if (int rc = Expect(seed_response && seed_response->status == 200 &&
                          seed_response->body == "{\"status\":\"ok\"}",
                      "MQTT invalid-input test should seed persisted config")) {
    return rc;
  }

  std::string invalid_broker = persisted;
  invalid_broker.replace(invalid_broker.find("tcp://127.0.0.1:2883"), 20,
                         "http://bad");
  if (int rc = ExpectBadMqttPut(fixture, invalid_broker,
                                "invalid MQTT broker URI")) {
    return rc;
  }
  auto get_response = fixture->Client().Get("/api/v1/mqtt-config");
  if (int rc = Expect(get_response && get_response->status == 200 &&
                          get_response->body == persisted,
                      "invalid MQTT PUT should preserve persisted config")) {
    return rc;
  }

  std::string missing = persisted;
  const std::string stale_timeout = ",\"stale_timeout_ms\":2500";
  missing.erase(missing.find(stale_timeout), stale_timeout.size());
  if (int rc = ExpectBadMqttPut(fixture, missing,
                                "missing MQTT stale_timeout_ms")) {
    return rc;
  }

  std::string duplicate = persisted;
  duplicate.insert(1, "\"enabled\":true,");
  if (int rc = ExpectBadMqttPut(fixture, duplicate,
                                "duplicate MQTT enabled")) {
    return rc;
  }

  std::string unknown = persisted;
  unknown.insert(1, "\"unknown\":1,");
  if (int rc = ExpectBadMqttPut(fixture, unknown, "unknown MQTT field")) {
    return rc;
  }

  std::string numeric_enabled = persisted;
  numeric_enabled.replace(numeric_enabled.find("true"), 4, "1");
  if (int rc = ExpectBadMqttPut(fixture, numeric_enabled,
                                "numeric MQTT enabled")) {
    return rc;
  }

  std::string nul_client_id = persisted;
  nul_client_id.replace(nul_client_id.find("test-client"), 11,
                        "test\\u0000client");
  if (int rc = ExpectBadMqttPut(fixture, nul_client_id,
                                "MQTT client_id containing NUL")) {
    return rc;
  }

  return ExpectBadMqttPut(fixture, std::string(65537, ' '),
                          "oversized MQTT JSON");
}

int TestMqttConfigHttpBoundaries(ApiFixture* fixture) {
  const std::string maximum = CompleteMqttJson("true", "4294967295");
  auto response = fixture->Client().Put("/api/v1/mqtt-config", maximum,
                                        "application/json");
  if (int rc = Expect(response && response->status == 200,
                      "UINT32_MAX MQTT node_id should be accepted")) {
    return rc;
  }
  auto get_response = fixture->Client().Get("/api/v1/mqtt-config");
  if (int rc = Expect(get_response && get_response->status == 200 &&
                          get_response->body == maximum,
                      "UINT32_MAX MQTT node_id should round-trip")) {
    return rc;
  }

  if (int rc = ExpectBadMqttPut(
          fixture, CompleteMqttJson("true", "4294967296"),
          "MQTT node_id above UINT32_MAX")) {
    return rc;
  }
  if (int rc = ExpectBadMqttPut(fixture,
                                CompleteMqttJson("true", "-1"),
                                "negative MQTT node_id")) {
    return rc;
  }

  const std::string disabled = CompleteMqttJson("false", "2001");
  response = fixture->Client().Put("/api/v1/mqtt-config", disabled,
                                   "application/json");
  if (int rc = Expect(response && response->status == 200,
                      "false MQTT enabled should be accepted")) {
    return rc;
  }
  get_response = fixture->Client().Get("/api/v1/mqtt-config");
  return Expect(get_response && get_response->status == 200 &&
                    get_response->body == disabled,
                "false MQTT enabled should round-trip");
}

int TestPutConfigPreservesIntegerTokenErrors(ApiFixture* fixture) {
  auto boolean_response = fixture->Client().Put(
      "/api/v1/config", CompleteConfigJson("true"), "application/json");
  auto null_response = fixture->Client().Put(
      "/api/v1/config", CompleteConfigJson("null"), "application/json");
  constexpr const char* kExpectedBody =
      "{\"error\":\"expected JSON integer\"}";

  int result = 0;
  if (int rc = Expect(boolean_response && boolean_response->status == 400 &&
                          boolean_response->body == kExpectedBody,
                      "boolean server_port error body changed")) {
    result = rc;
  }
  if (int rc = Expect(null_response && null_response->status == 400 &&
                          null_response->body == kExpectedBody,
                      "null server_port error body changed")) {
    result = rc;
  }
  return result;
}

int TestDaemonLifecycle(ApiFixture* fixture) {
  auto start = fixture->Client().Post("/api/v1/daemon/start", "", "text/plain");
  if (int rc = Expect(start && start->status == 200,
                      "daemon start should return 200")) {
    return rc;
  }

  auto status = fixture->Client().Get("/api/v1/status");
  if (int rc = Expect(status &&
                          status->body.find("\"state\":\"running\"") !=
                              std::string::npos,
                      "daemon should be running after start")) {
    return rc;
  }

  auto duplicate_start =
      fixture->Client().Post("/api/v1/daemon/start", "", "text/plain");
  if (int rc = Expect(duplicate_start && duplicate_start->status == 500,
                      "lifecycle errors should return 500")) {
    return rc;
  }

  auto restart =
      fixture->Client().Post("/api/v1/daemon/restart", "", "text/plain");
  if (int rc = Expect(restart && restart->status == 200,
                      "daemon restart should return 200")) {
    return rc;
  }

  auto stop = fixture->Client().Post("/api/v1/daemon/stop", "", "text/plain");
  if (int rc = Expect(stop && stop->status == 200,
                      "daemon stop should return 200")) {
    return rc;
  }
  status = fixture->Client().Get("/api/v1/status");
  return Expect(status &&
                    status->body.find("\"state\":\"stopped\"") !=
                        std::string::npos,
                "daemon should be stopped after stop");
}

int TestStopBeforeRunReturnsPromptly(ApiFixture* fixture) {
  for (int candidate = FirstTestPort() + 20;
       candidate < FirstTestPort() + 20 + kTestPortRange;
       ++candidate) {
    opcua::ApiServer server(&fixture->repository(), &fixture->controller());
    server.Stop();

    std::promise<opcua::Status> completion;
    auto future = completion.get_future();
    std::thread server_thread(
        [&server, candidate, completion = std::move(completion)]() mutable {
          completion.set_value(server.Run("127.0.0.1", candidate));
        });

    if (future.wait_for(std::chrono::milliseconds(250)) !=
        std::future_status::ready) {
      for (int attempt = 0; attempt < 200; ++attempt) {
        server.Stop();
        if (future.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
          break;
        }
      }
      server_thread.join();
      return Expect(false, "Stop before Run should be latched");
    }

    auto run_status = future.get();
    server_thread.join();
    if (run_status.ok()) return 0;
  }
  return Expect(false, "no test port available for immediate Stop test");
}

int TestImmediateStopAfterRunReturnsPromptly(ApiFixture* fixture) {
  for (int candidate = FirstTestPort() + 40;
       candidate < FirstTestPort() + 40 + kTestPortRange;
       ++candidate) {
    opcua::ApiServer server(&fixture->repository(), &fixture->controller());
    std::promise<opcua::Status> completion;
    auto future = completion.get_future();
    std::thread server_thread(
        [&server, candidate, completion = std::move(completion)]() mutable {
          completion.set_value(server.Run("127.0.0.1", candidate));
        });
    server.Stop();

    if (future.wait_for(std::chrono::milliseconds(250)) !=
        std::future_status::ready) {
      for (int attempt = 0; attempt < 200; ++attempt) {
        server.Stop();
        if (future.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
          break;
        }
      }
      server_thread.join();
      return Expect(false, "Stop immediately after Run should finish Run");
    }

    auto run_status = future.get();
    server_thread.join();
    if (run_status.ok()) return 0;
  }
  return Expect(false, "no test port available for startup Stop test");
}

int TestEphemeralPortLifecycle(ApiFixture* fixture) {
  opcua::ApiServer server(&fixture->repository(), &fixture->controller());
  std::packaged_task<opcua::Status()> server_task(
      [&server] { return server.Run("127.0.0.1", 0); });
  auto future = server_task.get_future();
  std::thread server_thread(std::move(server_task));

  if (future.wait_for(std::chrono::milliseconds(100)) !=
      std::future_status::ready) {
    server.Stop();
  }
  if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    server.Stop();
    server_thread.join();
    return Expect(false, "ephemeral-port API server did not stop");
  }

  const auto run_status = future.get();
  server_thread.join();
  return Expect(run_status.ok(),
                "API server should support ephemeral port zero");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "missing test_child path\n";
    return 1;
  }

  if (int rc = TestMqttConfigJsonCodecSupportsUint32MaxNodeId()) return rc;

  if (int rc = TestGetDefaultMqttConfigOnFreshRepository(argv[1])) return rc;

  auto fixture = ApiFixture::Create(argv[1]);
  if (int rc = Expect(fixture != nullptr, "failed to create API fixture")) {
    return rc;
  }
  if (int rc = Expect(fixture->Start(), "failed to bind API test server")) {
    return rc;
  }

  if (int rc = TestHealth(fixture.get())) return rc;
  if (int rc = TestStatus(fixture.get())) return rc;
  if (int rc = TestGetConfig(fixture.get())) return rc;
  if (int rc = TestPutConfigPersistsCompleteObject(fixture.get())) return rc;
  if (int rc = TestPutMqttConfigPersistsCompleteObject(fixture.get())) {
    return rc;
  }
  if (int rc = TestPutMqttConfigRejectsInvalidInput(fixture.get())) return rc;
  if (int rc = TestMqttConfigHttpBoundaries(fixture.get())) return rc;
  if (int rc = TestPutConfigRejectsNul(fixture.get())) return rc;
  if (int rc = TestOversizedPayloadRejectedByTransport(fixture.get())) return rc;
  if (int rc = TestMaximumPayloadSizeAccepted(fixture.get())) return rc;
  if (int rc = TestPutConfigRejectsInvalidJson(fixture.get())) return rc;
  if (int rc = TestPutConfigPreservesIntegerTokenErrors(fixture.get())) {
    return rc;
  }
  if (int rc = TestDaemonLifecycle(fixture.get())) return rc;
  if (int rc = TestStopBeforeRunReturnsPromptly(fixture.get())) return rc;
  if (int rc = TestImmediateStopAfterRunReturnsPromptly(fixture.get())) return rc;
  if (int rc = TestEphemeralPortLifecycle(fixture.get())) return rc;
  return 0;
}
