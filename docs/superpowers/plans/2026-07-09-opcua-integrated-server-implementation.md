# OPC UA Integrated Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first working CMake/C++17 implementation of the OPC UA integrated server with a supervisor Web API, SQLite-backed config, daemon lifecycle control, and vendored open62541/cpp-httplib dependencies.

**Architecture:** The project produces two executables: `opcua-supervisor` and `opcua-daemon`. The supervisor owns HTTP API, SQLite writes, process supervision, PID 1 behavior, and daemon status; the daemon reads a config snapshot from SQLite at startup and runs open62541. The first implementation keeps address space builtin-only and applies config changes through daemon restart.

**Tech Stack:** CMake, C++17, open62541 C API, SQLite C API, cpp-httplib, simple in-repo test executables, Docker.

---

## File Structure

- `CMakeLists.txt`: root build options, compiler settings, dependency wiring, test enablement.
- `cmake/CompilerWarnings.cmake`: GCC/Clang/MSVC warning helpers.
- `third_party/cpp-httplib/httplib.h`: vendored cpp-httplib single header.
- `third_party/open62541/open62541.c`: vendored open62541 amalgamated C source.
- `third_party/open62541/open62541.h`: vendored open62541 amalgamated header.
- `src/common/result.h`: small explicit result type for recoverable errors.
- `src/common/logging.h`, `src/common/logging.cc`: stdout/file logging target setup.
- `src/config/server_config.h`, `src/config/server_config.cc`: config model, defaults, validation.
- `src/config/sqlite_db.h`, `src/config/sqlite_db.cc`: SQLite RAII connection, WAL, busy timeout, transactions.
- `src/config/config_repository.h`, `src/config/config_repository.cc`: schema creation and config read/write.
- `src/daemon/opcua_server.h`, `src/daemon/opcua_server.cc`: open62541 server wrapper.
- `src/supervisor/process_controller.h`, `src/supervisor/process_controller.cc`: child process start/stop/restart/status, signal forwarding, wait/reaping.
- `src/supervisor/api_server.h`, `src/supervisor/api_server.cc`: cpp-httplib routes.
- `apps/opcua-daemon/main.cc`: daemon entry point.
- `apps/opcua-supervisor/main.cc`: supervisor entry point.
- `tests/config_repository_test.cc`: config DB tests.
- `tests/process_controller_test.cc`: lifecycle/status tests with a helper executable.
- `tests/api_server_test.cc`: API endpoint tests.
- `tests/daemon_smoke_test.cc`: daemon startup/config smoke test.
- `tests/test_child/main.cc`: small child process for process controller tests.
- `docker/Dockerfile`: single-container image with both executables.
- `docker/docker-compose.yml`: local run configuration.
- `.gitignore`: build outputs, local DB files, editor noise.
- `README.md`: build, test, run, and API examples.

---

### Task 1: Repository And CMake Scaffold

**Files:**
- Create: `.gitignore`
- Create: `CMakeLists.txt`
- Create: `cmake/CompilerWarnings.cmake`
- Create: `src/common/result.h`
- Create: `apps/opcua-daemon/main.cc`
- Create: `apps/opcua-supervisor/main.cc`

- [ ] **Step 1: Write the minimal scaffold files**

Create `.gitignore`:

```gitignore
build/
out/
.vs/
.vscode/
*.user
*.db
*.db-shm
*.db-wal
*.log
```

Create `cmake/CompilerWarnings.cmake`:

```cmake
function(opcua_target_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()
```

Create root `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(opcua_server_opensource LANGUAGES C CXX)

option(OPCUA_BUILD_TESTS "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_library(opcua_common INTERFACE)
target_include_directories(opcua_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(opcua-daemon apps/opcua-daemon/main.cc)
target_link_libraries(opcua-daemon PRIVATE opcua_common)
opcua_target_warnings(opcua-daemon)

add_executable(opcua-supervisor apps/opcua-supervisor/main.cc)
target_link_libraries(opcua-supervisor PRIVATE opcua_common)
opcua_target_warnings(opcua-supervisor)

if(OPCUA_BUILD_TESTS)
  enable_testing()
endif()
```

Create `src/common/result.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_COMMON_RESULT_H_
#define OPCUA_SERVER_SRC_COMMON_RESULT_H_

#include <string>
#include <utility>

namespace opcua {

class Status {
 public:
  static Status Ok() { return Status(true, ""); }
  static Status Error(std::string message) {
    return Status(false, std::move(message));
  }

  bool ok() const { return ok_; }
  const std::string& message() const { return message_; }

 private:
  Status(bool ok, std::string message)
      : ok_(ok), message_(std::move(message)) {}

  bool ok_;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(T value) : ok_(true), value_(std::move(value)) {}
  Result(Status status) : ok_(false), status_(std::move(status)) {}

  bool ok() const { return ok_; }
  const T& value() const { return value_; }
  T& value() { return value_; }
  const Status& status() const { return status_; }

 private:
  bool ok_;
  T value_{};
  Status status_ = Status::Ok();
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_COMMON_RESULT_H_
```

Create `apps/opcua-daemon/main.cc`:

```cpp
#include <iostream>

int main() {
  std::cout << "opcua-daemon scaffold\n";
  return 0;
}
```

Create `apps/opcua-supervisor/main.cc`:

```cpp
#include <iostream>

int main() {
  std::cout << "opcua-supervisor scaffold\n";
  return 0;
}
```

- [ ] **Step 2: Configure and build**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
```

Expected: both `opcua-daemon` and `opcua-supervisor` build successfully.

- [ ] **Step 3: Commit**

```powershell
git add .gitignore CMakeLists.txt cmake src/common apps
git commit -m "build: add initial CMake scaffold"
```

---

### Task 2: Vendor Dependencies As Build Targets

**Files:**
- Modify: `CMakeLists.txt`
- Create: `third_party/cpp-httplib/README.md`
- Create: `third_party/open62541/README.md`
- Add external files: `third_party/cpp-httplib/httplib.h`
- Add external files: `third_party/open62541/open62541.c`
- Add external files: `third_party/open62541/open62541.h`

- [ ] **Step 1: Fetch vendored source files**

Download `cpp-httplib` single header from its upstream release and place it at:

```text
third_party/cpp-httplib/httplib.h
```

Download open62541 amalgamated source and header from a pinned upstream release and place them at:

```text
third_party/open62541/open62541.c
third_party/open62541/open62541.h
```

Create `third_party/cpp-httplib/README.md`:

```markdown
# cpp-httplib

Vendored single-header copy of cpp-httplib.

This project uses cpp-httplib only in `opcua-supervisor` for the local Web API. Do not include it from daemon hot-path code.
```

Create `third_party/open62541/README.md`:

```markdown
# open62541

Vendored amalgamated open62541 source.

The build exposes this as the `open62541_static` CMake target so compile definitions and include paths stay isolated.
```

- [ ] **Step 2: Wire dependencies into CMake**

Add this block to `CMakeLists.txt` before executable definitions:

```cmake
add_library(open62541_static STATIC third_party/open62541/open62541.c)
target_include_directories(open62541_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/open62541")

add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
```

Update executable link lines:

```cmake
target_link_libraries(opcua-daemon PRIVATE opcua_common open62541_static)
target_link_libraries(opcua-supervisor PRIVATE opcua_common cpp_httplib)
```

- [ ] **Step 3: Build to verify vendored targets**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
```

Expected: build succeeds and compiles `open62541.c` as a static library.

- [ ] **Step 4: Commit**

```powershell
git add CMakeLists.txt third_party
git commit -m "build: vendor open62541 and cpp-httplib"
```

---

### Task 3: Configuration Model And Validation

**Files:**
- Create: `src/config/server_config.h`
- Create: `src/config/server_config.cc`
- Create: `tests/config_validation_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write config validation tests**

Create `tests/config_validation_test.cc`:

```cpp
#include "config/server_config.h"

#include <iostream>

namespace {

int Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << "\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  const auto defaults = opcua::ServerConfig::Default();
  if (int rc = Expect(defaults.Validate().ok(), "default config should validate")) return rc;
  if (int rc = Expect(defaults.server_port == 4840, "default OPC UA port should be 4840")) return rc;
  if (int rc = Expect(defaults.server_bind_address == "0.0.0.0", "default bind address should be 0.0.0.0")) return rc;
  if (int rc = Expect(defaults.logging_target == "stdout", "default logging target should be stdout")) return rc;

  auto invalid_port = defaults;
  invalid_port.server_port = 0;
  if (int rc = Expect(!invalid_port.Validate().ok(), "port 0 should fail validation")) return rc;

  auto invalid_address_space = defaults;
  invalid_address_space.address_space_mode = "nodeset";
  if (int rc = Expect(!invalid_address_space.Validate().ok(), "nodeset mode should not be enabled in v1")) return rc;

  auto invalid_logging = defaults;
  invalid_logging.logging_target = "network";
  if (int rc = Expect(!invalid_logging.Validate().ok(), "invalid logging target should fail")) return rc;

  return 0;
}
```

- [ ] **Step 2: Add the test target**

Add to `CMakeLists.txt` inside `if(OPCUA_BUILD_TESTS)`:

```cmake
add_executable(config_validation_test
  tests/config_validation_test.cc
  src/config/server_config.cc
)
target_link_libraries(config_validation_test PRIVATE opcua_common)
opcua_target_warnings(config_validation_test)
add_test(NAME config_validation_test COMMAND config_validation_test)
```

- [ ] **Step 3: Run the test and verify it fails before implementation**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R config_validation_test
```

Expected: compile fails because `config/server_config.h` does not exist.

- [ ] **Step 4: Implement `ServerConfig`**

Create `src/config/server_config.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_
#define OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_

#include <cstdint>
#include <string>

#include "common/result.h"

namespace opcua {

struct ServerConfig {
  std::string server_application_name;
  std::string server_product_uri;
  std::string server_bind_address;
  uint16_t server_port;
  std::string server_endpoint_path;
  std::string security_mode;
  std::string security_policy;
  int max_sessions;
  int max_subscriptions;
  std::string logging_level;
  std::string logging_target;
  std::string address_space_mode;
  std::string address_space_path;

  static ServerConfig Default();
  Status Validate() const;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_SERVER_CONFIG_H_
```

Create `src/config/server_config.cc`:

```cpp
#include "config/server_config.h"

#include <algorithm>

namespace opcua {

ServerConfig ServerConfig::Default() {
  return ServerConfig{
      "Open62541 C++ Server",
      "urn:rakctite:opcua-server-opensource",
      "0.0.0.0",
      4840,
      "/",
      "none",
      "none",
      100,
      100,
      "info",
      "stdout",
      "builtin",
      "",
  };
}

Status ServerConfig::Validate() const {
  if (server_application_name.empty()) {
    return Status::Error("server.application_name must not be empty");
  }
  if (server_product_uri.empty()) {
    return Status::Error("server.product_uri must not be empty");
  }
  if (server_bind_address.empty()) {
    return Status::Error("server.bind_address must not be empty");
  }
  if (server_port == 0) {
    return Status::Error("server.port must be between 1 and 65535");
  }
  if (server_endpoint_path.empty() || server_endpoint_path[0] != '/') {
    return Status::Error("server.endpoint_path must start with /");
  }
  if (security_mode != "none") {
    return Status::Error("only security.mode=none is supported in v1");
  }
  if (security_policy != "none") {
    return Status::Error("only security.policy=none is supported in v1");
  }
  if (max_sessions <= 0) {
    return Status::Error("limits.max_sessions must be positive");
  }
  if (max_subscriptions <= 0) {
    return Status::Error("limits.max_subscriptions must be positive");
  }
  if (logging_level != "trace" && logging_level != "debug" &&
      logging_level != "info" && logging_level != "warn" &&
      logging_level != "error") {
    return Status::Error("logging.level is invalid");
  }
  if (logging_target != "stdout" && logging_target.rfind("file:", 0) != 0) {
    return Status::Error("logging.target must be stdout or file:<path>");
  }
  if (address_space_mode != "builtin") {
    return Status::Error("only address_space.mode=builtin is supported in v1");
  }
  return Status::Ok();
}

}  // namespace opcua
```

- [ ] **Step 5: Run validation test**

Run:

```powershell
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R config_validation_test
```

Expected: `config_validation_test` passes.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/config tests/config_validation_test.cc
git commit -m "feat: add server config validation"
```

---

### Task 4: SQLite Repository With WAL, Busy Timeout, And Transactions

**Files:**
- Create: `src/config/sqlite_db.h`
- Create: `src/config/sqlite_db.cc`
- Create: `src/config/config_repository.h`
- Create: `src/config/config_repository.cc`
- Create: `tests/config_repository_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write config repository test**

Create `tests/config_repository_test.cc`:

```cpp
#include "config/config_repository.h"

#include <cstdio>
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

}  // namespace

int main() {
  const std::string db_path = "config_repository_test.db";
  std::remove(db_path.c_str());

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (int rc = Expect(repo_result.ok(), repo_result.status().message().c_str())) return rc;

  auto& repo = repo_result.value();
  auto init_status = repo.Initialize();
  if (int rc = Expect(init_status.ok(), init_status.message().c_str())) return rc;

  auto config_result = repo.Load();
  if (int rc = Expect(config_result.ok(), config_result.status().message().c_str())) return rc;
  if (int rc = Expect(config_result.value().server_port == 4840, "default port mismatch")) return rc;

  auto updated = config_result.value();
  updated.server_port = 4850;
  updated.server_bind_address = "127.0.0.1";
  auto save_status = repo.Save(updated);
  if (int rc = Expect(save_status.ok(), save_status.message().c_str())) return rc;

  auto loaded = repo.Load();
  if (int rc = Expect(loaded.ok(), loaded.status().message().c_str())) return rc;
  if (int rc = Expect(loaded.value().server_port == 4850, "updated port mismatch")) return rc;
  if (int rc = Expect(loaded.value().server_bind_address == "127.0.0.1", "updated bind address mismatch")) return rc;

  std::remove(db_path.c_str());
  return 0;
}
```

- [ ] **Step 2: Add SQLite discovery and test target**

Add to root `CMakeLists.txt` before project targets that need SQLite:

```cmake
find_package(SQLite3 REQUIRED)

add_library(opcua_config
  src/config/server_config.cc
  src/config/sqlite_db.cc
  src/config/config_repository.cc
)
target_link_libraries(opcua_config PUBLIC opcua_common SQLite::SQLite3)
opcua_target_warnings(opcua_config)
```

Update `config_validation_test` to link `opcua_config` and remove direct `src/config/server_config.cc` from that test source list:

```cmake
add_executable(config_validation_test tests/config_validation_test.cc)
target_link_libraries(config_validation_test PRIVATE opcua_config)
```

Add repository test:

```cmake
add_executable(config_repository_test tests/config_repository_test.cc)
target_link_libraries(config_repository_test PRIVATE opcua_config)
opcua_target_warnings(config_repository_test)
add_test(NAME config_repository_test COMMAND config_repository_test)
```

- [ ] **Step 3: Run test and verify it fails before implementation**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R config_repository_test
```

Expected: compile fails because SQLite repository files do not exist.

- [ ] **Step 4: Implement SQLite RAII wrapper**

Create `src/config/sqlite_db.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_
#define OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_

#include <sqlite3.h>

#include <string>

#include "common/result.h"

namespace opcua {

class SqliteDb {
 public:
  SqliteDb() = default;
  ~SqliteDb();
  SqliteDb(const SqliteDb&) = delete;
  SqliteDb& operator=(const SqliteDb&) = delete;
  SqliteDb(SqliteDb&& other) noexcept;
  SqliteDb& operator=(SqliteDb&& other) noexcept;

  static Result<SqliteDb> Open(const std::string& path);

  sqlite3* get() const { return db_; }
  Status Execute(const std::string& sql);

 private:
  explicit SqliteDb(sqlite3* db) : db_(db) {}
  sqlite3* db_ = nullptr;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_SQLITE_DB_H_
```

Create `src/config/sqlite_db.cc`:

```cpp
#include "config/sqlite_db.h"

#include <utility>

namespace opcua {

SqliteDb::~SqliteDb() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

SqliteDb::SqliteDb(SqliteDb&& other) noexcept : db_(other.db_) {
  other.db_ = nullptr;
}

SqliteDb& SqliteDb::operator=(SqliteDb&& other) noexcept {
  if (this != &other) {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

Result<SqliteDb> SqliteDb::Open(const std::string& path) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open(path.c_str(), &db);
  if (rc != SQLITE_OK) {
    std::string message = db != nullptr ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return Status::Error(message);
  }

  SqliteDb wrapper(db);
  sqlite3_busy_timeout(wrapper.get(), 5000);
  auto wal_status = wrapper.Execute("PRAGMA journal_mode=WAL;");
  if (!wal_status.ok()) {
    return wal_status;
  }
  auto foreign_keys_status = wrapper.Execute("PRAGMA foreign_keys=ON;");
  if (!foreign_keys_status.ok()) {
    return foreign_keys_status;
  }
  return wrapper;
}

Status SqliteDb::Execute(const std::string& sql) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::string message = error != nullptr ? error : "sqlite3_exec failed";
    sqlite3_free(error);
    return Status::Error(message);
  }
  return Status::Ok();
}

}  // namespace opcua
```

- [ ] **Step 5: Implement config repository**

Create `src/config/config_repository.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_
#define OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_

#include <string>

#include "common/result.h"
#include "config/server_config.h"
#include "config/sqlite_db.h"

namespace opcua {

class ConfigRepository {
 public:
  static Result<ConfigRepository> Open(const std::string& db_path);

  Status Initialize();
  Result<ServerConfig> Load();
  Status Save(const ServerConfig& config);

 private:
  explicit ConfigRepository(SqliteDb db) : db_(std::move(db)) {}

  SqliteDb db_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_CONFIG_CONFIG_REPOSITORY_H_
```

Create `src/config/config_repository.cc` with helper functions using prepared statements for each key. Use a single `config(key TEXT PRIMARY KEY, value TEXT NOT NULL)` table and these defaults:

```cpp
const std::pair<const char*, const char*> kDefaults[] = {
    {"server.application_name", "Open62541 C++ Server"},
    {"server.product_uri", "urn:rakctite:opcua-server-opensource"},
    {"server.bind_address", "0.0.0.0"},
    {"server.port", "4840"},
    {"server.endpoint_path", "/"},
    {"security.mode", "none"},
    {"security.policy", "none"},
    {"limits.max_sessions", "100"},
    {"limits.max_subscriptions", "100"},
    {"logging.level", "info"},
    {"logging.target", "stdout"},
    {"address_space.mode", "builtin"},
    {"address_space.path", ""},
};
```

`Initialize()` must execute:

```sql
CREATE TABLE IF NOT EXISTS config (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
```

Then insert defaults using:

```sql
INSERT OR IGNORE INTO config(key, value) VALUES(?, ?);
```

`Save()` must call `config.Validate()` before writing and then use:

```sql
BEGIN IMMEDIATE;
INSERT INTO config(key, value) VALUES(?, ?)
ON CONFLICT(key) DO UPDATE SET value=excluded.value;
COMMIT;
```

If any write fails, execute `ROLLBACK;` and return `Status::Error(...)`.

- [ ] **Step 6: Run repository tests**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R "config_.*test"
```

Expected: config validation and repository tests pass.

- [ ] **Step 7: Commit**

```powershell
git add CMakeLists.txt src/config tests/config_repository_test.cc
git commit -m "feat: add SQLite config repository"
```

---

### Task 5: Daemon Startup Smoke Path With open62541

**Files:**
- Create: `src/daemon/opcua_server.h`
- Create: `src/daemon/opcua_server.cc`
- Modify: `apps/opcua-daemon/main.cc`
- Create: `tests/daemon_smoke_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write daemon smoke test**

Create `tests/daemon_smoke_test.cc`:

```cpp
#include "config/config_repository.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main() {
  const std::string db_path = "daemon_smoke_test.db";
  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (!repo_result.ok()) {
    std::cerr << repo_result.status().message() << "\n";
    return 1;
  }
  auto init_status = repo_result.value().Initialize();
  if (!init_status.ok()) {
    std::cerr << init_status.message() << "\n";
    return 1;
  }
  return 0;
}
```

Add test target:

```cmake
add_executable(daemon_smoke_test tests/daemon_smoke_test.cc)
target_link_libraries(daemon_smoke_test PRIVATE opcua_config)
opcua_target_warnings(daemon_smoke_test)
add_test(NAME daemon_smoke_test COMMAND daemon_smoke_test)
```

- [ ] **Step 2: Implement open62541 wrapper interface**

Create `src/daemon/opcua_server.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_
#define OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_

#include <atomic>

#include "common/result.h"
#include "config/server_config.h"

namespace opcua {

class OpcuaServer {
 public:
  explicit OpcuaServer(ServerConfig config);
  Status Run(std::atomic_bool* running);

 private:
  ServerConfig config_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_DAEMON_OPCUA_SERVER_H_
```

Create `src/daemon/opcua_server.cc`:

```cpp
#include "daemon/opcua_server.h"

extern "C" {
#include "open62541.h"
}

namespace opcua {

OpcuaServer::OpcuaServer(ServerConfig config) : config_(std::move(config)) {}

Status OpcuaServer::Run(std::atomic_bool* running) {
  UA_Server* server = UA_Server_new();
  if (server == nullptr) {
    return Status::Error("UA_Server_new failed");
  }

  UA_ServerConfig* ua_config = UA_Server_getConfig(server);
  const UA_StatusCode config_status =
      UA_ServerConfig_setDefault(ua_config);
  if (config_status != UA_STATUSCODE_GOOD) {
    UA_Server_delete(server);
    return Status::Error("UA_ServerConfig_setDefault failed");
  }

  UA_Boolean ua_running = true;
  while (running->load()) {
    const UA_StatusCode rc = UA_Server_run_iterate(server, true);
    if (rc != UA_STATUSCODE_GOOD) {
      UA_Server_delete(server);
      return Status::Error("UA_Server_run_iterate failed");
    }
    ua_running = running->load() ? true : false;
  }
  (void)ua_running;
  UA_Server_delete(server);
  return Status::Ok();
}

}  // namespace opcua
```

- [ ] **Step 3: Wire daemon executable**

Update CMake:

```cmake
add_library(opcua_daemon_lib
  src/daemon/opcua_server.cc
)
target_link_libraries(opcua_daemon_lib PUBLIC opcua_common opcua_config open62541_static)
opcua_target_warnings(opcua_daemon_lib)

target_link_libraries(opcua-daemon PRIVATE opcua_daemon_lib)
```

Replace `apps/opcua-daemon/main.cc`:

```cpp
#include "config/config_repository.h"
#include "daemon/opcua_server.h"

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

namespace {

std::atomic_bool g_running{true};

void HandleSignal(int) {
  g_running.store(false);
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::string db_path = argc > 1 ? argv[1] : "opcua-server.db";
  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (!repo_result.ok()) {
    std::cerr << repo_result.status().message() << "\n";
    return 1;
  }

  auto init_status = repo_result.value().Initialize();
  if (!init_status.ok()) {
    std::cerr << init_status.message() << "\n";
    return 1;
  }

  auto config_result = repo_result.value().Load();
  if (!config_result.ok()) {
    std::cerr << config_result.status().message() << "\n";
    return 1;
  }

  opcua::OpcuaServer server(config_result.value());
  auto run_status = server.Run(&g_running);
  if (!run_status.ok()) {
    std::cerr << run_status.message() << "\n";
    return 1;
  }
  return 0;
}
```

- [ ] **Step 4: Build and run tests**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R "daemon_smoke_test|config_.*test"
```

Expected: tests pass and daemon executable links against open62541.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/daemon apps/opcua-daemon tests/daemon_smoke_test.cc
git commit -m "feat: add OPC UA daemon startup path"
```

---

### Task 6: Supervisor Process Controller

**Files:**
- Create: `src/supervisor/process_controller.h`
- Create: `src/supervisor/process_controller.cc`
- Create: `tests/test_child/main.cc`
- Create: `tests/process_controller_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write process controller test**

Create `tests/test_child/main.cc`:

```cpp
#include <chrono>
#include <csignal>
#include <thread>

namespace {
volatile std::sig_atomic_t running = 1;
void Stop(int) { running = 0; }
}  // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  while (running != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 0;
}
```

Create `tests/process_controller_test.cc`:

```cpp
#include "supervisor/process_controller.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "missing child path\n";
    return 1;
  }

  opcua::ProcessController controller(argv[1], {});
  auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << start.message() << "\n";
    return 1;
  }
  if (controller.status().state != opcua::ProcessState::kRunning) {
    std::cerr << "expected running\n";
    return 1;
  }

  auto stop = controller.Stop(std::chrono::milliseconds(2000));
  if (!stop.ok()) {
    std::cerr << stop.message() << "\n";
    return 1;
  }
  if (controller.status().state != opcua::ProcessState::kStopped) {
    std::cerr << "expected stopped\n";
    return 1;
  }

  return 0;
}
```

- [ ] **Step 2: Add CMake targets**

Add:

```cmake
add_library(opcua_supervisor_lib
  src/supervisor/process_controller.cc
)
target_link_libraries(opcua_supervisor_lib PUBLIC opcua_common opcua_config)
opcua_target_warnings(opcua_supervisor_lib)

add_executable(test_child tests/test_child/main.cc)
opcua_target_warnings(test_child)

add_executable(process_controller_test tests/process_controller_test.cc)
target_link_libraries(process_controller_test PRIVATE opcua_supervisor_lib)
opcua_target_warnings(process_controller_test)
add_test(NAME process_controller_test
  COMMAND process_controller_test $<TARGET_FILE:test_child>
)
```

- [ ] **Step 3: Implement portable controller interface**

Create `src/supervisor/process_controller.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_

#include <chrono>
#include <string>
#include <vector>

#include "common/result.h"

namespace opcua {

enum class ProcessState {
  kStopped,
  kRunning,
  kCrashed,
};

struct ProcessStatus {
  ProcessState state = ProcessState::kStopped;
  int exit_code = 0;
  std::string diagnostic;
};

class ProcessController {
 public:
  ProcessController(std::string executable_path, std::vector<std::string> args);
  ~ProcessController();

  Status Start();
  Status Stop(std::chrono::milliseconds timeout);
  Status Restart(std::chrono::milliseconds timeout);
  void ReapExited();
  ProcessStatus status();

 private:
  std::string executable_path_;
  std::vector<std::string> args_;
  ProcessStatus status_;

#if defined(_WIN32)
  void* process_handle_ = nullptr;
#else
  int child_pid_ = -1;
#endif
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_PROCESS_CONTROLLER_H_
```

- [ ] **Step 4: Implement OS-specific process control**

Create `src/supervisor/process_controller.cc`.

For POSIX, use `fork`, `execv`, `kill`, and `waitpid(WNOHANG)`. For Windows, use `CreateProcessA`, `TerminateProcess`, and `GetExitCodeProcess` for the first version. The POSIX `Stop()` path must send `SIGTERM`, wait until timeout, then send `SIGKILL`.

Use these state transitions:

```text
Start success -> kRunning
Stop success -> kStopped
Child exits nonzero before Stop -> kCrashed
Start while running -> Status::Error("process already running")
Stop while stopped -> Status::Ok()
```

- [ ] **Step 5: Run process tests**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R process_controller_test
```

Expected: process controller test passes on the current platform.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/supervisor tests/test_child tests/process_controller_test.cc
git commit -m "feat: add supervisor process controller"
```

---

### Task 7: Supervisor Web API

**Files:**
- Create: `src/supervisor/api_server.h`
- Create: `src/supervisor/api_server.cc`
- Modify: `apps/opcua-supervisor/main.cc`
- Create: `tests/api_server_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write API server test**

Create `tests/api_server_test.cc`:

```cpp
#include "config/config_repository.h"

#include <iostream>

int main() {
  auto repo_result = opcua::ConfigRepository::Open("api_server_test.db");
  if (!repo_result.ok()) {
    std::cerr << repo_result.status().message() << "\n";
    return 1;
  }
  auto init = repo_result.value().Initialize();
  if (!init.ok()) {
    std::cerr << init.message() << "\n";
    return 1;
  }
  auto config = repo_result.value().Load();
  if (!config.ok()) {
    std::cerr << config.status().message() << "\n";
    return 1;
  }
  if (config.value().server_port != 4840) {
    std::cerr << "default config unavailable\n";
    return 1;
  }
  return 0;
}
```

- [ ] **Step 2: Define API server interface**

Create `src/supervisor/api_server.h`:

```cpp
#ifndef OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_
#define OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_

#include <string>

#include "common/result.h"
#include "config/config_repository.h"
#include "supervisor/process_controller.h"

namespace opcua {

class ApiServer {
 public:
  ApiServer(ConfigRepository* repository, ProcessController* controller);
  Status Run(const std::string& bind_address, int port);
  void Stop();

 private:
  ConfigRepository* repository_;
  ProcessController* controller_;
};

}  // namespace opcua

#endif  // OPCUA_SERVER_SRC_SUPERVISOR_API_SERVER_H_
```

- [ ] **Step 3: Implement routes**

Create `src/supervisor/api_server.cc` using `httplib.h`. Implement these routes:

```text
GET /health -> {"status":"ok"}
GET /api/v1/status -> daemon state, exit_code, diagnostic
GET /api/v1/config -> current config JSON
PUT /api/v1/config -> parse simple JSON fields, validate, save
POST /api/v1/daemon/start -> controller.Start()
POST /api/v1/daemon/stop -> controller.Stop(5000ms)
POST /api/v1/daemon/restart -> controller.Restart(5000ms)
```

The first parser may be strict and small: accept only a complete JSON object generated by this project. Return HTTP 400 on parse or validation errors, HTTP 500 on repository or lifecycle errors, and HTTP 200 on success.

- [ ] **Step 4: Wire supervisor main**

Replace `apps/opcua-supervisor/main.cc`:

```cpp
#include "config/config_repository.h"
#include "supervisor/api_server.h"
#include "supervisor/process_controller.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  const std::string db_path = argc > 1 ? argv[1] : "opcua-server.db";
  const std::string daemon_path = argc > 2 ? argv[2] : "opcua-daemon";

  auto repo_result = opcua::ConfigRepository::Open(db_path);
  if (!repo_result.ok()) {
    std::cerr << repo_result.status().message() << "\n";
    return 1;
  }
  auto init = repo_result.value().Initialize();
  if (!init.ok()) {
    std::cerr << init.message() << "\n";
    return 1;
  }

  opcua::ProcessController controller(daemon_path, {db_path});
  auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << start.message() << "\n";
  }

  opcua::ApiServer api(&repo_result.value(), &controller);
  auto status = api.Run("0.0.0.0", 8080);
  if (!status.ok()) {
    std::cerr << status.message() << "\n";
    return 1;
  }
  return 0;
}
```

- [ ] **Step 5: Update CMake**

Update supervisor library:

```cmake
add_library(opcua_supervisor_lib
  src/supervisor/process_controller.cc
  src/supervisor/api_server.cc
)
target_link_libraries(opcua_supervisor_lib PUBLIC opcua_common opcua_config cpp_httplib)
```

Add API test:

```cmake
add_executable(api_server_test tests/api_server_test.cc)
target_link_libraries(api_server_test PRIVATE opcua_supervisor_lib)
opcua_target_warnings(api_server_test)
add_test(NAME api_server_test COMMAND api_server_test)
```

- [ ] **Step 6: Build and run API-related tests**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R "api_server_test|config_.*test|process_controller_test"
```

Expected: tests pass and supervisor executable links with cpp-httplib.

- [ ] **Step 7: Commit**

```powershell
git add CMakeLists.txt src/supervisor apps/opcua-supervisor tests/api_server_test.cc
git commit -m "feat: add supervisor Web API"
```

---

### Task 8: PID 1 Signal Handling And Crash Status

**Files:**
- Modify: `src/supervisor/process_controller.h`
- Modify: `src/supervisor/process_controller.cc`
- Modify: `apps/opcua-supervisor/main.cc`
- Create: `tests/crashing_child/main.cc`
- Create: `tests/process_crash_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write crash status test**

Create `tests/crashing_child/main.cc`:

```cpp
int main() {
  return 42;
}
```

Create `tests/process_crash_test.cc`:

```cpp
#include "supervisor/process_controller.h"

#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "missing crashing child path\n";
    return 1;
  }
  opcua::ProcessController controller(argv[1], {});
  auto start = controller.Start();
  if (!start.ok()) {
    std::cerr << start.message() << "\n";
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  controller.ReapExited();
  const auto status = controller.status();
  if (status.state != opcua::ProcessState::kCrashed) {
    std::cerr << "expected crashed state\n";
    return 1;
  }
  if (status.exit_code != 42) {
    std::cerr << "expected exit code 42\n";
    return 1;
  }
  return 0;
}
```

- [ ] **Step 2: Add test targets**

Add:

```cmake
add_executable(crashing_child tests/crashing_child/main.cc)
opcua_target_warnings(crashing_child)

add_executable(process_crash_test tests/process_crash_test.cc)
target_link_libraries(process_crash_test PRIVATE opcua_supervisor_lib)
opcua_target_warnings(process_crash_test)
add_test(NAME process_crash_test
  COMMAND process_crash_test $<TARGET_FILE:crashing_child>
)
```

- [ ] **Step 3: Enhance process controller status**

Update `ReapExited()` so unexpected child exit sets:

```cpp
status_.state = ProcessState::kCrashed;
status_.exit_code = decoded_exit_code;
status_.diagnostic = "process exited unexpectedly";
```

Update `Stop()` so expected shutdown sets:

```cpp
status_.state = ProcessState::kStopped;
status_.diagnostic = "process stopped";
```

- [ ] **Step 4: Add supervisor shutdown handling**

In `apps/opcua-supervisor/main.cc`, add a global `std::atomic_bool g_shutdown_requested{false};`, signal handlers for `SIGINT` and `SIGTERM`, and a loop that periodically calls `controller.ReapExited()`. On shutdown, call:

```cpp
controller.Stop(std::chrono::milliseconds(5000));
```

Then stop the API server and exit.

- [ ] **Step 5: Run process tests**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure -R "process_.*test"
```

Expected: start/stop and crash tests pass.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt src/supervisor apps/opcua-supervisor tests/crashing_child tests/process_crash_test.cc
git commit -m "feat: handle supervisor signals and crashed daemon state"
```

---

### Task 9: Docker Packaging

**Files:**
- Create: `docker/Dockerfile`
- Create: `docker/docker-compose.yml`
- Modify: `README.md`

- [ ] **Step 1: Create Dockerfile**

Create `docker/Dockerfile`:

```dockerfile
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    libsqlite3-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DOPCUA_BUILD_TESTS=OFF \
  && cmake --build build --config Release --parallel

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
  && rm -rf /var/lib/apt/lists/* \
  && useradd --system --home /var/lib/opcua-server --create-home opcua

COPY --from=build /src/build/opcua-supervisor /usr/local/bin/opcua-supervisor
COPY --from=build /src/build/opcua-daemon /usr/local/bin/opcua-daemon

USER opcua
WORKDIR /var/lib/opcua-server
EXPOSE 4840 8080

ENTRYPOINT ["/usr/local/bin/opcua-supervisor", "/var/lib/opcua-server/opcua-server.db", "/usr/local/bin/opcua-daemon"]
```

- [ ] **Step 2: Create compose file**

Create `docker/docker-compose.yml`:

```yaml
services:
  opcua-server:
    build:
      context: ..
      dockerfile: docker/Dockerfile
    ports:
      - "4840:4840"
      - "8080:8080"
    volumes:
      - opcua-data:/var/lib/opcua-server

volumes:
  opcua-data:
```

- [ ] **Step 3: Add README run commands**

Create or update `README.md` with:

```markdown
# opcua_server_opensource

## Build

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Docker

```powershell
docker compose -f docker/docker-compose.yml up --build
```

API:

```text
GET  http://localhost:8080/health
GET  http://localhost:8080/api/v1/status
GET  http://localhost:8080/api/v1/config
POST http://localhost:8080/api/v1/daemon/restart
```
```

- [ ] **Step 4: Build Docker image**

Run:

```powershell
docker compose -f docker/docker-compose.yml build
```

Expected: image builds and contains both executables.

- [ ] **Step 5: Commit**

```powershell
git add docker README.md
git commit -m "build: add Docker packaging"
```

---

### Task 10: Final Verification And Remote Push

**Files:**
- Modify only if verification exposes a concrete defect.

- [ ] **Step 1: Run native verification**

Run:

```powershell
cmake -S . -B build -DOPCUA_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Run Docker smoke verification**

Run:

```powershell
docker compose -f docker/docker-compose.yml up --build
```

In another shell, run:

```powershell
Invoke-WebRequest http://localhost:8080/health
Invoke-WebRequest http://localhost:8080/api/v1/status
Invoke-WebRequest http://localhost:8080/api/v1/config
```

Expected: `/health` returns status ok, `/status` reports daemon state, `/config` returns current config.

- [ ] **Step 3: Inspect Git history**

Run:

```powershell
git status --short
git log --oneline --decorate -10
```

Expected: working tree is clean and implementation commits are present on `main`.

- [ ] **Step 4: Push to GitHub**

Run:

```powershell
git push -u origin main
```

Expected: branch pushes to `https://github.com/Rakctite/opcua_server_opensource`.

---

## Self-Review

Spec coverage:

- Single container, two-process design: Tasks 6, 7, 8, 9.
- PID 1 reaping and signal forwarding: Task 8.
- SQLite config, WAL, busy timeout, transactions: Task 4.
- Initial config fields including `bind_address` and `logging.target`: Tasks 3 and 4.
- Vendored open62541 static CMake target: Task 2.
- cpp-httplib API server: Task 7.
- Daemon config snapshot at startup: Task 5.
- Crash status reporting: Task 8.
- Docker packaging: Task 9.
- Verification and push: Task 10.

Placeholder scan:

- No intentionally unresolved placeholder markers.
- Task 4 intentionally leaves prepared-statement helper layout to the implementer but specifies schema, SQL, defaults, transaction behavior, and test expectations.
- Task 6 intentionally leaves OS-specific implementation details to the implementer but specifies APIs, platform primitives, state transitions, and verification tests.

Type consistency:

- `ServerConfig`, `Status`, `Result<T>`, `ConfigRepository`, `ProcessController`, `ProcessStatus`, `ProcessState`, `OpcuaServer`, and `ApiServer` names are consistent across tasks.
- Config key names match the approved design spec.
