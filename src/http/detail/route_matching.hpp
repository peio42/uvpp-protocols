#pragma once

#include <string_view>

#include <uvpp/protocols/http/detail/route_path.hpp>
#include <uvpp/protocols/http/route_params.hpp>
#include <uvpp/protocols/http/route_path_matching.hpp>

namespace uvp::http::detail {

bool route_pattern_matches(
  const route_path& pattern,
  const route_path& path,
  route_path_matching matching,
  route_params& params);

bool route_pattern_matches(
  std::string_view pattern,
  const route_path& path,
  route_path_matching matching,
  route_params& params);

} // namespace uvp::http::detail
