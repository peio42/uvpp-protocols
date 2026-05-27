cmake_minimum_required(VERSION 3.20)

project(
  uvpp-protocols
  VERSION 0.1.0
  DESCRIPTION "Higher-level protocol modules for uvpp"
  HOMEPAGE_URL "https://github.com/peio42/uvpp-protocols"
  LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(UVPP_PROTOCOLS_BUILD_EXAMPLES "Build uvpp-protocols examples" ${PROJECT_IS_TOP_LEVEL})
option(UVPP_PROTOCOLS_BUILD_TESTS "Build uvpp-protocols tests" ${PROJECT_IS_TOP_LEVEL})

set(UVPP_PROTOCOLS_HTTP1_BACKEND
    "auto"
    CACHE STRING "HTTP/1 state machine backend: auto, llhttp, stub")
set_property(CACHE UVPP_PROTOCOLS_HTTP1_BACKEND PROPERTY STRINGS auto llhttp stub)

set(UVPP_PROTOCOLS_HTTP2_BACKEND
    "auto"
    CACHE STRING "HTTP/2 state machine backend: auto, nghttp2, none")
set_property(CACHE UVPP_PROTOCOLS_HTTP2_BACKEND PROPERTY STRINGS auto nghttp2 none)

add_library(uvpp_protocols)
add_library(uvpp::protocols ALIAS uvpp_protocols)

target_sources(
  uvpp_protocols
  PRIVATE
    src/http/detail/http1_state_machine.cpp
    src/http/detail/http2_state_machine.cpp
    src/http/headers.cpp
    src/http/method.cpp
    src/http/request.cpp
    src/http/response.cpp
    src/http/route_params.cpp
    src/http/server.cpp
    src/http/server_options.cpp
    src/http/status.cpp)

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
uvpp_protocols_configure_http_backends(uvpp_protocols)

if(UVPP_PROTOCOLS_BUILD_EXAMPLES)
  add_executable(uvpp_protocols_http_server_example examples/http_server.cpp)
  target_link_libraries(uvpp_protocols_http_server_example PRIVATE uvpp::protocols)

  find_package(uvpp CONFIG QUIET)
  if(TARGET uvpp::uvpp)
    target_link_libraries(uvpp_protocols_http_server_example PRIVATE uvpp::uvpp)
    target_compile_definitions(uvpp_protocols_http_server_example PRIVATE UVPP_PROTOCOLS_EXAMPLE_HAS_UVPP=1)
  else()
    target_compile_definitions(uvpp_protocols_http_server_example PRIVATE UVPP_PROTOCOLS_EXAMPLE_HAS_UVPP=0)
  endif()
endif()

if(UVPP_PROTOCOLS_BUILD_TESTS)
  enable_testing()
  add_executable(uvpp_protocols_structure_test tests/structure_test.cpp)
  target_link_libraries(uvpp_protocols_structure_test PRIVATE uvpp::protocols)
  add_test(NAME uvpp_protocols_structure_test COMMAND uvpp_protocols_structure_test)
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
