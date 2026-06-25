#pragma once

#include <string>
#include <string_view>

namespace uvp::websocket::detail {

[[nodiscard]] std::string websocket_accept_value(std::string_view key);

} // namespace uvp::websocket::detail

