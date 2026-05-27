#pragma once

#include <string_view>

namespace uv::http::detail {

class http2_state_machine {
public:
  [[nodiscard]] static bool nghttp2_available() noexcept;
  [[nodiscard]] static std::string_view backend_name() noexcept;
};

} // namespace uv::http::detail

