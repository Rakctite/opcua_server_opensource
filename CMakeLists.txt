cmake_minimum_required(VERSION 3.20)
project(opcua_server_opensource LANGUAGES C CXX)

option(OPCUA_BUILD_TESTS "Build tests" ON)
option(OPCUA_MQTT_INTEGRATION_TESTS "Build Mosquitto-backed MQTT integration test" OFF)
set(MOSQUITTO_EXECUTABLE "" CACHE FILEPATH "Path to the Mosquitto broker executable for MQTT integration tests")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

add_library(opcua_common INTERFACE)
target_include_directories(opcua_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

find_package(Threads REQUIRED)
add_library(sqlite3_static STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_link_libraries(sqlite3_static PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
  target_link_libraries(sqlite3_static PUBLIC ${CMAKE_DL_LIBS})
endif()

add_library(opcua_config
  src/config/server_config.cc
  src/config/mqtt_config.cc
  src/config/sqlite_db.cc
  src/config/config_repository.cc
)
target_link_libraries(opcua_config PUBLIC opcua_common sqlite3_static)
opcua_target_warnings(opcua_config)

add_library(open62541_static STATIC third_party/open62541/open62541.c)
target_include_directories(open62541_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/open62541")
if(WIN32)
  target_compile_options(open62541_static PRIVATE "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
  target_link_libraries(open62541_static PUBLIC ws2_32 iphlpapi)
endif()

set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PAHO_WITH_SSL OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(PAHO_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(PAHO_ENABLE_CPACK OFF CACHE BOOL "" FORCE)
set(PAHO_HIGH_PERFORMANCE ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/paho.mqtt.c EXCLUDE_FROM_ALL)

add_library(opcua_daemon_lib
  src/daemon/mqtt_adapter.cc
  src/daemon/mqtt_payload_parser.cc
  src/daemon/opcua_server.cc
  src/daemon/realtime_address_space.cc
  src/daemon/realtime_value_store.cc
)
target_link_libraries(opcua_daemon_lib PUBLIC
  opcua_common
  opcua_config
  open62541_static
  paho-mqtt3a-static
)
opcua_target_warnings(opcua_daemon_lib)

add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib"
)
target_compile_options(cpp_httplib INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
target_link_libraries(cpp_httplib INTERFACE "$<$<PLATFORM_ID:Windows>:ws2_32>")

add_library(opcua_supervisor_lib
  src/supervisor/process_controller.cc
  src/supervisor/api_server.cc
  src/supervisor/config_json_codec.cc
  src/supervisor/supervisor_exit.cc
  src/supervisor/supervisor_options.cc
)
target_link_libraries(opcua_supervisor_lib PUBLIC opcua_common opcua_config cpp_httplib)
opcua_target_warnings(opcua_supervisor_lib)

add_executable(opcua-daemon apps/opcua-daemon/main.cc)
target_link_libraries(opcua-daemon PRIVATE opcua_daemon_lib)
opcua_target_warnings(opcua-daemon)

add_executable(opcua-supervisor apps/opcua-supervisor/main.cc)
target_link_libraries(opcua-supervisor PRIVATE opcua_supervisor_lib)
opcua_target_warnings(opcua-supervisor)

if(OPCUA_BUILD_TESTS)
  enable_testing()

  if(OPCUA_MQTT_INTEGRATION_TESTS)
    if(NOT MOSQUITTO_EXECUTABLE OR NOT EXISTS "${MOSQUITTO_EXECUTABLE}")
      message(FATAL_ERROR
        "OPCUA_MQTT_INTEGRATION_TESTS requires MOSQUITTO_EXECUTABLE to point to a Mosquitto executable")
    endif()
  endif()

  add_executable(config_validation_test
    tests/config_validation_test.cc
  )
  target_link_libraries(config_validation_test PRIVATE opcua_config)
  opcua_target_warnings(config_validation_test)
  add_test(NAME config_validation_test COMMAND config_validation_test)

  add_executable(mqtt_config_validation_test
    tests/mqtt_config_validation_test.cc
  )
  target_link_libraries(mqtt_config_validation_test PRIVATE opcua_config)
  opcua_target_warnings(mqtt_config_validation_test)
  add_test(NAME mqtt_config_validation_test COMMAND mqtt_config_validation_test)

  add_executable(config_repository_test tests/config_repository_test.cc)
  target_link_libraries(config_repository_test PRIVATE opcua_config)
  opcua_target_warnings(config_repository_test)
  add_test(NAME config_repository_test COMMAND config_repository_test)

  add_executable(mqtt_config_repository_test
    tests/mqtt_config_repository_test.cc
  )
  target_link_libraries(mqtt_config_repository_test PRIVATE opcua_config)
  opcua_target_warnings(mqtt_config_repository_test)
  add_test(NAME mqtt_config_repository_test COMMAND mqtt_config_repository_test)

  add_executable(daemon_smoke_test tests/daemon_smoke_test.cc)
  target_link_libraries(daemon_smoke_test PRIVATE opcua_daemon_lib)
  target_compile_definitions(daemon_smoke_test PRIVATE
    OPCUA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  opcua_target_warnings(daemon_smoke_test)
  add_test(NAME daemon_smoke_test COMMAND daemon_smoke_test)

  add_executable(realtime_value_store_test
    tests/realtime_value_store_test.cc
  )
  target_link_libraries(realtime_value_store_test PRIVATE opcua_daemon_lib)
  opcua_target_warnings(realtime_value_store_test)
  add_test(NAME realtime_value_store_test COMMAND realtime_value_store_test)

  add_executable(mqtt_payload_parser_test
    tests/mqtt_payload_parser_test.cc
  )
  target_link_libraries(mqtt_payload_parser_test PRIVATE opcua_daemon_lib)
  opcua_target_warnings(mqtt_payload_parser_test)
  add_test(NAME mqtt_payload_parser_test COMMAND mqtt_payload_parser_test)

  add_executable(mqtt_adapter_test
    tests/mqtt_adapter_test.cc
  )
  target_link_libraries(mqtt_adapter_test PRIVATE opcua_daemon_lib)
  opcua_target_warnings(mqtt_adapter_test)
  add_test(NAME mqtt_adapter_test COMMAND mqtt_adapter_test)

  if(OPCUA_MQTT_INTEGRATION_TESTS)
    add_executable(mqtt_integration_test
      tests/mqtt_integration_test.cc
    )
    target_link_libraries(mqtt_integration_test PRIVATE
      opcua_daemon_lib
      opcua_supervisor_lib
      paho-mqtt3a-static
    )
    opcua_target_warnings(mqtt_integration_test)
    add_test(NAME mqtt_integration_test
      COMMAND mqtt_integration_test
        "${MOSQUITTO_EXECUTABLE}"
        $<TARGET_FILE:opcua-daemon>
    )
    set_tests_properties(mqtt_integration_test PROPERTIES TIMEOUT 30)
  endif()

  add_executable(realtime_address_space_test
    tests/realtime_address_space_test.cc
  )
  target_link_libraries(realtime_address_space_test PRIVATE opcua_daemon_lib)
  opcua_target_warnings(realtime_address_space_test)
  add_test(NAME realtime_address_space_test COMMAND realtime_address_space_test)

  add_executable(test_child tests/test_child/main.cc)
  opcua_target_warnings(test_child)

  add_executable(crashing_child tests/crashing_child/main.cc)
  opcua_target_warnings(crashing_child)

  if(WIN32)
    add_executable(exit_259_child tests/exit_259_child/main.cc)
    opcua_target_warnings(exit_259_child)

    add_executable(windows_exit_code_test tests/windows_exit_code_test.cc)
    target_link_libraries(windows_exit_code_test PRIVATE opcua_supervisor_lib)
    opcua_target_warnings(windows_exit_code_test)
    add_test(NAME windows_exit_code_test
      COMMAND windows_exit_code_test $<TARGET_FILE:exit_259_child>
    )
    set_tests_properties(windows_exit_code_test PROPERTIES TIMEOUT 10)
  endif()

  add_executable(process_controller_test tests/process_controller_test.cc)
  target_link_libraries(process_controller_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(process_controller_test)
  add_test(NAME process_controller_test
    COMMAND process_controller_test $<TARGET_FILE:test_child>
  )

  add_executable(process_crash_test tests/process_crash_test.cc)
  target_link_libraries(process_crash_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(process_crash_test)
  add_test(NAME process_crash_test
    COMMAND process_crash_test
      $<TARGET_FILE:crashing_child>
      $<TARGET_FILE:test_child>
  )
  set_tests_properties(process_crash_test PROPERTIES TIMEOUT 10)

  add_executable(process_shutdown_test tests/process_shutdown_test.cc)
  target_link_libraries(process_shutdown_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(process_shutdown_test)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(stubborn_child tests/stubborn_child/main.cc)
    opcua_target_warnings(stubborn_child)
    add_test(NAME process_shutdown_test
      COMMAND process_shutdown_test
        $<TARGET_FILE:test_child>
        $<TARGET_FILE:stubborn_child>
    )
  else()
    add_test(NAME process_shutdown_test
      COMMAND process_shutdown_test $<TARGET_FILE:test_child>
    )
  endif()
  set_tests_properties(process_shutdown_test PROPERTIES TIMEOUT 10)

  add_executable(supervisor_exit_test tests/supervisor_exit_test.cc)
  target_link_libraries(supervisor_exit_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(supervisor_exit_test)
  add_test(NAME supervisor_exit_test COMMAND supervisor_exit_test)
  set_tests_properties(supervisor_exit_test PROPERTIES TIMEOUT 5)

  add_executable(supervisor_options_test tests/supervisor_options_test.cc)
  target_link_libraries(supervisor_options_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(supervisor_options_test)
  add_test(NAME supervisor_options_test COMMAND supervisor_options_test)
  set_tests_properties(supervisor_options_test PROPERTIES TIMEOUT 5)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(supervisor_signal_test tests/supervisor_signal_test.cc)
    opcua_target_warnings(supervisor_signal_test)
    add_test(NAME supervisor_signal_test
      COMMAND supervisor_signal_test
        $<TARGET_FILE:opcua-supervisor>
        $<TARGET_FILE:opcua-daemon>
    )
    set_tests_properties(supervisor_signal_test PROPERTIES TIMEOUT 30)
  endif()

  add_executable(api_server_test tests/api_server_test.cc)
  target_link_libraries(api_server_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(api_server_test)
  add_test(NAME api_server_test
    COMMAND api_server_test $<TARGET_FILE:test_child>
  )
  set_tests_properties(api_server_test PROPERTIES TIMEOUT 30)
endif()
