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

find_package(Threads REQUIRED)
add_library(sqlite3_static STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
target_link_libraries(sqlite3_static PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
  target_link_libraries(sqlite3_static PUBLIC ${CMAKE_DL_LIBS})
endif()

add_library(opcua_config
  src/config/server_config.cc
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

add_library(opcua_daemon_lib
  src/daemon/opcua_server.cc
)
target_link_libraries(opcua_daemon_lib PUBLIC opcua_common opcua_config open62541_static)
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
  add_executable(config_validation_test
    tests/config_validation_test.cc
  )
  target_link_libraries(config_validation_test PRIVATE opcua_config)
  opcua_target_warnings(config_validation_test)
  add_test(NAME config_validation_test COMMAND config_validation_test)

  add_executable(config_repository_test tests/config_repository_test.cc)
  target_link_libraries(config_repository_test PRIVATE opcua_config)
  opcua_target_warnings(config_repository_test)
  add_test(NAME config_repository_test COMMAND config_repository_test)

  add_executable(daemon_smoke_test tests/daemon_smoke_test.cc)
  target_link_libraries(daemon_smoke_test PRIVATE opcua_daemon_lib)
  opcua_target_warnings(daemon_smoke_test)
  add_test(NAME daemon_smoke_test COMMAND daemon_smoke_test)

  add_executable(test_child tests/test_child/main.cc)
  opcua_target_warnings(test_child)

  add_executable(process_controller_test tests/process_controller_test.cc)
  target_link_libraries(process_controller_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(process_controller_test)
  add_test(NAME process_controller_test
    COMMAND process_controller_test $<TARGET_FILE:test_child>
  )

  add_executable(api_server_test tests/api_server_test.cc)
  target_link_libraries(api_server_test PRIVATE opcua_supervisor_lib)
  opcua_target_warnings(api_server_test)
  add_test(NAME api_server_test
    COMMAND api_server_test $<TARGET_FILE:test_child>
  )
endif()
