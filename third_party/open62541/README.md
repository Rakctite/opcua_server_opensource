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
