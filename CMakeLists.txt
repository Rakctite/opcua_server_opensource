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

add_library(open62541_static STATIC third_party/open62541/open62541.c)
target_include_directories(open62541_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/open62541")
if(WIN32)
  target_compile_options(open62541_static PRIVATE "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
  target_link_libraries(open62541_static PUBLIC ws2_32 iphlpapi)
endif()

add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
target_link_libraries(cpp_httplib INTERFACE "$<$<PLATFORM_ID:Windows>:ws2_32>")

add_executable(opcua-daemon apps/opcua-daemon/main.cc)
target_link_libraries(opcua-daemon PRIVATE opcua_common open62541_static)
opcua_target_warnings(opcua-daemon)

add_executable(opcua-supervisor apps/opcua-supervisor/main.cc)
target_link_libraries(opcua-supervisor PRIVATE opcua_common cpp_httplib)
opcua_target_warnings(opcua-supervisor)

if(OPCUA_BUILD_TESTS)
  enable_testing()
endif()
