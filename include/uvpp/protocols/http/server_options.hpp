#pragma once

#include <chrono>
#include <cstddef>

namespace uvp::http {

struct server_options {
  std::size_t max_header_bytes_ = 16 * 1024;
  std::size_t max_body_bytes_ = 1024 * 1024;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::size_t max_pending_responses_per_connection_ = 16;

  std::chrono::milliseconds header_timeout_ = std::chrono::seconds{10};
  std::chrono::milliseconds body_timeout_ = std::chrono::seconds{30};
  std::chrono::milliseconds idle_timeout_ = std::chrono::seconds{60};

  bool keep_alive_ = true;
  bool server_header_ = true;

  server_options& max_header_bytes(std::size_t value) &;
  server_options&& max_header_bytes(std::size_t value) &&;

  server_options& max_body_bytes(std::size_t value) &;
  server_options&& max_body_bytes(std::size_t value) &&;

  server_options& max_pending_write_bytes(std::size_t value) &;
  server_options&& max_pending_write_bytes(std::size_t value) &&;

  server_options& max_pending_responses_per_connection(std::size_t value) &;
  server_options&& max_pending_responses_per_connection(std::size_t value) &&;

  server_options& header_timeout(std::chrono::milliseconds value) &;
  server_options&& header_timeout(std::chrono::milliseconds value) &&;

  server_options& body_timeout(std::chrono::milliseconds value) &;
  server_options&& body_timeout(std::chrono::milliseconds value) &&;

  server_options& idle_timeout(std::chrono::milliseconds value) &;
  server_options&& idle_timeout(std::chrono::milliseconds value) &&;

  server_options& keep_alive(bool value = true) & noexcept;
  server_options&& keep_alive(bool value = true) && noexcept;

  server_options& server_header(bool value = true) & noexcept;
  server_options&& server_header(bool value = true) && noexcept;

  void validate() const;
};

} // namespace uvp::http
