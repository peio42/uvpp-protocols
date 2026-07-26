#pragma once

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uvp::tls {

struct context_access;

class server_context_options {
public:
  server_context_options& certificate_chain_file(std::string_view path) &;
  server_context_options&& certificate_chain_file(std::string_view path) &&;
  server_context_options& private_key_file(std::string_view path) &;
  server_context_options&& private_key_file(std::string_view path) &&;
  server_context_options& alpn(std::initializer_list<std::string_view> protocols) &;
  server_context_options&& alpn(std::initializer_list<std::string_view> protocols) &&;
  server_context_options& require_alpn(bool enabled = true) & noexcept;
  server_context_options&& require_alpn(bool enabled = true) && noexcept;
  server_context_options& max_pending_write_bytes(std::size_t value) & noexcept;
  server_context_options&& max_pending_write_bytes(std::size_t value) && noexcept;
  server_context_options& max_pending_read_bytes(std::size_t value) & noexcept;
  server_context_options&& max_pending_read_bytes(std::size_t value) && noexcept;

private:
  friend class server_context;

  std::string certificate_chain_file_;
  std::string private_key_file_;
  std::vector<std::string> alpn_;
  bool require_alpn_ = false;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::size_t max_pending_read_bytes_ = 1024 * 1024;
};

class client_context_options {
public:
  client_context_options& server_name(std::string_view name) &;
  client_context_options&& server_name(std::string_view name) &&;
  client_context_options& default_verify_paths(bool enabled = true) & noexcept;
  client_context_options&& default_verify_paths(bool enabled = true) && noexcept;
  client_context_options& ca_file(std::string_view path) &;
  client_context_options&& ca_file(std::string_view path) &&;
  client_context_options& ca_path(std::string_view path) &;
  client_context_options&& ca_path(std::string_view path) &&;
  client_context_options& alpn(std::initializer_list<std::string_view> protocols) &;
  client_context_options&& alpn(std::initializer_list<std::string_view> protocols) &&;
  client_context_options& insecure_no_verify_peer(bool enabled = true) & noexcept;
  client_context_options&& insecure_no_verify_peer(bool enabled = true) && noexcept;
  client_context_options& max_pending_write_bytes(std::size_t value) & noexcept;
  client_context_options&& max_pending_write_bytes(std::size_t value) && noexcept;
  client_context_options& max_pending_read_bytes(std::size_t value) & noexcept;
  client_context_options&& max_pending_read_bytes(std::size_t value) && noexcept;

private:
  friend class client_context;

  std::string server_name_;
  std::string ca_file_;
  std::string ca_path_;
  std::vector<std::string> alpn_;
  bool default_verify_paths_ = false;
  bool verify_peer_ = true;
  std::size_t max_pending_write_bytes_ = 1024 * 1024;
  std::size_t max_pending_read_bytes_ = 1024 * 1024;
};

class server_context {
public:
  struct impl;

  explicit server_context(server_context_options options);
  ~server_context();

  server_context(const server_context&) = default;
  server_context& operator=(const server_context&) = default;
  server_context(server_context&&) noexcept = default;
  server_context& operator=(server_context&&) noexcept = default;

private:
  friend struct context_access;
  std::shared_ptr<const impl> impl_;
};

class client_context {
public:
  struct impl;

  explicit client_context(client_context_options options);
  ~client_context();

  client_context(const client_context&) = default;
  client_context& operator=(const client_context&) = default;
  client_context(client_context&&) noexcept = default;
  client_context& operator=(client_context&&) noexcept = default;

private:
  friend struct context_access;
  std::shared_ptr<const impl> impl_;
};

} // namespace uvp::tls
