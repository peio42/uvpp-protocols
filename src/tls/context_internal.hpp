#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <openssl/ssl.h>

#include <uvpp/protocols/tls/context.hpp>

namespace uvp::tls {

struct server_context::impl {
  impl();
  ~impl();

  SSL_CTX* ctx = nullptr;
  std::vector<unsigned char> alpn;
  bool require_alpn = false;
  std::size_t max_pending_write_bytes = 1024 * 1024;
  std::size_t max_pending_read_bytes = 1024 * 1024;
};

struct client_context::impl {
  impl();
  ~impl();

  SSL_CTX* ctx = nullptr;
  std::vector<unsigned char> alpn;
  std::string server_name;
  bool verify_peer = true;
  std::size_t max_pending_write_bytes = 1024 * 1024;
  std::size_t max_pending_read_bytes = 1024 * 1024;
};

struct context_access {
  static SSL_CTX* native(const server_context& context) noexcept {
    return context.impl_->ctx;
  }

  static SSL_CTX* native(const client_context& context) noexcept {
    return context.impl_->ctx;
  }

  static const std::vector<unsigned char>& alpn(const client_context& context) noexcept {
    return context.impl_->alpn;
  }

  static const std::string& server_name(const client_context& context) noexcept {
    return context.impl_->server_name;
  }

  static bool verify_peer(const client_context& context) noexcept {
    return context.impl_->verify_peer;
  }

  static std::size_t max_pending_write_bytes(const server_context& context) noexcept {
    return context.impl_->max_pending_write_bytes;
  }

  static std::size_t max_pending_write_bytes(const client_context& context) noexcept {
    return context.impl_->max_pending_write_bytes;
  }

  static std::size_t max_pending_read_bytes(const server_context& context) noexcept {
    return context.impl_->max_pending_read_bytes;
  }

  static std::size_t max_pending_read_bytes(const client_context& context) noexcept {
    return context.impl_->max_pending_read_bytes;
  }
};

} // namespace uvp::tls
