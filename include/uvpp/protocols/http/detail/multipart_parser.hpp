#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <uvpp/protocols/http/error.hpp>
#include <uvpp/protocols/result.hpp>

namespace uvp::http::detail {

struct multipart_disposition {
  std::string name;
  std::optional<std::string> filename;
};

[[nodiscard]] uvp::result<std::string> parse_multipart_boundary(std::string_view content_type);
[[nodiscard]] uvp::result<multipart_disposition> parse_multipart_content_disposition(std::string_view value);

} // namespace uvp::http::detail
