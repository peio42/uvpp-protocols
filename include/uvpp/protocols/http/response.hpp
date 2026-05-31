#pragma once

#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/status.hpp>

namespace uvp::http {

class response {
public:
  response() = default;

  response& status(unsigned int code);
  response& status(http::status value);
  response& header(std::string_view name, std::string_view value);
  response& type(std::string_view content_type);

  void text(std::string_view body);
  void json(std::string_view serialized_json);
  void json(std::initializer_list<std::pair<std::string_view, std::string_view>> object);
  void bytes(std::span<const std::byte> body);
  void end();

  [[nodiscard]] unsigned int status_code() const noexcept { return status_code_; }
  [[nodiscard]] const http::headers& headers() const noexcept { return headers_; }
  [[nodiscard]] std::string_view body() const noexcept { return body_; }
  [[nodiscard]] bool ended() const noexcept { return ended_; }

private:
  unsigned int status_code_ = static_cast<unsigned int>(http::status::ok);
  http::headers headers_;
  std::string body_;
  bool ended_ = false;
};

} // namespace uvp::http

