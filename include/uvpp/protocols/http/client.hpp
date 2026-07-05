#pragma once

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/result.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace uv {
class loop;
}

namespace uvp::http {

struct client_options {
  std::size_t max_header_bytes = 64 * 1024;
  std::size_t max_body_bytes = 4 * 1024 * 1024;
  std::chrono::milliseconds dns_timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds response_header_timeout{0};
  std::chrono::milliseconds response_body_timeout{0};
};

using client_callback = std::function<void(uvp::result<http::response>)>;

class request_operation {
public:
  request_operation() = default;
  explicit request_operation(std::shared_ptr<void> state);

  void cancel() noexcept;
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
  std::shared_ptr<void> state_;
};

class client {
public:
  explicit client(uv::loop& loop);
  client(uv::loop& loop, client_options options);

  [[nodiscard]] request_operation get(std::string_view url, client_callback callback);
  [[nodiscard]] request_operation fetch(http::method method, std::string_view url, client_callback callback);

private:
  uv::loop* loop_;
  client_options options_;
};

} // namespace uvp::http
