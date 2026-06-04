#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <uvpp/protocols/http/headers.hpp>
#include <uvpp/protocols/http/method.hpp>

namespace uvp::http::detail {

struct http1_message {
  http::method method = http::method::unknown;
  std::string target;
  http::headers headers;
  std::string body;
  unsigned int http_major = 1;
  unsigned int http_minor = 1;
  bool keep_alive = false;
  bool upgrade = false;
};

struct http1_event {
  enum class type {
    headers,
    body,
    complete,
  };

  type event_type = type::complete;
  http1_message message;
  std::string body;
};

struct http1_parse_result {
  enum class status {
    ok,
    error,
    upgrade,
  };

  status code = status::ok;
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return code == status::ok; }
};

class http1_state_machine {
public:
  http1_state_machine();
  ~http1_state_machine();

  http1_state_machine(const http1_state_machine&) = delete;
  http1_state_machine& operator=(const http1_state_machine&) = delete;

  http1_state_machine(http1_state_machine&&) noexcept;
  http1_state_machine& operator=(http1_state_machine&&) noexcept;

  void reset();
  [[nodiscard]] http1_parse_result parse(std::string_view bytes);

  [[nodiscard]] const std::vector<http1_message>& completed_messages() const noexcept;
  [[nodiscard]] const std::vector<http1_event>& events() const noexcept;

private:
  class impl;

  std::unique_ptr<impl> impl_;
};

} // namespace uvp::http::detail
