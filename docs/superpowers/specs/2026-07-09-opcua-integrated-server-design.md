# OPC UA Integrated Server Design

## Goal

Build a CMake-based C/C++ OPC UA server project using vendored open62541, with a supervisor process that exposes a Web API for configuration and daemon lifecycle control. The first version targets a single Docker container running two processes: `opcua-supervisor` as PID 1 and `opcua-daemon` as its child process.

The design prioritizes OPC UA runtime performance, predictable configuration application, and portability across GCC, Clang, and MSVC.

## Architecture

The runtime uses one container with two executables.

```text
opcua-server container
  opcua-supervisor
    - PID 1
    - Provides the Web API through cpp-httplib
    - Reads and writes SQLite configuration
    - Starts, stops, restarts, and monitors opcua-daemon
    - Reaps child processes and forwards shutdown signals
    - Exposes daemon status through API endpoints

  opcua-daemon
    - Runs the open62541 OPC UA server
    - Reads a configuration snapshot from SQLite at startup
    - Does not watch configuration changes while running
    - Focuses on OPC UA server performance
```

`opcua-supervisor` and `opcua-daemon` are separate processes but are shipped in the same image. This keeps local Docker deployment simple and avoids giving a Web API container access to the Docker socket or an external orchestrator API.

Because `opcua-supervisor` runs as PID 1 in the container, it must implement process supervision responsibilities directly. It must reap terminated child processes with `wait` or `waitpid` so daemon exits do not leave zombie processes. On container shutdown, the supervisor must handle `SIGTERM`, forward a shutdown signal to `opcua-daemon`, wait for graceful termination, and then force termination only after a configured timeout.

## Configuration Model

The Web API only persists configuration changes to SQLite. It does not force the running daemon to reconfigure itself in place. New settings are applied by restarting the daemon through a lifecycle API endpoint.

```text
API client
  -> PUT /api/v1/config
  -> supervisor writes SQLite
  -> POST /api/v1/daemon/restart
  -> supervisor restarts opcua-daemon
  -> daemon reads latest SQLite snapshot at startup
  -> daemon configures open62541 server
```

This keeps OPC UA endpoint, security, limits, and address-space setup deterministic. It also avoids runtime mutation of open62541 server state for the first implementation.

SQLite must be configured for predictable concurrent access between supervisor writes and daemon startup reads. The configuration database should enable WAL mode and set a `sqlite3_busy_timeout()` value on each connection. Configuration writes should happen in transactions so the daemon either reads the previous complete configuration or the new complete configuration, never a partial update.

## Initial API

The initial Web API surface is intentionally small.

```text
GET  /health
GET  /api/v1/status
GET  /api/v1/config
PUT  /api/v1/config
POST /api/v1/daemon/start
POST /api/v1/daemon/stop
POST /api/v1/daemon/restart
```

The API is owned by `opcua-supervisor`. It should return structured JSON responses and clear error codes for invalid configuration, database errors, and daemon lifecycle failures.

## Initial Configuration Scope

The first version stores only server-level configuration in SQLite.

```text
server.application_name
server.product_uri
server.bind_address
server.port
server.endpoint_path
security.mode
security.policy
limits.max_sessions
limits.max_subscriptions
logging.level
logging.target
address_space.mode
address_space.path
```

`address_space.mode = builtin` is the only required address-space implementation in the first version. `address_space.path` is included so a later `nodeset` mode can be added without changing the high-level configuration contract.

`server.bind_address` defaults to `0.0.0.0` for container deployment. `logging.target` defaults to `stdout`; file logging can be supported where the configured path is writable, but stdout remains the container-first default.

Node CRUD, namespace editing, certificate management, and user management are out of scope for the first version.

## Build And Dependencies

The project uses CMake and targets GCC, Clang, and MSVC.

The primary language standard is C++17. This gives broad compiler support while still allowing RAII, `std::optional`, `std::filesystem` where available, and modern type-safe interfaces.

Dependencies:

- open62541: vendored source, built directly as part of the project.
- cpp-httplib: vendored header-only HTTP server for supervisor Web API.
- SQLite: accessed through the SQLite C API and wrapped with project-owned C++ RAII types.

The OPC UA performance path must avoid unnecessary abstraction and runtime allocation. Web API convenience must not leak into daemon hot paths.

open62541 should be exposed to the rest of the project as a CMake `STATIC` library target, even when using the common amalgamated `open62541.c` and `open62541.h` form. This keeps compile options, include paths, and dependencies isolated from application targets.

## Coding Rules

The C++ code should follow C++ Core Guidelines as far as practical:

- Use RAII for ownership and cleanup.
- Keep C API handles behind small wrapper types.
- Wrap open62541 resources with narrow RAII helpers where ownership rules are non-trivial.
- Prefer explicit lifetimes and narrow interfaces.
- Avoid global mutable state except where required by process-level signal handling.
- Keep error paths explicit and testable.

Naming follows Google C++ Style Guide conventions:

- Types: `PascalCase`
- Functions and local variables: `snake_case`
- Data members: `snake_case_`
- Constants: `kConstantName`
- Files: `snake_case.h`, `snake_case.cc`

## Proposed Source Layout

```text
CMakeLists.txt
cmake/
third_party/
  open62541/
  cpp-httplib/
src/
  common/
  config/
  supervisor/
  daemon/
apps/
  opcua-supervisor/
  opcua-daemon/
docker/
tests/
```

`src/config` owns SQLite schema, defaults, validation, and config snapshots. `src/supervisor` owns HTTP routing and daemon process control. `src/daemon` owns open62541 server construction and execution.

## Error Handling

Configuration validation happens before writing new settings to SQLite. Invalid settings must not partially update the active configuration.

Daemon lifecycle operations return explicit status:

- already running
- already stopped
- start failed
- stop timed out
- restart failed
- database unavailable
- crashed

The supervisor should track the latest daemon process state and exit code. The daemon should fail fast on invalid startup configuration and log the reason before exiting.

If a new configuration causes daemon startup to fail, the supervisor must not hide the failure. `GET /api/v1/status` should report a crashed state, the last exit code or signal, and enough diagnostic text to identify the startup failure. The first implementation may keep a bounded in-memory tail of daemon stderr/stdout for status reporting.

## Testing Strategy

Initial tests should cover:

- SQLite schema creation and migration.
- Default configuration insertion.
- WAL and busy timeout initialization.
- Config validation.
- Config read/write round trip.
- Supervisor process controller behavior.
- Child process reaping and shutdown signal forwarding.
- Web API endpoint behavior.
- Daemon startup smoke test using a temporary SQLite database.
- Crashed daemon status reporting.

Docker smoke testing can be added after the native executables work reliably.

## Out Of Scope For The First Version

- Runtime hot reload of open62541 server configuration.
- Web API node CRUD.
- Full OPC UA address-space modeling in SQLite.
- Certificate lifecycle management.
- User authentication and authorization.
- Multi-container orchestration control.
- Kubernetes operator or Docker socket integration.
