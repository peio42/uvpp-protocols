#pragma once

#include <chrono>
#include <cstddef>

namespace uv::http {

struct server_options {
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 1024 * 1024;
  std::size_t max_pending_write_bytes = 1024 * 1024;

  std::chrono::milliseconds header_timeout = std::chrono::seconds{10};
  std::chrono::milliseconds body_timeout = std::chrono::seconds{30};
  std::chrono::milliseconds idle_timeout = std::chrono::seconds{60};

  bool keep_alive = true;
  bool server_header = true;

  class builder_type;

  [[nodiscard]] static builder_type builder();
};

class server_options::builder_type {
public:
  builder_type& max_header_bytes(std::size_t value);
  builder_type& max_body_bytes(std::size_t value);
  builder_type& max_pending_write_bytes(std::size_t value);
  builder_type& header_timeout(std::chrono::milliseconds value);
  builder_type& body_timeout(std::chrono::milliseconds value);
  builder_type& idle_timeout(std::chrono::milliseconds value);
  builder_type& keep_alive(bool value) noexcept;
  builder_type& server_header(bool value) noexcept;

  [[nodiscard]] server_options build() const;

private:
  server_options options_;
};

} // namespace uv::http
