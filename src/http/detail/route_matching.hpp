#pragma once

#include <string_view>

#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http::detail {

bool route_pattern_matches(std::string_view pattern, std::string_view path, route_params& params);

} // namespace uvp::http::detail
