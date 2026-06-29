#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <uvpp/protocols/http/route_path_matching.hpp>

namespace uvp::http::detail {

struct route_path {
  std::vector<std::string> raw_segments;
  std::vector<std::string> decoded_segments;
  bool valid = true;

  [[nodiscard]] std::span<const std::string> segments(route_path_matching matching) const noexcept;
};

[[nodiscard]] route_path parse_route_path(std::string_view path);
[[nodiscard]] std::string join_route_segments(std::span<const std::string> segments, std::size_t first = 0);

} // namespace uvp::http::detail
