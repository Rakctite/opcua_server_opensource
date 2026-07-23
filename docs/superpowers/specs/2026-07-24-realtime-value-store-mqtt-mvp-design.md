# Realtime Value Store And MQTT MVP Design

## Goal

Add the first live data path to the existing OPC UA daemon:

```text
MQTT scalar message
  -> application-owned realtime value store
  -> open62541 callback-backed Variable Node
  -> OPC UA Read and Subscription clients
```

The OPC UA server keeps only the latest value. History remains the
responsibility of a separate OPC UA client and its time-series database.

This phase validates the server-side storage model and callback integration.
It does not attempt to implement the complete MQTT feature set.

## Scope

The first increment supports:

- One MQTT broker.
- One MQTT topic mapped to one OPC UA Variable Node.
- Scalar `Boolean`, `Int64`, and `Double` values.
- MQTT QoS 1.
- Read-only OPC UA data Nodes.
- Latest value, quality, source timestamp, and sequence tracking.
- Connection loss, stale data, and recovery quality transitions.
- SQLite-backed MQTT configuration changed through the supervisor API.
- Configuration application only after daemon restart.
- Eclipse Paho MQTT C Async as a vendored static dependency.
- An external Eclipse Mosquitto broker for integration testing.

The following are explicitly deferred:

- JSON payloads and JSON Pointer mappings.
- Sparkplug B.
- TLS and MQTT authentication.
- Multiple brokers or multiple mappings in the public API.
- OPC UA client writes to source data Nodes.
- Historian and time-series database integration.
- Runtime configuration hot reload.
- Zero-copy optimization for variable-sized values.

## Runtime Architecture

```text
opcua-supervisor
  -> Web API
  -> SQLite configuration
  -> starts and monitors opcua-daemon

opcua-daemon
  -> loads one configuration snapshot at startup
  -> creates RealtimeValueStore slots
  -> creates callback-backed OPC UA Variable Nodes
  -> starts MqttAdapter
  -> runs open62541

MqttAdapter
  -> receives a scalar payload through Paho callbacks
  -> parses the configured type
  -> updates RealtimeValueStore

open62541
  -> invokes the Variable Node read callback
  -> reads a consistent slot snapshot
  -> returns a UA_DataValue to Read or Subscription processing
```

The MQTT callback never calls `UA_Server_writeValue`. It only updates the
application-owned store. This keeps MQTT networking independent from the
open62541 server lock and event loop.

## Realtime Value Store

`RealtimeValueStore` owns a fixed set of `ValueSlot` objects created before
the OPC UA server starts. Slot addresses remain stable for the lifetime of the
daemon.

Each slot contains:

```text
configured data type
latest scalar value
OPC UA StatusCode
source timestamp
last successful update time
monotonic sequence number
has_value flag
```

The public interface is narrow:

```text
Update(slot index, typed value, source timestamp)
MarkSourceUnavailable(source id, status transition time)
ReadSnapshot(slot index)
```

The initial implementation uses a short per-slot lock to produce a consistent
snapshot. This is preferred over premature lock-free storage because value,
quality, timestamp, and sequence must represent one update. Fixed-size scalar
storage can be benchmarked later and replaced with an atomic or double-buffered
implementation without changing the adapter or OPC UA interfaces.

The sequence number increments whenever the value or quality state changes.
An idempotent update with the same value and quality does not increment it.

The store does not retain prior values. A future historian consumes OPC UA
Subscription notifications and persists them outside this process.

## OPC UA Address Space

The daemon creates one read-only Variable Node from the persisted mapping.
The initial test mapping is:

```text
NodeId: ns=2;i=1001
BrowseName: Temperature
DataType: Double
MQTT topic: test/temperature
```

The Variable Node uses open62541
`UA_Server_addCallbackValueSourceVariableNode`. Its `nodeContext` points to a
stable project-owned context containing the value-store reference and slot
index.

The read callback:

1. Reads one consistent `ValueSlot` snapshot.
2. Converts the fixed scalar type to a `UA_Variant`.
3. Sets value presence, StatusCode, and SourceTimestamp on `UA_DataValue`.
4. Returns without performing network access, allocation-heavy parsing, or
   database access.

The first implementation copies the scalar into the returned `UA_DataValue`.
This is the correctness baseline. Zero-copy reads are considered only after
measurement and only where memory lifetime remains explicit.

No write callback is registered. The Variable Node access level is Read-only,
so an OPC UA client cannot overwrite PLC or MQTT source values.

Subscription sampling uses the same callback-backed value. The default
`STATUS_VALUE` trigger reports a notification when either the value or
StatusCode changes. SourceTimestamp-only changes do not create notifications,
which matches the latest-value requirement.

## MQTT Adapter

Eclipse Paho MQTT C Async is vendored and built as a static CMake target.
The MVP disables TLS and enables the Paho high-performance build option.
Paho owns MQTT network processing and invokes project callbacks for connection
and message events.

The adapter owns:

- Paho client lifecycle through RAII.
- Connect and automatic reconnect behavior.
- Subscription to the configured topic at QoS 1.
- Strict scalar payload parsing.
- Source connection state.
- Stale timeout tracking.

The message callback performs bounded work:

1. Match the configured topic.
2. Validate payload size and scalar syntax.
3. Convert to the configured type.
4. Update the mapped `ValueSlot`.
5. Return ownership of the Paho message resources.

Malformed payloads are rejected and counted for diagnostics. They do not
replace the last valid value. If valid data is not received before the stale
timeout, the normal stale-data quality transition applies.

## Quality State Model

The value store applies these transitions:

```text
Daemon startup before first valid message
  value: Null
  status: Bad_WaitingForInitialData

Valid MQTT message
  value: latest parsed value
  status: Good
  source timestamp: message receipt time for the scalar MVP

Broker connection lost or stale timeout after a valid value
  value: last valid value
  status: Uncertain_NoCommunicationLastUsableValue
  source timestamp: timestamp of the last valid value

Broker unavailable before any valid value
  value: Null
  status: Bad_WaitingForInitialData

First valid message after reconnect
  value: latest parsed value
  status: Good

Source disabled by configuration
  value: Null
  status: Bad_OutOfService
```

A connection-state diagnostic Node is also exposed for the source. It reports
at least connection state, last successful update time, and consecutive
failures. Data Nodes still carry their own quality so generic OPC UA clients do
not need knowledge of the diagnostic Node.

Repeated disconnect callbacks do not repeatedly write the same quality state.
Recovery to `Good` creates a Subscription notification even when the recovered
numeric value equals the previous value, because the StatusCode changed.

## Configuration And API

MQTT configuration follows the existing persistence rule:

```text
PUT /api/v1/mqtt-config
  -> validate complete request
  -> write SQLite transaction
  -> return without changing the running daemon

POST /api/v1/daemon/restart
  -> restart daemon
  -> daemon loads the new snapshot
  -> create store, Node, and MQTT adapter
```

The API surface is:

```text
GET /api/v1/mqtt-config
PUT /api/v1/mqtt-config
```

The complete MVP request contains:

```json
{
  "enabled": true,
  "broker_uri": "tcp://127.0.0.1:1883",
  "client_id": "opcua-server",
  "topic": "test/temperature",
  "qos": 1,
  "node_id": 1001,
  "browse_name": "Temperature",
  "data_type": "double",
  "stale_timeout_ms": 5000
}
```

The public API accepts one source and mapping in this phase. The SQLite schema
uses separate source, node, and mapping tables so later support for multiple
brokers and mappings does not require replacing the persistence model.

The schema is created through an idempotent repository migration and does not
replace the existing server configuration table. When `enabled` is false, the
daemon still creates the configured Node with `Bad_OutOfService` quality but
does not create an MQTT connection. This keeps the address space predictable.

The save operation validates URI, client id, topic, QoS, NodeId, browse name,
data type, and timeout before beginning a transaction. The daemon fails fast
with a clear diagnostic if persisted configuration cannot be loaded or used to
construct the address space.

## Concurrency And Ownership

Ownership boundaries are:

- `OpcuaServer` owns the address space and callback registration.
- `RealtimeValueStore` owns all live `ValueSlot` objects.
- Node contexts refer to stable slots but do not own them.
- `MqttAdapter` owns the Paho client handle.
- The daemon starts the store before Nodes and adapters.
- Shutdown stops MQTT callbacks before destroying Node contexts or slots.

The required shutdown order is:

```text
stop accepting MQTT callbacks
-> disconnect and destroy Paho client
-> stop open62541
-> destroy Node contexts
-> destroy RealtimeValueStore
```

This order prevents callbacks from accessing destroyed storage.

## Error Handling

Configuration and startup errors are fail-fast because a partially constructed
address space is not useful.

Runtime MQTT failures do not terminate the daemon. They update source
diagnostics and Node quality while Paho reconnect logic continues.

Payload parse failures:

- Leave the previous value unchanged.
- Increment an error counter.
- Emit rate-limited logging.
- Eventually produce the stale quality state if no valid message arrives.

The adapter must bound accepted payload length even though the MVP only parses
scalars. This prevents oversized MQTT messages from causing uncontrolled
allocation or logging.

## Testing

Unit tests do not require a broker:

- Create, update, and snapshot each supported scalar slot type.
- Verify value, status, timestamp, and sequence consistency.
- Verify initial, Good, unavailable, and recovery quality transitions.
- Verify repeated identical state transitions are idempotent.
- Verify strict scalar parsing and oversized payload rejection.
- Verify SQLite MQTT configuration defaults, validation, and round trip.
- Verify callback-backed OPC UA Read returns the expected `UA_DataValue`.
- Verify the data Node rejects OPC UA client writes.
- Verify Subscription notification on value and quality changes.
- Verify no notification for an unchanged value and unchanged quality.

The Mosquitto integration test:

1. Starts the daemon with a temporary database and test mapping.
2. Connects to an externally supplied local Mosquitto broker.
3. Publishes `37.5` to `test/temperature` at QoS 1.
4. Reads or subscribes to `ns=2;i=1001`.
5. Verifies `37.5`, `Good`, and a SourceTimestamp.
6. Stops or disconnects the broker and verifies the unavailable state.
7. Restarts the broker, publishes a value, and verifies recovery to `Good`.

Broker-dependent tests are explicitly enabled and are not required for the
default broker-free unit test run. Native build and unit verification remain
available on GCC, Clang, and MSVC.

## Extension Path

After this MVP is verified:

1. Add multiple source and Node mappings.
2. Add JSON payload decoding with JSON Pointer mappings.
3. Add MQTT TLS and authentication.
4. Add Sparkplug B Protobuf decoding and Birth/Death state handling.
5. Add an external historian client that subscribes to OPC UA and writes a
   time-series database.

All later decoders produce the same typed store update, so they do not change
the callback-backed OPC UA storage model.
