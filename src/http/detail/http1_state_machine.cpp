#include "http1_state_machine.hpp"

#if UVPP_PROTOCOLS_DETAIL_HAS_LLHTTP
#include <llhttp.h>
#endif

namespace uv::http::detail {

bool http1_state_machine::llhttp_available() noexcept {
#if UVPP_PROTOCOLS_DETAIL_HAS_LLHTTP
  return true;
#else
  return false;
#endif
}

std::string_view http1_state_machine::backend_name() noexcept {
#if UVPP_PROTOCOLS_DETAIL_HAS_LLHTTP
  return "llhttp";
#else
  return "stub";
#endif
}

} // namespace uv::http::detail

