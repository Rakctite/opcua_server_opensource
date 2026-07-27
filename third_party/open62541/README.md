# open62541

Vendored amalgamated open62541 source.

The build exposes this as the `open62541_static` CMake target so compile definitions and include paths stay isolated.

Version: v1.5.5
Source: https://github.com/open62541/open62541

## Local generated-config changes

`open62541.h` is not a byte-for-byte copy of the upstream v1.5.5 release asset.
The generated configuration macros were adjusted for this repository:

- `UA_ARCHITECTURE_POSIX` is not hard-coded. open62541's default architecture
  selection is used instead, so `_WIN32` selects `UA_ARCHITECTURE_WIN32` and
  other platforms select `UA_ARCHITECTURE_POSIX`.
- `UA_ENABLE_ENCRYPTION_MBEDTLS` is disabled because v1 does not vendor
  mbedTLS or OpenSSL dependencies. The integrated server currently supports
  only `security.mode=none` and `security.policy=none`.

These changes keep MSVC builds from requiring `pthread.h` and `mbedtls/md.h`.

## Local source patches

- Callback-backed Value reads preserve callback-provided `hasValue` and
  `hasStatus` flags when the callback returns `UA_STATUSCODE_GOOD`. This follows
  the `UA_CallbackValueSource` contract: caller-facing errors remain in
  `UA_DataValue.status`, while the callback return code is reserved for
  execution/logging. Empty non-nullable callback values still fall back to
  `BadWaitingForInitialData` when no explicit bad status was provided. Other
  attributes and internal/external value sources retain upstream behavior.
- `UA_Server_addCallbackValueSourceVariableNode` removes the raw nodestore node
  when callback-source setup or reference addition fails. This matches the
  atomic behavior of the generic add-node path and prevents failed additions
  from leaving a node that still references caller-owned context.
