include_guard(GLOBAL)

function(uvpp_protocols_require_uvpp)
  if(TARGET uvpp::uvpp)
    return()
  endif()

  find_package(uvpp CONFIG QUIET)
  if(TARGET uvpp::uvpp)
    return()
  endif()

  if(NOT UVPP_PROTOCOLS_FETCH_UVPP)
    message(FATAL_ERROR "uvpp::uvpp was not found. Enable UVPP_PROTOCOLS_FETCH_UVPP or install uvpp.")
  endif()

  if(NOT UVPP_PROTOCOLS_UPDATE_FETCHED_UVPP)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_UVPP ON)
  endif()

  FetchContent_Declare(
    uvpp
    GIT_REPOSITORY https://github.com/peio42/uvpp.git
    GIT_TAG main)

  FetchContent_GetProperties(uvpp)
  if(NOT uvpp_POPULATED)
    FetchContent_Populate(uvpp)
  endif()

  list(APPEND CMAKE_MODULE_PATH "${uvpp_SOURCE_DIR}/cmake")
  find_package(LibUV REQUIRED)
  find_package(Threads REQUIRED)

  add_library(uvpp_protocols_fetched_uvpp INTERFACE)
  add_library(uvpp::uvpp ALIAS uvpp_protocols_fetched_uvpp)

  target_compile_features(uvpp_protocols_fetched_uvpp INTERFACE cxx_std_20)
  target_include_directories(uvpp_protocols_fetched_uvpp INTERFACE "${uvpp_SOURCE_DIR}/include")
  target_link_libraries(uvpp_protocols_fetched_uvpp INTERFACE LibUV::LibUV Threads::Threads)

  if(NOT TARGET uvpp::uvpp)
    message(FATAL_ERROR "uvpp was fetched, but target uvpp::uvpp is not available")
  endif()
endfunction()

function(uvpp_protocols_add_llhttp_sources target source_dir)
  if(NOT EXISTS "${source_dir}/include/llhttp.h")
    message(FATAL_ERROR "llhttp source directory does not contain include/llhttp.h: ${source_dir}")
  endif()

  target_sources(
    ${target}
    PRIVATE
      "${source_dir}/src/llhttp.c"
      "${source_dir}/src/api.c"
      "${source_dir}/src/http.c")

  target_include_directories(${target} PRIVATE "${source_dir}/include")
endfunction()

function(uvpp_protocols_require_llhttp target)
  if(UVPP_PROTOCOLS_LLHTTP_SOURCE_DIR)
    uvpp_protocols_add_llhttp_sources(${target} "${UVPP_PROTOCOLS_LLHTTP_SOURCE_DIR}")
    message(STATUS "uvpp-protocols: HTTP/1 parser: llhttp from UVPP_PROTOCOLS_LLHTTP_SOURCE_DIR")
    message(STATUS "uvpp-protocols: HTTP/2 backend: not configured in current milestones")
    return()
  endif()

  if(NOT UVPP_PROTOCOLS_FETCH_LLHTTP)
    message(FATAL_ERROR "llhttp is required for HTTP/1. Enable UVPP_PROTOCOLS_FETCH_LLHTTP or set UVPP_PROTOCOLS_LLHTTP_SOURCE_DIR.")
  endif()

  if(NOT UVPP_PROTOCOLS_UPDATE_FETCHED_LLHTTP)
    set(FETCHCONTENT_UPDATES_DISCONNECTED_LLHTTP ON)
  endif()

  FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG release/v9.3.0)

  FetchContent_GetProperties(llhttp)
  if(NOT llhttp_POPULATED)
    FetchContent_Populate(llhttp)
  endif()

  uvpp_protocols_add_llhttp_sources(${target} "${llhttp_SOURCE_DIR}")
  message(STATUS "uvpp-protocols: HTTP/1 parser: llhttp")
  message(STATUS "uvpp-protocols: HTTP/2 backend: not configured in current milestones")
endfunction()

function(uvpp_protocols_require_nlohmann_json target)
  set(_uvpp_protocols_nlohmann_json_from_package OFF)
  if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG QUIET)
    if(TARGET nlohmann_json::nlohmann_json)
      set(_uvpp_protocols_nlohmann_json_from_package ON)
    endif()
  else()
    set(_uvpp_protocols_nlohmann_json_from_package ON)
  endif()

  if(NOT TARGET nlohmann_json::nlohmann_json)
    if(NOT UVPP_PROTOCOLS_FETCH_NLOHMANN_JSON)
      message(FATAL_ERROR "nlohmann_json::nlohmann_json was not found. Enable UVPP_PROTOCOLS_FETCH_NLOHMANN_JSON or install nlohmann_json.")
    endif()

    if(NOT UVPP_PROTOCOLS_UPDATE_FETCHED_NLOHMANN_JSON)
      set(FETCHCONTENT_UPDATES_DISCONNECTED_JSON ON)
    endif()

    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_Declare(
      json
      URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(json)
    set(UVPP_PROTOCOLS_BUNDLED_NLOHMANN_JSON ON PARENT_SCOPE)
  else()
    set(UVPP_PROTOCOLS_BUNDLED_NLOHMANN_JSON OFF PARENT_SCOPE)
  endif()

  if(NOT TARGET nlohmann_json::nlohmann_json)
    message(FATAL_ERROR "nlohmann_json was fetched, but target nlohmann_json::nlohmann_json is not available")
  endif()

  if(_uvpp_protocols_nlohmann_json_from_package)
    target_link_libraries(${target} PUBLIC nlohmann_json::nlohmann_json)
  else()
    target_include_directories(
      ${target}
      PUBLIC
        "$<BUILD_INTERFACE:${json_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    install(
      DIRECTORY "${json_SOURCE_DIR}/include/nlohmann"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  endif()

  message(STATUS "uvpp-protocols: JSON value: nlohmann/json")
endfunction()

function(uvpp_protocols_require_openssl_crypto target)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  target_link_libraries(${target} PRIVATE OpenSSL::Crypto)
  set(UVPP_PROTOCOLS_USES_OPENSSL_CRYPTO ON PARENT_SCOPE)
  message(STATUS "uvpp-protocols: WebSocket handshake digest: OpenSSL Crypto")
endfunction()
