#include <uvpp/protocols/tls/context.hpp>

#include "context_internal.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <openssl/err.h>

namespace uvp::tls {

namespace {

std::string openssl_error_detail(std::string_view prefix) {
  std::string detail(prefix);
  auto code = ERR_get_error();
  if (code == 0) {
    return detail;
  }

  detail += ": ";
  detail += ERR_error_string(code, nullptr);

  while (ERR_get_error() != 0) {
  }

  return detail;
}

std::vector<unsigned char> encode_alpn(std::initializer_list<std::string_view> protocols) {
  std::vector<unsigned char> encoded;
  for (auto protocol : protocols) {
    if (protocol.empty() || protocol.size() > std::numeric_limits<unsigned char>::max()) {
      throw std::invalid_argument("TLS ALPN protocol names must be 1..255 bytes");
    }

    encoded.push_back(static_cast<unsigned char>(protocol.size()));
    encoded.insert(encoded.end(), protocol.begin(), protocol.end());
  }
  return encoded;
}

int select_alpn(SSL* ssl,
                const unsigned char** out,
                unsigned char* outlen,
                const unsigned char* in,
                unsigned int inlen,
                void* arg) {
  auto* context = static_cast<server_context::impl*>(arg);
  if (!context || context->alpn.empty()) {
    return SSL_TLSEXT_ERR_NOACK;
  }

  for (const unsigned char* server = context->alpn.data();
       server < context->alpn.data() + context->alpn.size();) {
    const auto server_len = *server++;
    const auto* server_name = server;
    server += server_len;

    for (const unsigned char* client = in; client < in + inlen;) {
      const auto client_len = *client++;
      const auto* client_name = client;
      client += client_len;

      if (server_len == client_len && std::equal(server_name, server_name + server_len, client_name)) {
        *out = server_name;
        *outlen = server_len;
        return SSL_TLSEXT_ERR_OK;
      }
    }
  }

  return context->require_alpn ? SSL_TLSEXT_ERR_ALERT_FATAL : SSL_TLSEXT_ERR_NOACK;
}

} // namespace

server_context::impl::impl() : ctx(SSL_CTX_new(TLS_server_method())) {
  if (!ctx) {
    throw std::runtime_error(openssl_error_detail("failed to create TLS server context"));
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_alpn_select_cb(ctx, select_alpn, this);
}

server_context::impl::~impl() {
  SSL_CTX_free(ctx);
}

client_context::impl::impl() : ctx(SSL_CTX_new(TLS_client_method())) {
  if (!ctx) {
    throw std::runtime_error(openssl_error_detail("failed to create TLS client context"));
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
}

client_context::impl::~impl() {
  SSL_CTX_free(ctx);
}

server_context::server_context()
    : impl_(std::make_shared<impl>()) {}

server_context::~server_context() = default;

server_context& server_context::certificate_chain_file(std::string_view path) {
  const auto storage = std::string(path);
  if (SSL_CTX_use_certificate_chain_file(impl_->ctx, storage.c_str()) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS certificate chain"));
  }
  return *this;
}

server_context& server_context::private_key_file(std::string_view path) {
  const auto storage = std::string(path);
  if (SSL_CTX_use_PrivateKey_file(impl_->ctx, storage.c_str(), SSL_FILETYPE_PEM) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS private key"));
  }
  if (SSL_CTX_check_private_key(impl_->ctx) != 1) {
    throw std::runtime_error(openssl_error_detail("TLS private key does not match certificate"));
  }
  return *this;
}

server_context& server_context::alpn(std::initializer_list<std::string_view> protocols) {
  impl_->alpn = encode_alpn(protocols);
  return *this;
}

server_context& server_context::require_alpn(bool enabled) noexcept {
  impl_->require_alpn = enabled;
  return *this;
}

client_context::client_context()
    : impl_(std::make_shared<impl>()) {}

client_context::~client_context() = default;

client_context& client_context::server_name(std::string_view name) {
  impl_->server_name = std::string(name);
  return *this;
}

client_context& client_context::default_verify_paths() {
  if (SSL_CTX_set_default_verify_paths(impl_->ctx) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load default TLS verify paths"));
  }
  return *this;
}

client_context& client_context::ca_file(std::string_view path) {
  const auto storage = std::string(path);
  if (SSL_CTX_load_verify_locations(impl_->ctx, storage.c_str(), nullptr) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS CA file"));
  }
  return *this;
}

client_context& client_context::ca_path(std::string_view path) {
  const auto storage = std::string(path);
  if (SSL_CTX_load_verify_locations(impl_->ctx, nullptr, storage.c_str()) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS CA path"));
  }
  return *this;
}

client_context& client_context::alpn(std::initializer_list<std::string_view> protocols) {
  impl_->alpn = encode_alpn(protocols);
  return *this;
}

client_context& client_context::insecure_no_verify_peer() noexcept {
  impl_->verify_peer = false;
  SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_NONE, nullptr);
  return *this;
}

} // namespace uvp::tls
