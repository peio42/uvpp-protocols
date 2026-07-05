#pragma once

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
};

struct client_context::impl {
  impl();
  ~impl();

  SSL_CTX* ctx = nullptr;
  std::vector<unsigned char> alpn;
  std::string server_name;
  bool verify_peer = true;
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
};

} // namespace uvp::tls
