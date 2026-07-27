# Eclipse Paho MQTT C Provenance

This tree is Eclipse Paho MQTT C 1.3.16, imported from the annotated upstream
tag `v1.3.16`. Its tag object ID is
`b830b1d8fe272dca0f6fcb52eab7a69ca67d3a5f`; that tag peels to commit
`4a939ddb01eea581a32fd6f0adcfee51b91d2601`.

The application sets `PAHO_ENABLE_CPACK=OFF`. The vendored `CMakeLists.txt` has
one local build-system patch: Paho's final CPack version setup and
`include(CPack)` are guarded by that existing option. This prevents dependency
configuration from creating root packaging files and a Paho-branded `PACKAGE`
target. Runtime sources, headers, licenses, and notices are unchanged.
