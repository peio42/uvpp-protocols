#pragma once

#include <chrono>
#include <cstddef>

#include <uvpp/protocols/http/route_path_matching.hpp>

namespace uvp::http {

struct server_options {
  server_options& max_header_bytes(std::size_t value) &;
  server_options&& max_header_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_header_bytes() const noexcept { return max_header_bytes_; }

  server_options& max_body_bytes(std::size_t value) &;
  server_options&& max_body_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_body_bytes() const noexcept { return max_body_bytes_; }

  server_options& max_pending_write_bytes(std::size_t value) &;
  server_options&& max_pending_write_bytes(std::size_t value) &&;
  [[nodiscard]] std::size_t max_pending_write_bytes() const noexcept { return max_pending_write_bytes_; }

  server_options& max_pending_responses_per_connection(std::size_t value) &;
  server_options&& max_pending_responses_per_connection(std::size_t value) &&;
  [[nodiscard]] std::size_t max_pending_responses_per_connection() const noexcept {
    return max_pending_responses_per_connection_;
  }

  server_options& header_timeout(std::chrono::milliseconds value) &;
  server_options&& header_timeout(std::chrono::milliseconds value) &&;
  [[nodiscard]] std::chrono::milliseconds header_timeout() const noexcept { return header_timeout_; }

  server_options& body_timeout(std::chrono::milliseconds value) &;
  server_options&& body_timeout(std::chrono::milliseconds value) &&;
  [[nodiscard]] std::chrono::milliseconds body_timeout() const noexcept { return body_timeout_; }

  server_options& idle_timeout(std::chrono::milliseconds value) &;
  server_options&& idle_timeout(std::chrono::milliseconds value) &&;
  [[nodiscard]] std::chrono::milliseconds idle_timeout() const noexcept { return idle_timeout_; }

  server_options& keep_alive(bool value) & noexcept;
  server_options&& keep_alive(bool value) && noexcept;
  [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

  server_options& server_header(bool value) & noexcept;
  server_options&& server_header(bool value) && noexcept;
  [[nodiscard]] bool server_header() const noexcept { return server_header_; }

  server_options& route_path_matching(http::route_path_matching value) & noexcept;
  server_options&& route_path_matching(http::route_path_matching value) && noexcept;
  [[nodiscard]] http::route_path_matching route_path_matching() const noexcept { return route_path_matching_; }

  void validate() const;

private:
  std::size_t max_header_bytes_ = 16 * 1024;
  std::size_t max_body_bytes_ = 1024 * 1024;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::size_t max_pending_responses_per_connection_ = 16;

  std::chrono::milliseconds header_timeout_ = std::chrono::seconds{10};
  std::chrono::milliseconds body_timeout_ = std::chrono::seconds{30};
  std::chrono::milliseconds idle_timeout_ = std::chrono::seconds{60};

  bool keep_alive_ = true;
  bool server_header_ = true;
  http::route_path_matching route_path_matching_ = http::route_path_matching::percent_decoded_segments;
};

} // namespace uvp::http
