#pragma once

#include <string_view>

namespace uv::http::detail {

class http1_state_machine {
public:
  [[nodiscard]] static bool llhttp_available() noexcept;
  [[nodiscard]] static std::string_view backend_name() noexcept;
};

} // namespace uv::http::detail

