#include <uvpp/protocols/http/server_options.hpp>

#include <utility>

namespace uvp::http {

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

server_options& server_options::max_header_bytes(std::size_t value) & {
  require_positive(value, "max_header_bytes must be greater than zero");
  max_header_bytes_ = value;
  return *this;
}

server_options&& server_options::max_header_bytes(std::size_t value) && {
  max_header_bytes(value);
  return std::move(*this);
}

server_options& server_options::max_body_bytes(std::size_t value) & {
  require_positive(value, "max_body_bytes must be greater than zero");
  max_body_bytes_ = value;
  return *this;
}

server_options&& server_options::max_body_bytes(std::size_t value) && {
  max_body_bytes(value);
  return std::move(*this);
}

server_options& server_options::max_pending_write_bytes(std::size_t value) & {
  require_positive(value, "max_pending_write_bytes must be greater than zero");
  max_pending_write_bytes_ = value;
  return *this;
}

server_options&& server_options::max_pending_write_bytes(std::size_t value) && {
  max_pending_write_bytes(value);
  return std::move(*this);
}

server_options& server_options::max_pending_responses_per_connection(std::size_t value) & {
  require_positive(value, "max_pending_responses_per_connection must be greater than zero");
  max_pending_responses_per_connection_ = value;
  return *this;
}

server_options&& server_options::max_pending_responses_per_connection(std::size_t value) && {
  max_pending_responses_per_connection(value);
  return std::move(*this);
}

server_options& server_options::header_timeout(std::chrono::milliseconds value) & {
  require_positive(value, "header_timeout must be greater than zero");
  header_timeout_ = value;
  return *this;
}

server_options&& server_options::header_timeout(std::chrono::milliseconds value) && {
  header_timeout(value);
  return std::move(*this);
}

server_options& server_options::body_timeout(std::chrono::milliseconds value) & {
  require_positive(value, "body_timeout must be greater than zero");
  body_timeout_ = value;
  return *this;
}

server_options&& server_options::body_timeout(std::chrono::milliseconds value) && {
  body_timeout(value);
  return std::move(*this);
}

server_options& server_options::idle_timeout(std::chrono::milliseconds value) & {
  require_positive(value, "idle_timeout must be greater than zero");
  idle_timeout_ = value;
  return *this;
}

server_options&& server_options::idle_timeout(std::chrono::milliseconds value) && {
  idle_timeout(value);
  return std::move(*this);
}

server_options& server_options::keep_alive(bool value) & noexcept {
  keep_alive_ = value;
  return *this;
}

server_options&& server_options::keep_alive(bool value) && noexcept {
  keep_alive(value);
  return std::move(*this);
}

server_options& server_options::server_header(bool value) & noexcept {
  server_header_ = value;
  return *this;
}

server_options&& server_options::server_header(bool value) && noexcept {
  server_header(value);
  return std::move(*this);
}

server_options& server_options::route_path_matching(http::route_path_matching value) & noexcept {
  route_path_matching_ = value;
  return *this;
}

server_options&& server_options::route_path_matching(http::route_path_matching value) && noexcept {
  route_path_matching(value);
  return std::move(*this);
}

void server_options::validate() const {
  require_positive(max_header_bytes_, "max_header_bytes must be greater than zero");
  require_positive(max_body_bytes_, "max_body_bytes must be greater than zero");
  require_positive(max_pending_write_bytes_, "max_pending_write_bytes must be greater than zero");
  require_positive(max_pending_responses_per_connection_, "max_pending_responses_per_connection must be greater than zero");
  require_positive(header_timeout_, "header_timeout must be greater than zero");
  require_positive(body_timeout_, "body_timeout must be greater than zero");
  require_positive(idle_timeout_, "idle_timeout must be greater than zero");
}

} // namespace uvp::http
