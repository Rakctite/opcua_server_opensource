# 2026-07-14 Progress Checkpoint

## Completed

- Built the single-container, two-process architecture around `opcua-supervisor` and `opcua-daemon`.
- Wired SQLite-backed configuration and lifecycle control paths into the supervisor/daemon split.
- Added the open62541-backed daemon runtime and the supervisor API surface.
- Verified the native build matrix on GCC and MSVC.
- Skipped Docker packaging per instruction.

## Current Problems

1. Daemon configuration application is still incomplete.
   - The current runtime only applies the port through `UA_ServerConfig_setMinimal`.
   - Remaining config fields still need explicit mapping: bind address, endpoint path, application name, product URI, session/subscription limits, and logging target/level.

2. Linux process reaping still only covers the direct child path.
   - The supervisor needs a real `waitpid(-1, WNOHANG)` reap loop so adopted descendants are collected too.
   - This matters for PID 1 behavior and for test coverage that matches container runtime behavior.

3. Windows shutdown still uses hard termination.
   - `TerminateProcess` bypasses graceful daemon shutdown.
   - The supervisor needs an explicit graceful shutdown signal/event path before fallback termination.

## Next Steps

- Finish the three review fixes above.
- Re-run native verification across GCC, Clang, and MSVC.
- Re-check the supervisor/daemon shutdown behavior after the process model update.
- Revisit Docker packaging only after the runtime and verification path are stable.

## Notes

- The working branch is `feature/opcua-integrated-server`.
- This checkpoint intentionally preserves the current state for a later resume instead of expanding scope now.

## MQTT MVP Update

- Added the MQTT realtime value-store MVP through Task 10 on branch `feature/realtime-value-store-mqtt-mvp`.
- Added an opt-in Mosquitto end-to-end integration test target for Task 11. The target is disabled by default and requires `OPCUA_MQTT_INTEGRATION_TESTS=ON` plus a valid `MOSQUITTO_EXECUTABLE`.
- The integration harness is intended to run a real Mosquitto broker, configure SQLite for an enabled MQTT double mapping, start `opcua-daemon`, publish through Paho MQTTAsync, read and subscribe through open62541, verify read-only OPC UA writes are rejected, and cover broker loss/reconnect quality transitions.
- The harness depends on an external Mosquitto executable. If Mosquitto is absent, only default builds/tests and the opt-in configure failure path can be verified locally.
