# Realtime Value Store And MQTT MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a read-only, callback-backed OPC UA realtime value path that receives one scalar MQTT topic, stores only the latest value and quality in process memory, and applies SQLite/API configuration after daemon restart.

**Architecture:** The supervisor persists one flat MQTT configuration in normalized SQLite tables. At daemon startup, a fixed-address `RealtimeValueStore` is created before callback-backed open62541 Variable Nodes and an asynchronous Paho MQTT adapter; MQTT callbacks update only the store, while OPC UA reads and subscriptions obtain consistent snapshots through node callbacks.

**Tech Stack:** C++17, C99, CMake 3.20+, open62541 1.5.5, SQLite, cpp-httplib, Eclipse Paho MQTT C 1.3.16, Eclipse Mosquitto for opt-in integration tests, CTest, GCC/Clang/MSVC.

---

## File Map

New production files:

- `src/config/mqtt_config.h`: MQTT configuration value type and validation contract.
- `src/config/mqtt_config.cc`: defaults and validation.
- `src/supervisor/config_json_codec.h`: JSON codec API for server and MQTT configuration.
- `src/supervisor/config_json_codec.cc`: extracted flat JSON parser and serializers.
- `src/daemon/realtime_value_store.h`: stable slot identifiers, scalar values, snapshots, and store API.
- `src/daemon/realtime_value_store.cc`: synchronized latest-value and quality transitions.
- `src/daemon/realtime_address_space.h`: callback Node registration API.
- `src/daemon/realtime_address_space.cc`: open62541 callback and scalar conversion.
- `src/daemon/mqtt_payload_parser.h`: bounded scalar parser API.
- `src/daemon/mqtt_payload_parser.cc`: Boolean, Int64, and Double parsing.
- `src/daemon/mqtt_adapter.h`: Paho lifecycle and source-health API.
- `src/daemon/mqtt_adapter.cc`: connection, reconnect, message, and stale handling.

Modified production files:

- `src/config/config_repository.h`: add MQTT load/save methods.
- `src/config/config_repository.cc`: normalized MQTT schema and transactional persistence.
- `src/supervisor/api_server.cc`: route MQTT config through the extracted codec.
- `src/daemon/opcua_server.h`: accept MQTT configuration.
- `src/daemon/opcua_server.cc`: own store, address-space registration, and adapter lifecycle.
- `apps/opcua-daemon/main.cc`: load the MQTT snapshot at startup.
- `CMakeLists.txt`: build new libraries, vendored Paho, and tests.

New test files:

- `tests/mqtt_config_validation_test.cc`
- `tests/mqtt_config_repository_test.cc`
- `tests/realtime_value_store_test.cc`
- `tests/realtime_address_space_test.cc`
- `tests/mqtt_payload_parser_test.cc`
- `tests/mqtt_adapter_test.cc`
- `tests/mqtt_integration_test.cc`

Modified tests:

- `tests/api_server_test.cc`: MQTT config API round trip and validation.
- `tests/daemon_smoke_test.cc`: disabled-MQTT startup remains broker-free.

Vendored dependency:

- `third_party/paho.mqtt.c/`: exact upstream commit
  `b830b1d8fe272dca0f6fcb52eab7a69ca67d3a5f` (`v1.3.16`).

## Task 1: MQTT Configuration Domain Model

**Files:**

- Create: `src/config/mqtt_config.h`
- Create: `src/config/mqtt_config.cc`
- Create: `tests/mqtt_config_validation_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing validation test**

Create `tests/mqtt_config_validation_test.cc` with table-driven checks:

```cpp
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

int ExpectInvalid(opcua::MqttConfig config, const char* message) {
  return Expect(!config.Validate().ok(), message);
}

}  // namespace

int main() {
  auto config = opcua::MqttConfig::Default();
  if (int rc = Expect(config.Validate().ok(), "default MQTT config is invalid")) {
    return rc;
  }
  if (int rc = Expect(!config.enabled, "MQTT must default to disabled")) return rc;

  config.enabled = true;
  if (int rc = Expect(config.Validate().ok(), "enabled default config is invalid")) {
    return rc;
  }

  auto invalid = config;
  invalid.broker_uri = "http://localhost:1883";
  if (int rc = ExpectInvalid(invalid, "non-MQTT URI must fail")) return rc;
  invalid = config;
  invalid.broker_uri = "tcp://localhost";
  if (int rc = ExpectInvalid(invalid, "broker URI without port must fail")) return rc;
  invalid = config;
  invalid.broker_uri = "tcp://localhost:70000";
  if (int rc = ExpectInvalid(invalid, "out-of-range broker port must fail")) return rc;
  invalid = config;
  invalid.client_id.clear();
  if (int rc = ExpectInvalid(invalid, "empty client id must fail")) return rc;
  invalid = config;
  invalid.topic = "test/#";
  if (int rc = ExpectInvalid(invalid, "wildcard topic must fail")) return rc;
  invalid = config;
  invalid.qos = 2;
  if (int rc = ExpectInvalid(invalid, "MVP QoS other than 1 must fail")) return rc;
  invalid = config;
  invalid.node_id = 0;
  if (int rc = ExpectInvalid(invalid, "zero NodeId must fail")) return rc;
  invalid = config;
  invalid.data_type = "string";
  if (int rc = ExpectInvalid(invalid, "unsupported scalar type must fail")) return rc;
  invalid = config;
  invalid.stale_timeout_ms = 0;
  return ExpectInvalid(invalid, "non-positive stale timeout must fail");
}
```

Add a temporary CMake test target linked to `opcua_config`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target mqtt_config_validation_test
```

Expected: compilation fails because `config/mqtt_config.h` does not exist.

- [ ] **Step 3: Implement the minimal configuration type**

Create this public contract in `src/config/mqtt_config.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_
#define OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_

#include <cstdint>
#include <string>

#include "common/result.h"

namespace opcua {

struct MqttConfig {
  bool enabled;
  std::string broker_uri;
  std::string client_id;
  std::string topic;
  int qos;
  std::uint32_t node_id;
  std::string browse_name;
  std::string data_type;
  int stale_timeout_ms;

  static MqttConfig Default();
  Status Validate() const;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_MQTT_CONFIG_H_
```

Implement defaults and explicit validation in `src/config/mqtt_config.cc`:

```cpp
MqttConfig MqttConfig::Default() {
  return MqttConfig{false,
                    "tcp://127.0.0.1:1883",
                    "opcua-server",
                    "test/temperature",
                    1,
                    1001U,
                    "Temperature",
                    "double",
                    5000};
}

Status MqttConfig::Validate() const {
  if (!IsValidTcpBrokerUri(broker_uri)) {
    return Status::Error("mqtt.broker_uri must be tcp://host:port");
  }
  if (client_id.empty()) return Status::Error("mqtt.client_id must not be empty");
  if (topic.empty() || topic.find_first_of("+#") != std::string::npos) {
    return Status::Error("mqtt.topic must be a concrete topic without wildcards");
  }
  if (qos != 1) return Status::Error("mqtt.qos must be 1 in the MVP");
  if (node_id == 0U) return Status::Error("mqtt.node_id must be positive");
  if (browse_name.empty()) {
    return Status::Error("mqtt.browse_name must not be empty");
  }
  if (data_type != "boolean" && data_type != "int64" &&
      data_type != "double") {
    return Status::Error("mqtt.data_type must be boolean, int64, or double");
  }
  if (stale_timeout_ms <= 0) {
    return Status::Error("mqtt.stale_timeout_ms must be positive");
  }
  return Status::Ok();
}
```

Implement `IsValidTcpBrokerUri` in the `.cc` anonymous namespace. Require a
non-empty host, a final decimal port in `1..65535`, no path, query, whitespace,
or embedded NUL, and accept bracketed IPv6 such as `tcp://[::1]:1883`.

Add `src/config/mqtt_config.cc` to `opcua_config`.

- [ ] **Step 4: Run the focused test**

Run:

```powershell
cmake --build build --config Debug --target mqtt_config_validation_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R mqtt_config_validation_test --output-on-failure
```

Expected: `mqtt_config_validation_test` passes.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/config/mqtt_config.h src/config/mqtt_config.cc tests/mqtt_config_validation_test.cc
git commit -m "feat: add MQTT configuration model"
```

## Task 2: Normalized SQLite MQTT Persistence

**Files:**

- Modify: `src/config/config_repository.h`
- Modify: `src/config/config_repository.cc`
- Create: `tests/mqtt_config_repository_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing repository tests**

The test must initialize a temporary database, verify defaults, save an enabled
configuration, reload it, and verify that invalid saves preserve prior data:

```cpp
auto repository_result = opcua::ConfigRepository::Open(db_path);
if (!repository_result.ok()) return 1;
auto& repository = repository_result.value();
if (!repository.Initialize().ok()) return 1;

auto defaults = repository.LoadMqtt();
if (!defaults.ok() || defaults.value().enabled) return 1;

auto updated = defaults.value();
updated.enabled = true;
updated.broker_uri = "tcp://127.0.0.1:2883";
updated.node_id = 2001U;
if (!repository.SaveMqtt(updated).ok()) return 1;

auto loaded = repository.LoadMqtt();
if (!loaded.ok() || !loaded.value().enabled ||
    loaded.value().broker_uri != "tcp://127.0.0.1:2883" ||
    loaded.value().node_id != 2001U) {
  return 1;
}

auto invalid = updated;
invalid.qos = 2;
if (repository.SaveMqtt(invalid).ok()) return 1;
loaded = repository.LoadMqtt();
return loaded.ok() && loaded.value().qos == 1 ? 0 : 1;
```

Also query `sqlite_master` and assert that `mqtt_sources`, `data_nodes`, and
`mqtt_mappings` exist alongside the existing `config` table.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target mqtt_config_repository_test
```

Expected: compilation fails because `LoadMqtt` and `SaveMqtt` are missing.

- [ ] **Step 3: Add repository methods and schema**

Add to `ConfigRepository`:

```cpp
Result<MqttConfig> LoadMqtt();
Status SaveMqtt(const MqttConfig& config);
```

Extend `Initialize()` with one transaction containing these idempotent tables:

```sql
CREATE TABLE IF NOT EXISTS mqtt_sources(
  source_id INTEGER PRIMARY KEY CHECK(source_id = 1),
  enabled INTEGER NOT NULL CHECK(enabled IN (0, 1)),
  broker_uri TEXT NOT NULL,
  client_id TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS data_nodes(
  node_id INTEGER PRIMARY KEY,
  browse_name TEXT NOT NULL,
  data_type TEXT NOT NULL,
  stale_timeout_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS mqtt_mappings(
  source_id INTEGER NOT NULL REFERENCES mqtt_sources(source_id),
  topic TEXT NOT NULL,
  qos INTEGER NOT NULL,
  node_id INTEGER NOT NULL REFERENCES data_nodes(node_id),
  PRIMARY KEY(source_id, topic)
);
```

Insert `MqttConfig::Default()` with `INSERT OR IGNORE`. Implement `SaveMqtt`
using `BEGIN IMMEDIATE`, an upsert for source and node,
`DELETE FROM mqtt_mappings WHERE source_id=1`, one mapping insert, deletion of
unreferenced `data_nodes`, and `COMMIT`; execute `ROLLBACK` on every failure.
This gives the complete-object API replacement semantics when topic or NodeId
changes. Implement `LoadMqtt` with one explicit join:

```sql
SELECT s.enabled, s.broker_uri, s.client_id,
       m.topic, m.qos, n.node_id, n.browse_name,
       n.data_type, n.stale_timeout_ms
FROM mqtt_sources AS s
JOIN mqtt_mappings AS m ON m.source_id = s.source_id
JOIN data_nodes AS n ON n.node_id = m.node_id
WHERE s.source_id = 1;
```

Require exactly one row and call `Validate()` before returning it.

- [ ] **Step 4: Run repository and existing configuration tests**

Run:

```powershell
cmake --build build --config Debug --target mqtt_config_repository_test config_repository_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R "mqtt_config_repository_test|config_repository_test" --output-on-failure
```

Expected: both tests pass, including WAL and existing server configuration
round trips.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/config/config_repository.h src/config/config_repository.cc tests/mqtt_config_repository_test.cc
git commit -m "feat: persist MQTT mapping configuration"
```

## Task 3: Extract Configuration JSON Codec

**Files:**

- Create: `src/supervisor/config_json_codec.h`
- Create: `src/supervisor/config_json_codec.cc`
- Modify: `src/supervisor/api_server.cc`
- Modify: `CMakeLists.txt`
- Test: `tests/api_server_test.cc`

- [ ] **Step 1: Preserve API behavior with the existing test**

Run the current test before extraction:

```powershell
cmake --build build --config Debug --target api_server_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R api_server_test --output-on-failure
```

Expected: `api_server_test` passes before the refactor.

- [ ] **Step 2: Define the codec API**

Create `src/supervisor/config_json_codec.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_CONFIG_JSON_CODEC_H_

#include <string>

#include "common/result.h"
#include "config/mqtt_config.h"
#include "config/server_config.h"

namespace opcua {

Result<ServerConfig> ParseServerConfigJson(const std::string& input);
std::string ServerConfigToJson(const ServerConfig& config);
Result<MqttConfig> ParseMqttConfigJson(const std::string& input);
std::string MqttConfigToJson(const MqttConfig& config);
std::string JsonError(const std::string& message);

}  // namespace opcua

#endif
```

- [ ] **Step 3: Move parser code without behavioral changes**

Move UTF-8 validation, string escaping, flat object parsing, and the existing
server configuration builder from `api_server.cc` into
`config_json_codec.cc`. Extend `JsonValueKind` with Boolean:

```cpp
enum class JsonValueKind { kString, kInteger, kBoolean };

struct JsonValue {
  JsonValueKind kind = JsonValueKind::kString;
  std::string string_value;
  std::int64_t integer_value = 0;
  bool boolean_value = false;
};
```

The object parser accepts `true` and `false` only when the next non-whitespace
character is neither a quote nor an integer prefix. Keep the 65,536-byte limit,
duplicate-key rejection, unknown-field rejection, strict UTF-8 handling, and
complete-object semantics unchanged for server config. Parse JSON integers to
`std::int64_t`; field readers perform checked conversion to `int` or
`std::uint32_t`, so the existing server-port overflow test still fails and MQTT
NodeIds through `UINT32_MAX` are representable.

Replace local parser calls in `api_server.cc` with
`ParseServerConfigJson`, `ServerConfigToJson`, and `JsonError`.

- [ ] **Step 4: Run the API regression test**

Run:

```powershell
cmake --build build --config Debug --target api_server_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R api_server_test --output-on-failure
```

Expected: every existing API test passes with identical response bodies.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/supervisor/config_json_codec.h src/supervisor/config_json_codec.cc src/supervisor/api_server.cc
git commit -m "refactor: extract configuration JSON codec"
```

## Task 4: MQTT Configuration API

**Files:**

- Modify: `src/supervisor/config_json_codec.cc`
- Modify: `src/supervisor/api_server.cc`
- Modify: `tests/api_server_test.cc`

- [ ] **Step 1: Add failing GET and PUT tests**

Add these assertions to `api_server_test.cc`:

```cpp
const std::string mqtt_json =
    "{\"enabled\":true,"
    "\"broker_uri\":\"tcp://127.0.0.1:2883\","
    "\"client_id\":\"test-client\","
    "\"topic\":\"test/temperature\","
    "\"qos\":1,"
    "\"node_id\":2001,"
    "\"browse_name\":\"Temperature\","
    "\"data_type\":\"double\","
    "\"stale_timeout_ms\":2500}";

auto put = fixture->Client().Put("/api/v1/mqtt-config", mqtt_json,
                                 "application/json");
if (!put || put->status != 200 || put->body != "{\"status\":\"ok\"}") return 1;

auto get = fixture->Client().Get("/api/v1/mqtt-config");
if (!get || get->status != 200 || get->body != mqtt_json) return 1;

auto invalid = fixture->Client().Put(
    "/api/v1/mqtt-config",
    "{\"enabled\":true,\"broker_uri\":\"http://bad\","
    "\"client_id\":\"x\",\"topic\":\"t\",\"qos\":1,\"node_id\":1,"
    "\"browse_name\":\"n\",\"data_type\":\"double\","
    "\"stale_timeout_ms\":1}",
    "application/json");
return invalid && invalid->status == 400 ? 0 : 1;
```

Add a helper that sends a body and requires HTTP 400, then call it with these
exact invalid documents:

```cpp
int ExpectBadMqttPut(ApiFixture* fixture, const std::string& body) {
  auto response = fixture->Client().Put("/api/v1/mqtt-config", body,
                                        "application/json");
  return Expect(response && response->status == 400,
                "invalid MQTT config must return 400");
}

if (int rc = ExpectBadMqttPut(
        fixture, "{\"enabled\":true,\"broker_uri\":\"tcp://x:1883\","
                 "\"client_id\":\"c\",\"topic\":\"t\",\"qos\":1,"
                 "\"node_id\":1,\"browse_name\":\"n\","
                 "\"data_type\":\"double\"}")) return rc;
if (int rc = ExpectBadMqttPut(
        fixture, "{\"enabled\":true,\"enabled\":false,"
                 "\"broker_uri\":\"tcp://x:1883\",\"client_id\":\"c\","
                 "\"topic\":\"t\",\"qos\":1,\"node_id\":1,"
                 "\"browse_name\":\"n\",\"data_type\":\"double\","
                 "\"stale_timeout_ms\":1}")) return rc;
if (int rc = ExpectBadMqttPut(
        fixture, "{\"enabled\":true,\"unknown\":0,"
                 "\"broker_uri\":\"tcp://x:1883\",\"client_id\":\"c\","
                 "\"topic\":\"t\",\"qos\":1,\"node_id\":1,"
                 "\"browse_name\":\"n\",\"data_type\":\"double\","
                 "\"stale_timeout_ms\":1}")) return rc;
if (int rc = ExpectBadMqttPut(
        fixture, "{\"enabled\":1,\"broker_uri\":\"tcp://x:1883\","
                 "\"client_id\":\"c\",\"topic\":\"t\",\"qos\":1,"
                 "\"node_id\":1,\"browse_name\":\"n\","
                 "\"data_type\":\"double\",\"stale_timeout_ms\":1}")) return rc;
if (int rc = ExpectBadMqttPut(
        fixture, "{\"enabled\":true,\"broker_uri\":\"tcp://x:1883\","
                 "\"client_id\":\"a\\u0000b\",\"topic\":\"t\",\"qos\":1,"
                 "\"node_id\":1,\"browse_name\":\"n\","
                 "\"data_type\":\"double\",\"stale_timeout_ms\":1}")) return rc;
if (int rc = ExpectBadMqttPut(fixture, std::string(65537, ' '))) return rc;
```

- [ ] **Step 2: Run the test to verify 404 failure**

Run:

```powershell
cmake --build build --config Debug --target api_server_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R api_server_test --output-on-failure
```

Expected: failure because `/api/v1/mqtt-config` is not registered.

- [ ] **Step 3: Implement the flat MQTT JSON codec and routes**

Implement `ParseMqttConfigJson` as a complete nine-field object and
`MqttConfigToJson` in the exact field order used by the test. The builder must
read:

```cpp
MqttConfig config;
ReadBoolean(object, "enabled", &config.enabled);
ReadString(object, "broker_uri", &config.broker_uri);
ReadString(object, "client_id", &config.client_id);
ReadString(object, "topic", &config.topic);
ReadInteger(object, "qos", &config.qos);
ReadUnsignedInteger(object, "node_id", &config.node_id);
ReadString(object, "browse_name", &config.browse_name);
ReadString(object, "data_type", &config.data_type);
ReadInteger(object, "stale_timeout_ms", &config.stale_timeout_ms);
```

Register routes in `ApiServer::Impl`:

```cpp
server_.Get("/api/v1/mqtt-config",
            [this](const httplib::Request&, httplib::Response& res) {
              std::lock_guard<std::mutex> lock(operations_mutex_);
              auto result = repository_->LoadMqtt();
              if (!result.ok()) {
                SetError(&res, 500, result.status().message());
                return;
              }
              SetJson(&res, 200, MqttConfigToJson(result.value()));
            });

server_.Put("/api/v1/mqtt-config",
            [this](const httplib::Request& req, httplib::Response& res) {
              auto result = ParseMqttConfigJson(req.body);
              if (!result.ok()) {
                SetError(&res, 400, result.status().message());
                return;
              }
              std::lock_guard<std::mutex> lock(operations_mutex_);
              auto status = repository_->SaveMqtt(result.value());
              SetLifecycleResult(status, &res);
            });
```

Return HTTP 400 for parse and validation failures and HTTP 500 only for
repository failures.

- [ ] **Step 4: Run API and repository tests**

Run:

```powershell
cmake --build build --config Debug --target api_server_test mqtt_config_repository_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R "api_server_test|mqtt_config_repository_test" --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/supervisor/config_json_codec.cc src/supervisor/api_server.cc tests/api_server_test.cc
git commit -m "feat: expose MQTT configuration API"
```

## Task 5: Realtime Value Store

**Files:**

- Create: `src/daemon/realtime_value_store.h`
- Create: `src/daemon/realtime_value_store.cc`
- Create: `tests/realtime_value_store_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing state-transition tests**

Create slots for every type and verify the full lifecycle:

```cpp
opcua::RealtimeValueStore store;
const auto slot = store.AddSlot(opcua::ScalarType::kDouble);

auto initial_result = store.ReadSnapshot(slot);
if (!initial_result.ok()) return 1;
const auto initial = initial_result.value();
if (initial.has_value ||
    initial.status != UA_STATUSCODE_BADWAITINGFORINITIALDATA ||
    initial.sequence != 0U) {
  return 1;
}

const UA_DateTime first_time = UA_DateTime_now();
if (!store.Update(slot, 37.5, first_time)) return 1;
auto good = store.ReadSnapshot(slot).value();
if (!good.has_value || std::get<double>(good.value) != 37.5 ||
    good.status != UA_STATUSCODE_GOOD || good.sequence != 1U) {
  return 1;
}

if (store.Update(slot, 37.5, first_time + UA_DATETIME_MSEC)) return 1;
auto repeated = store.ReadSnapshot(slot).value();
if (repeated.sequence != 1U ||
    repeated.source_timestamp != first_time + UA_DATETIME_MSEC) {
  return 1;
}

if (!store.MarkUnavailable(slot)) return 1;
auto unavailable = store.ReadSnapshot(slot).value();
if (unavailable.status !=
        UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE ||
    std::get<double>(unavailable.value) != 37.5 ||
    unavailable.sequence != 2U) {
  return 1;
}

if (!store.Update(slot, 37.5, first_time + 2 * UA_DATETIME_MSEC)) return 1;
return store.ReadSnapshot(slot).value().status == UA_STATUSCODE_GOOD ? 0 : 1;
```

Add these type, disabled, invalid-ID, and concurrent checks:

```cpp
const auto boolean_slot = store.AddSlot(opcua::ScalarType::kBoolean);
const auto integer_slot = store.AddSlot(opcua::ScalarType::kInt64);
const auto disabled_slot = store.AddSlot(opcua::ScalarType::kDouble, false);
if (!store.Update(boolean_slot, true, first_time)) return 1;
if (!store.Update(integer_slot, std::int64_t{42}, first_time)) return 1;
if (!std::get<bool>(store.ReadSnapshot(boolean_slot).value().value)) return 1;
if (std::get<std::int64_t>(
        store.ReadSnapshot(integer_slot).value().value) != 42) return 1;
if (store.ReadSnapshot(disabled_slot).value().status !=
    UA_STATUSCODE_BADOUTOFSERVICE) return 1;
if (store.ReadSnapshot(99999U).ok()) return 1;

std::atomic_bool run{true};
std::thread writer([&] {
  for (std::int64_t value = 0; value < 10000; ++value) {
    store.Update(integer_slot, value, first_time + value);
  }
  run.store(false);
});
while (run.load()) {
  auto snapshot = store.ReadSnapshot(integer_slot);
  if (!snapshot.ok() ||
      !std::holds_alternative<std::int64_t>(snapshot.value().value)) {
    writer.join();
    return 1;
  }
}
writer.join();
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target realtime_value_store_test
```

Expected: compilation fails because the store does not exist.

- [ ] **Step 3: Implement stable synchronized slots**

Define:

```cpp
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
};
```

`ValueSlot` owns a mutex and one snapshot. The store owns one source-health
mutex and snapshot for the MVP source. `AddSlot` stores
`BadWaitingForInitialData` when enabled and `BadOutOfService` when disabled.
`Update` rejects type mismatches, always refreshes SourceTimestamp, changes
quality to Good, records the last successful source update, resets consecutive
failures, and increments sequence only when value or quality changed.
`MarkUnavailable` keeps the value and applies
`UncertainNoCommunicationLastUsableValue`; without a value it keeps
`BadWaitingForInitialData`. `SetSourceDisconnected` increments consecutive
failures once per connected-to-disconnected transition.

- [ ] **Step 4: Run the store test repeatedly**

Run:

```powershell
cmake --build build --config Debug --target realtime_value_store_test
1..20 | ForEach-Object { & .\build\Debug\realtime_value_store_test.exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
```

Expected: all 20 runs exit with code 0.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon/realtime_value_store.h src/daemon/realtime_value_store.cc tests/realtime_value_store_test.cc
git commit -m "feat: add realtime latest-value store"
```

## Task 6: Callback-Backed OPC UA Address Space

**Files:**

- Create: `src/daemon/realtime_address_space.h`
- Create: `src/daemon/realtime_address_space.cc`
- Create: `tests/realtime_address_space_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing local Read and access-level tests**

Construct an open62541 server, add one callback Node, and use the local service
API:

```cpp
opcua::RealtimeValueStore store;
const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
store.Update(slot, 37.5, UA_DateTime_now());

opcua::RealtimeAddressSpace address_space(&store);
UA_Server* server = UA_Server_new();
if (server == nullptr) return 1;

opcua::RealtimeNodeConfig node{
    1001U, "Temperature", opcua::ScalarType::kDouble, slot};
if (!address_space.AddNode(server, node).ok()) return 1;

UA_ReadValueId read_id;
UA_ReadValueId_init(&read_id);
read_id.nodeId = UA_NODEID_NUMERIC(2, 1001U);
read_id.attributeId = UA_ATTRIBUTEID_VALUE;
UA_DataValue value =
    UA_Server_read(server, &read_id, UA_TIMESTAMPSTORETURN_SOURCE);
if (!value.hasValue || !value.hasStatus ||
    value.status != UA_STATUSCODE_GOOD ||
    *static_cast<UA_Double*>(value.value.data) != 37.5) {
  return 1;
}
UA_DataValue_clear(&value);

UA_Byte access_level = 0;
const auto access_status = UA_Server_readAccessLevel(
    server, UA_NODEID_NUMERIC(2, 1001U), &access_level);
if (access_status != UA_STATUSCODE_GOOD ||
    access_level != UA_ACCESSLEVELMASK_READ) return 1;
```

Verify initial and unavailable callback values with the same local read API:

```cpp
opcua::RealtimeValueStore waiting_store;
const auto waiting_slot =
    waiting_store.AddSlot(opcua::ScalarType::kDouble);
opcua::RealtimeAddressSpace waiting_space(&waiting_store);
if (!waiting_space.AddNode(
        server, {1002U, "Waiting", opcua::ScalarType::kDouble,
                 waiting_slot}).ok()) return 1;

read_id.nodeId = UA_NODEID_NUMERIC(2, 1002U);
value = UA_Server_read(server, &read_id, UA_TIMESTAMPSTORETURN_SOURCE);
if (value.hasValue || !value.hasStatus ||
    value.status != UA_STATUSCODE_BADWAITINGFORINITIALDATA) return 1;
UA_DataValue_clear(&value);

waiting_store.Update(waiting_slot, 11.0, UA_DateTime_now());
waiting_store.MarkUnavailable(waiting_slot);
value = UA_Server_read(server, &read_id, UA_TIMESTAMPSTORETURN_SOURCE);
if (!value.hasValue ||
    value.status !=
        UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE ||
    *static_cast<UA_Double*>(value.value.data) != 11.0) return 1;
UA_DataValue_clear(&value);
UA_Server_delete(server);
return 0;
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target realtime_address_space_test
```

Expected: compilation fails because `RealtimeAddressSpace` does not exist.

- [ ] **Step 3: Implement Node contexts and the read callback**

Define:

```cpp
struct RealtimeNodeConfig {
  std::uint32_t node_id;
  std::string browse_name;
  ScalarType type;
  ValueSlotId slot_id;
};

class RealtimeAddressSpace {
 public:
  explicit RealtimeAddressSpace(RealtimeValueStore* store);
  Status AddNode(UA_Server* server, const RealtimeNodeConfig& config);

 private:
  struct NodeContext;
  static UA_StatusCode Read(UA_Server* server, const UA_NodeId* session_id,
                            void* session_context, const UA_NodeId* node_id,
                            void* node_context,
                            UA_Boolean include_source_timestamp,
                            const UA_NumericRange* range,
                            UA_DataValue* value);

  RealtimeValueStore* store_;
  std::vector<std::unique_ptr<NodeContext>> contexts_;
};
```

Create the data Node under `Objects` with namespace index 2, a numeric NodeId,
scalar data types, `UA_ACCESSLEVELMASK_READ`, and a null write callback. The read
callback obtains a snapshot, uses `UA_Variant_setScalarCopy`, sets
`hasStatus/status`, and sets SourceTimestamp only when available and requested.
Return `BadIndexRangeInvalid` for a non-null range in the scalar MVP.

Also create an `MqttSource` Object with these read-only callback Variables:

```text
ns=2;s=MqttSource.ConnectionState       String
ns=2;s=MqttSource.LastSuccessfulUpdate  DateTime
ns=2;s=MqttSource.ConsecutiveFailures   UInt32
```

Their callbacks read `SourceHealthSnapshot`; they do not retain pointers to the
MQTT adapter. Extend the test to read each diagnostic Node before and after
source-health transitions.

- [ ] **Step 4: Run focused tests**

Run:

```powershell
cmake --build build --config Debug --target realtime_address_space_test realtime_value_store_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R "realtime_address_space_test|realtime_value_store_test" --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon/realtime_address_space.h src/daemon/realtime_address_space.cc tests/realtime_address_space_test.cc
git commit -m "feat: expose callback-backed realtime nodes"
```

## Task 7: Bounded MQTT Scalar Parser

**Files:**

- Create: `src/daemon/mqtt_payload_parser.h`
- Create: `src/daemon/mqtt_payload_parser.cc`
- Create: `tests/mqtt_payload_parser_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing parser tests**

Test exact accepted and rejected inputs:

```cpp
using opcua::ParseMqttScalar;
using opcua::ScalarType;

auto boolean = ParseMqttScalar("true", ScalarType::kBoolean);
if (!boolean.ok() || !std::get<bool>(boolean.value())) return 1;
auto integer = ParseMqttScalar("-9223372036854775808", ScalarType::kInt64);
if (!integer.ok() ||
    std::get<std::int64_t>(integer.value()) != INT64_MIN) return 1;
auto number = ParseMqttScalar("37.5", ScalarType::kDouble);
if (!number.ok() || std::get<double>(number.value()) != 37.5) return 1;

if (ParseMqttScalar("TRUE", ScalarType::kBoolean).ok()) return 1;
if (ParseMqttScalar("1.0x", ScalarType::kDouble).ok()) return 1;
if (ParseMqttScalar("nan", ScalarType::kDouble).ok()) return 1;
if (ParseMqttScalar("", ScalarType::kInt64).ok()) return 1;
if (ParseMqttScalar(std::string(129, '1'), ScalarType::kDouble).ok()) return 1;
return 0;
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target mqtt_payload_parser_test
```

Expected: compilation fails because `ParseMqttScalar` is missing.

- [ ] **Step 3: Implement strict allocation-bounded parsing**

Expose:

```cpp
constexpr std::size_t kMaxMqttScalarPayloadBytes = 128;
Result<ScalarValue> ParseMqttScalar(std::string_view payload,
                                    ScalarType type);
```

Use exact comparison for Boolean and `std::from_chars` for Int64 and Double.
Require the returned pointer to equal `payload.data() + payload.size()`.
Reject empty values, leading/trailing whitespace, non-finite Double values,
embedded NUL, and payloads larger than 128 bytes.

- [ ] **Step 4: Run the parser test**

Run:

```powershell
cmake --build build --config Debug --target mqtt_payload_parser_test
& .\build\Debug\mqtt_payload_parser_test.exe
```

Expected: exit code 0.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon/mqtt_payload_parser.h src/daemon/mqtt_payload_parser.cc tests/mqtt_payload_parser_test.cc
git commit -m "feat: parse bounded MQTT scalar payloads"
```

## Task 8: Vendor And Build Eclipse Paho MQTT C

**Files:**

- Create: `third_party/paho.mqtt.c/` from pinned upstream commit.
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Record the clean dependency baseline**

Run:

```powershell
git status --short
git ls-remote https://github.com/eclipse-paho/paho.mqtt.c.git refs/tags/v1.3.16
```

Expected: the tag resolves to
`b830b1d8fe272dca0f6fcb52eab7a69ca67d3a5f`. Preserve any unrelated working
tree entries and do not stage them.

- [ ] **Step 2: Import the pinned source**

Run from the isolated feature worktree:

```powershell
git remote add paho-upstream https://github.com/eclipse-paho/paho.mqtt.c.git
git fetch --depth=1 paho-upstream b830b1d8fe272dca0f6fcb52eab7a69ca67d3a5f
git read-tree --prefix=third_party/paho.mqtt.c/ -u FETCH_HEAD
git remote remove paho-upstream
```

Expected: the Paho release source, `LICENSE`, `NOTICE`, and CMake files appear
under `third_party/paho.mqtt.c`.

- [ ] **Step 3: Configure a static high-performance Paho target**

Add before `opcua_daemon_lib`:

```cmake
set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PAHO_WITH_SSL OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(PAHO_HIGH_PERFORMANCE ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/paho.mqtt.c EXCLUDE_FROM_ALL)
```

Link `paho-mqtt3a-static` only to the daemon library. Do not apply project
warning-as-error policy to Paho.

- [ ] **Step 4: Configure and build on MSVC**

Run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --target paho-mqtt3a-static opcua-daemon
```

Expected: both static Paho and `opcua-daemon.exe` build without requiring
OpenSSL.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt third_party/paho.mqtt.c
git commit -m "build: vendor Eclipse Paho MQTT C 1.3.16"
```

## Task 9: MQTT Adapter And Source Health

**Files:**

- Create: `src/daemon/mqtt_adapter.h`
- Create: `src/daemon/mqtt_adapter.cc`
- Create: `tests/mqtt_adapter_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write a broker-free adapter state test**

Separate message and health handling from Paho callback glue so this test can
invoke them directly:

```cpp
opcua::RealtimeValueStore store;
const auto slot = store.AddSlot(opcua::ScalarType::kDouble);
auto config = opcua::MqttConfig::Default();
config.enabled = true;
config.stale_timeout_ms = 50;

opcua::MqttAdapter adapter(config, opcua::ScalarType::kDouble, &store, slot);
if (!adapter.AcceptMessage("test/temperature", "37.5", UA_DateTime_now()).ok()) {
  return 1;
}
if (std::get<double>(store.ReadSnapshot(slot).value().value) != 37.5) return 1;

adapter.NotifyConnectionLost();
auto lost = store.ReadSnapshot(slot).value();
if (lost.status !=
    UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE) {
  return 1;
}

if (!adapter.AcceptMessage("test/temperature", "37.5", UA_DateTime_now()).ok()) {
  return 1;
}
if (store.ReadSnapshot(slot).value().status != UA_STATUSCODE_GOOD) return 1;
adapter.PollHealth(std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(100));
if (store.ReadSnapshot(slot).value().status !=
    UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE) return 1;

if (adapter.AcceptMessage("other/topic", "99", UA_DateTime_now()).ok()) return 1;
if (adapter.AcceptMessage("test/temperature", "bad", UA_DateTime_now()).ok()) {
  return 1;
}
return 0;
```

Pass an explicit monotonic time later than the stale deadline to
`PollHealth(now)` without sleeping, then assert that it marks the slot
unavailable.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```powershell
cmake --build build --config Debug --target mqtt_adapter_test
```

Expected: compilation fails because `MqttAdapter` is missing.

- [ ] **Step 3: Implement adapter ownership and callback glue**

Expose:

```cpp
class MqttAdapter {
 public:
  MqttAdapter(MqttConfig config, ScalarType type,
              RealtimeValueStore* store, ValueSlotId slot);
  ~MqttAdapter();

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
  std::chrono::steady_clock::time_point last_valid_message_;
};
```

`Start()` creates the Paho client, registers callbacks and connected callback,
enables automatic reconnect, and starts a small watchdog thread.
`Connected()` subscribes to the concrete topic at QoS 1.
`MessageArrived()` creates bounded `string_view` objects, calls
`AcceptMessage`, and always frees the Paho message and topic name.
`ConnectionLost()` calls `NotifyConnectionLost`.
`Stop()` first clears `accepting_`, sets `watchdog_stop_` while holding
`health_mutex_`, notifies and joins `watchdog_thread_`, disconnects, and
destroys the Paho client.

The production watchdog passes `std::chrono::steady_clock::now()` to
`PollHealth`. The broker-free test sets a known `last_valid_message_` through
`AcceptMessage`, then passes an explicit later time to `PollHealth`; no sleep or
clock mock is required. Connection callbacks update the source-health snapshot
in `RealtimeValueStore`.

Log callback failures without including payload contents. Keep parse errors
rate-limited with one aggregate count per reporting interval.

- [ ] **Step 4: Run adapter, parser, and store tests**

Run:

```powershell
cmake --build build --config Debug --target mqtt_adapter_test mqtt_payload_parser_test realtime_value_store_test
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug -R "mqtt_adapter_test|mqtt_payload_parser_test|realtime_value_store_test" --output-on-failure
```

Expected: all three tests pass without a running broker.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon/mqtt_adapter.h src/daemon/mqtt_adapter.cc tests/mqtt_adapter_test.cc
git commit -m "feat: add asynchronous MQTT source adapter"
```

## Task 10: Daemon Lifecycle Integration

**Files:**

- Modify: `src/daemon/opcua_server.h`
- Modify: `src/daemon/opcua_server.cc`
- Modify: `apps/opcua-daemon/main.cc`
- Modify: `tests/daemon_smoke_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add a failing disabled-MQTT smoke assertion**

Update the smoke helper to pass both snapshots:

```cpp
opcua::Status RunServerBriefly(const opcua::ServerConfig& server_config,
                               const opcua::MqttConfig& mqtt_config) {
  opcua::OpcuaServer server(server_config, mqtt_config);
  std::atomic_bool running(true);
  opcua::Status run_status = opcua::Status::Ok();
  std::thread server_thread([&server, &running, &run_status]() {
    run_status = server.Run(&running);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  running.store(false);
  server_thread.join();
  return run_status;
}
```

Load `repository.LoadMqtt()`, assert that defaults are disabled, and pass the
snapshot into every smoke run. The test must succeed without Mosquitto.

- [ ] **Step 2: Run the test to verify constructor failure**

Run:

```powershell
cmake --build build --config Debug --target daemon_smoke_test
```

Expected: compilation fails because `OpcuaServer` does not accept `MqttConfig`.

- [ ] **Step 3: Integrate ownership in the required order**

Change the constructor:

```cpp
OpcuaServer(ServerConfig server_config, MqttConfig mqtt_config);
```

Declare ownership in this exact order during `Run()` so reverse destruction
keeps every callback dependency alive:

```cpp
RealtimeValueStore value_store;
const ScalarType type = ScalarTypeFromConfig(mqtt_config_.data_type);
const ValueSlotId slot = value_store.AddSlot(type, mqtt_config_.enabled);

RealtimeAddressSpace address_space(&value_store);
ServerPtr server(UA_Server_new());
if (server == nullptr) return Status::Error("UA_Server_new failed");

RealtimeNodeConfig node{mqtt_config_.node_id, mqtt_config_.browse_name,
                        type, slot};
auto node_status = address_space.AddNode(server.get(), node);
if (!node_status.ok()) return node_status;

std::unique_ptr<MqttAdapter> mqtt_adapter;
if (mqtt_config_.enabled) {
  mqtt_adapter = std::make_unique<MqttAdapter>(
      mqtt_config_, type, &value_store, slot);
  auto mqtt_status = mqtt_adapter->Start();
  if (!mqtt_status.ok()) return mqtt_status;
}
```

After all Nodes are registered, call `UA_Server_run_startup`, create
`ServerShutdown`, and only then construct and start `mqtt_adapter`. The
resulting destruction order is MQTT adapter, open62541 shutdown and deletion,
address-space contexts, then value store. In `apps/opcua-daemon/main.cc`, call
`LoadMqtt()` after `Load()` and fail fast if either snapshot is invalid.

- [ ] **Step 4: Run all broker-free tests**

Run:

```powershell
cmake --build build --config Debug
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug --output-on-failure
```

Expected: every default test passes without Mosquitto.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon/opcua_server.h src/daemon/opcua_server.cc apps/opcua-daemon/main.cc tests/daemon_smoke_test.cc
git commit -m "feat: integrate realtime MQTT path into daemon"
```

## Task 11: Opt-In Mosquitto End-To-End Test

**Files:**

- Create: `tests/mqtt_integration_test.cc`
- Modify: `CMakeLists.txt`
- Modify: `docs/superpowers/status/2026-07-14-checkpoint.md`

- [ ] **Step 1: Add the disabled-by-default CMake test target**

Add:

```cmake
option(OPCUA_MQTT_INTEGRATION_TESTS
  "Run tests that launch an external Mosquitto executable" OFF)
set(MOSQUITTO_EXECUTABLE "" CACHE FILEPATH "Path to the Mosquitto broker")

if(OPCUA_BUILD_TESTS AND OPCUA_MQTT_INTEGRATION_TESTS)
  if(NOT EXISTS "${MOSQUITTO_EXECUTABLE}")
    message(FATAL_ERROR
      "OPCUA_MQTT_INTEGRATION_TESTS requires MOSQUITTO_EXECUTABLE")
  endif()
  add_executable(mqtt_integration_test tests/mqtt_integration_test.cc)
  target_link_libraries(mqtt_integration_test PRIVATE
    opcua_daemon_lib opcua_supervisor_lib paho-mqtt3a-static)
  opcua_target_warnings(mqtt_integration_test)
  add_test(NAME mqtt_integration_test
    COMMAND mqtt_integration_test
      "${MOSQUITTO_EXECUTABLE}"
      $<TARGET_FILE:opcua-daemon>)
  set_tests_properties(mqtt_integration_test PROPERTIES TIMEOUT 30)
endif()
```

Configure with integration tests on and verify that the missing test source
causes generation or build failure.

- [ ] **Step 2: Implement the end-to-end test harness**

The test must:

1. Select free candidate ports from MQTT `18883..18892` and OPC UA
   `48450..48459`.
2. Write a temporary Mosquitto config containing:

```text
listener <selected MQTT port> 127.0.0.1
allow_anonymous true
persistence false
```

3. Initialize SQLite with an enabled MQTT mapping and selected ports.
4. Launch Mosquitto and the real `opcua-daemon` using `ProcessController`.
5. Publish `37.5` at QoS 1 using a Paho asynchronous client.
6. Connect an open62541 client, read `ns=2;i=1001`, and assert Double `37.5`,
   Good quality, and SourceTimestamp.
7. Attempt a remote client Write of `99.0`, assert `BadNotWritable`, and
   re-read `37.5`.
8. Create a Subscription with `STATUS_VALUE`, publish `38.0`, and assert one
   data-change callback.
9. Stop Mosquitto, wait for
   `UncertainNoCommunicationLastUsableValue`, and assert the retained value is
   `38.0`.
10. Restart Mosquitto, publish `38.0`, and assert a notification returning the
   status to Good even though the numeric value is unchanged.
11. Stop the daemon and broker and remove temporary DB, WAL, SHM, and config
    files through RAII cleanup.

Use polling helpers with explicit deadlines; no unbounded sleeps or waits.
Organize the test around this concrete harness contract:

```cpp
class IntegrationFixture {
 public:
  static std::unique_ptr<IntegrationFixture> Create(
      const std::string& mosquitto_path,
      const std::string& daemon_path);
  bool StartBroker();
  bool ConfigureDatabase();
  bool StartDaemon();
  bool PublishDouble(double value);
  bool WaitForDataValue(double value, UA_StatusCode status,
                        std::chrono::milliseconds timeout);
  bool WaitForNotification(double value, UA_StatusCode status,
                           std::chrono::milliseconds timeout);
  bool VerifyRemoteWriteRejected();
  bool StopBroker();
  bool RestartBroker();
  void StopAll();
  ~IntegrationFixture();
};

int main(int argc, char** argv) {
  using namespace std::chrono_literals;
  if (argc != 3) return 1;
  auto fixture = IntegrationFixture::Create(argv[1], argv[2]);
  if (!fixture || !fixture->StartBroker() ||
      !fixture->ConfigureDatabase() || !fixture->StartDaemon() ||
      !fixture->PublishDouble(37.5) ||
      !fixture->WaitForDataValue(37.5, UA_STATUSCODE_GOOD, 5s) ||
      !fixture->VerifyRemoteWriteRejected() ||
      !fixture->PublishDouble(38.0) ||
      !fixture->WaitForNotification(38.0, UA_STATUSCODE_GOOD, 5s) ||
      !fixture->StopBroker() ||
      !fixture->WaitForNotification(
          38.0, UA_STATUSCODE_UNCERTAINNOCOMMUNICATIONLASTUSABLEVALUE, 5s) ||
      !fixture->RestartBroker() || !fixture->PublishDouble(38.0) ||
      !fixture->WaitForNotification(38.0, UA_STATUSCODE_GOOD, 5s)) {
    return 1;
  }
  fixture->StopAll();
  return 0;
}
```

- [ ] **Step 3: Run the end-to-end test**

Run with the actual local Mosquitto path:

```powershell
cmake -S . -B build-mqtt -G "Visual Studio 17 2022" -A x64 `
  -DOPCUA_MQTT_INTEGRATION_TESTS=ON `
  -DMOSQUITTO_EXECUTABLE="C:/Program Files/mosquitto/mosquitto.exe"
cmake --build build-mqtt --config Debug --target mqtt_integration_test opcua-daemon
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build-mqtt -C Debug -R mqtt_integration_test --output-on-failure
```

Expected: one integration test passes. If Mosquitto is not installed, install
or provide it before claiming this task complete; do not replace the test with
a broker mock.

- [ ] **Step 4: Update the checkpoint**

Append this verified MQTT MVP subsection while retaining unrelated preexisting
runtime limitations:

```markdown
- Added a callback-backed realtime value store for Boolean, Int64, and Double.
- Added one read-only MQTT scalar mapping configured through SQLite and API.
- Verified value, quality, disconnect, stale, and recovery behavior.
- JSON, Sparkplug B, TLS, multiple mappings, and Historian remain deferred.
```

- [ ] **Step 5: Run the complete verification matrix**

MSVC:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --test-dir build -C Debug --output-on-failure
```

GCC where available:

```powershell
cmake -S . -B build-gcc -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc
ctest --test-dir build-gcc --output-on-failure
```

Clang where available:

```powershell
cmake -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-clang
ctest --test-dir build-clang --output-on-failure
```

Expected: every available native matrix is green, and the opt-in Mosquitto
test is green in the explicitly configured MSVC build.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt tests/mqtt_integration_test.cc docs/superpowers/status/2026-07-14-checkpoint.md
git commit -m "test: verify MQTT realtime path end to end"
```

## Final Review

- [ ] Confirm `git status --short` contains no generated database, WAL, SHM,
      Paho build output, or test configuration files.
- [ ] Confirm the default test suite does not require a running MQTT broker.
- [ ] Confirm the MQTT callback does not call any `UA_Server_*` write API.
- [ ] Confirm every data Node is Read-only.
- [ ] Confirm adapter destruction precedes store and Node-context destruction.
- [ ] Confirm default MQTT configuration is disabled for existing deployments.
- [ ] Confirm API changes persist only and require daemon restart.
- [ ] Confirm the exact Paho source commit and notices are present.
- [ ] Confirm no TLS, JSON, Sparkplug B, historian, or multi-mapping behavior
      leaked into the MVP.
