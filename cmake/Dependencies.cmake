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
