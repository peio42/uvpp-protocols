cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _uvpp_protocols_version)
string(STRIP "${_uvpp_protocols_version}" _uvpp_protocols_version)

project(
  uvpp-protocols
  VERSION ${_uvpp_protocols_version}
  DESCRIPTION "Higher-level protocol modules for uvpp"
  HOMEPAGE_URL "https://github.com/peio42/uvpp-protocols"
  LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

option(UVPP_PROTOCOLS_BUILD_EXAMPLES "Build uvpp-protocols examples" ${PROJECT_IS_TOP_LEVEL})
option(UVPP_PROTOCOLS_BUILD_TESTS "Build uvpp-protocols tests" ${PROJECT_IS_TOP_LEVEL})
option(UVPP_PROTOCOLS_FETCH_UVPP "Fetch uvpp when uvpp::uvpp is not already available" ON)
option(UVPP_PROTOCOLS_UPDATE_FETCHED_UVPP "Allow CMake to update an already fetched uvpp checkout" OFF)
option(UVPP_PROTOCOLS_FETCH_LLHTTP "Fetch llhttp when it is not already available" ON)
option(UVPP_PROTOCOLS_UPDATE_FETCHED_LLHTTP "Allow CMake to update an already fetched llhttp checkout" OFF)
option(UVPP_PROTOCOLS_FETCH_NLOHMANN_JSON "Fetch nlohmann/json when nlohmann_json::nlohmann_json is not already available" ON)
option(UVPP_PROTOCOLS_UPDATE_FETCHED_NLOHMANN_JSON "Allow CMake to update an already fetched nlohmann/json checkout" OFF)
set(UVPP_PROTOCOLS_LLHTTP_SOURCE_DIR
    ""
    CACHE PATH "Use an existing llhttp source checkout instead of fetching one")

add_library(uvpp_protocols)
add_library(uvpp::protocols ALIAS uvpp_protocols)

target_sources(
  uvpp_protocols
  PRIVATE
    src/dns.cpp
    src/io/transport.cpp
    src/url.cpp
    src/http/client.cpp
    src/http/detail/http1_state_machine.cpp
    src/http/error.cpp
    src/http/headers.cpp
    src/http/method.cpp
    src/http/multipart.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/route_path.cpp
    src/http/route_params.cpp
    src/http/router.cpp
    src/http/server.cpp
    src/http/server_options.cpp
    src/http/static_files.cpp
    src/http/status.cpp
    src/http/upgrade.cpp
    src/tls/context.cpp
    src/tls/error.cpp
    src/tls/listener.cpp
    src/tls/stream.cpp
    src/websocket/detail/handshake.cpp
    src/websocket/session.cpp)

target_include_directories(
  uvpp_protocols
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_compile_features(uvpp_protocols PUBLIC cxx_std_20)

set_target_properties(
  uvpp_protocols
  PROPERTIES
    EXPORT_NAME protocols
    OUTPUT_NAME uvpp-protocols)

include(cmake/Dependencies.cmake)
uvpp_protocols_require_uvpp()
uvpp_protocols_require_llhttp(uvpp_protocols)
uvpp_protocols_require_nlohmann_json(uvpp_protocols)
uvpp_protocols_require_openssl_crypto(uvpp_protocols)
uvpp_protocols_require_openssl_ssl(uvpp_protocols)
target_include_directories(
  uvpp_protocols
  PRIVATE
    $<TARGET_PROPERTY:uvpp::uvpp,INTERFACE_INCLUDE_DIRECTORIES>)

if(UVPP_PROTOCOLS_BUILD_EXAMPLES)
  function(uvpp_protocols_add_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE uvpp::protocols uvpp::uvpp)
  endfunction()

  uvpp_protocols_add_example(uvpp_protocols_http_server_example examples/http_server.cpp)
  uvpp_protocols_add_example(uvpp_protocols_admin_endpoints_example examples/admin_endpoints.cpp)
  uvpp_protocols_add_example(uvpp_protocols_local_json_api_example examples/local_json_api.cpp)
  uvpp_protocols_add_example(uvpp_protocols_log_streaming_example examples/log_streaming.cpp)
  uvpp_protocols_add_example(uvpp_protocols_websocket_echo_example examples/websocket_echo.cpp)
endif()

if(UVPP_PROTOCOLS_BUILD_TESTS)
  enable_testing()
  add_executable(
    uvpp_protocols_tests
    tests/test_main.cpp
    tests/dns_test.cpp
    tests/http1_parser_test.cpp
    tests/http_request_test.cpp
    tests/http_response_test.cpp
    tests/http_client_test.cpp
    tests/http_router_test.cpp
    tests/http_server_integration_test.cpp
    tests/io_test.cpp
    tests/multipart_test.cpp
    tests/tls_stream_test.cpp
    tests/url_test.cpp
    tests/websocket_test.cpp)
  target_include_directories(uvpp_protocols_tests PRIVATE ${PROJECT_SOURCE_DIR})
  target_link_libraries(uvpp_protocols_tests PRIVATE uvpp::protocols uvpp::uvpp)
  add_test(NAME uvpp_protocols_tests COMMAND uvpp_protocols_tests)
endif()

install(
  TARGETS uvpp_protocols
  EXPORT uvpp-protocolsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
  EXPORT uvpp-protocolsTargets
  NAMESPACE uvpp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uvpp-protocols)

configure_package_config_file(
  cmake/uvpp-protocolsConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/uvpp-protocolsConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uvpp-protocols)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/uvpp-protocolsConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/uvpp-protocolsConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/uvpp-protocolsConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/uvpp-protocols)
