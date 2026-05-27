#include <uvpp/protocols/http/server_options.hpp>

namespace uv::http {

namespace {

void require_positive(std::size_t value, const char* name) {
  if (value == 0) {
    throw std::invalid_argument(name);
  }
}

void require_positive(std::chrono::milliseconds value, const char* name) {
  if (value.count() <= 0) {
    throw std::invalid_argument(name);
  }
}

} // namespace

server_options::builder_type& server_options::builder_type::max_header_bytes(std::size_t value) {
  require_positive(value, "max_header_bytes must be greater than zero");
  options_.max_header_bytes = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::max_body_bytes(std::size_t value) {
  options_.max_body_bytes = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::max_pending_write_bytes(std::size_t value) {
  require_positive(value, "max_pending_write_bytes must be greater than zero");
  options_.max_pending_write_bytes = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::header_timeout(std::chrono::milliseconds value) {
  require_positive(value, "header_timeout must be greater than zero");
  options_.header_timeout = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::body_timeout(std::chrono::milliseconds value) {
  require_positive(value, "body_timeout must be greater than zero");
  options_.body_timeout = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::idle_timeout(std::chrono::milliseconds value) {
  require_positive(value, "idle_timeout must be greater than zero");
  options_.idle_timeout = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::keep_alive(bool value) noexcept {
  options_.keep_alive = value;
  return *this;
}

server_options::builder_type& server_options::builder_type::server_header(bool value) noexcept {
  options_.server_header = value;
  return *this;
}

server_options server_options::builder_type::build() const {
  require_positive(options_.max_header_bytes, "max_header_bytes must be greater than zero");
  require_positive(options_.max_pending_write_bytes, "max_pending_write_bytes must be greater than zero");
  require_positive(options_.header_timeout, "header_timeout must be greater than zero");
  require_positive(options_.body_timeout, "body_timeout must be greater than zero");
  require_positive(options_.idle_timeout, "idle_timeout must be greater than zero");
  return options_;
}

server_options::builder_type server_options::builder() {
  return builder_type{};
}

} // namespace uv::http

