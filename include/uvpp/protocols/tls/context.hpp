#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace uvp::tls {

struct context_access;

class server_context {
public:
  struct impl;

  server_context();
  ~server_context();

  server_context(const server_context&) = default;
  server_context& operator=(const server_context&) = default;
  server_context(server_context&&) noexcept = default;
  server_context& operator=(server_context&&) noexcept = default;

  server_context& certificate_chain_file(std::string_view path);
  server_context& private_key_file(std::string_view path);
  server_context& alpn(std::initializer_list<std::string_view> protocols);
  server_context& require_alpn(bool enabled = true) noexcept;
  server_context& max_pending_write_bytes(std::size_t value) noexcept;
  server_context& max_pending_read_bytes(std::size_t value) noexcept;

private:
  friend struct context_access;
  std::shared_ptr<impl> impl_;
};

class client_context {
public:
  struct impl;

  client_context();
  ~client_context();

  client_context(const client_context&) = default;
  client_context& operator=(const client_context&) = default;
  client_context(client_context&&) noexcept = default;
  client_context& operator=(client_context&&) noexcept = default;

  client_context& server_name(std::string_view name);
  client_context& default_verify_paths();
  client_context& ca_file(std::string_view path);
  client_context& ca_path(std::string_view path);
  client_context& alpn(std::initializer_list<std::string_view> protocols);
  client_context& insecure_no_verify_peer() noexcept;
  client_context& max_pending_write_bytes(std::size_t value) noexcept;
  client_context& max_pending_read_bytes(std::size_t value) noexcept;

private:
  friend struct context_access;
  std::shared_ptr<impl> impl_;
};

} // namespace uvp::tls
