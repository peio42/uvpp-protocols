#include "http2_state_machine.hpp"

#if UVPP_PROTOCOLS_DETAIL_HAS_NGHTTP2
#include <nghttp2/nghttp2.h>
#endif

namespace uv::http::detail {

bool http2_state_machine::nghttp2_available() noexcept {
#if UVPP_PROTOCOLS_DETAIL_HAS_NGHTTP2
  return true;
#else
  return false;
#endif
}

std::string_view http2_state_machine::backend_name() noexcept {
#if UVPP_PROTOCOLS_DETAIL_HAS_NGHTTP2
  return "libnghttp2";
#else
  return "none";
#endif
}

} // namespace uv::http::detail

