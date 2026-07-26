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

std::vector<unsigned char> encode_alpn(const std::vector<std::string>& protocols) {
  std::vector<unsigned char> encoded;
  for (const auto& protocol : protocols) {
    if (protocol.empty() || protocol.size() > std::numeric_limits<unsigned char>::max()) {
      throw std::invalid_argument("TLS ALPN protocol names must be 1..255 bytes");
    }

    encoded.push_back(static_cast<unsigned char>(protocol.size()));
    encoded.insert(encoded.end(), protocol.begin(), protocol.end());
  }
  return encoded;
}

int select_alpn(SSL*,
                const unsigned char** out,
                unsigned char* outlen,
                const unsigned char* in,
                unsigned int inlen,
                void* arg) {
  const auto* context = static_cast<const server_context::impl*>(arg);
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

server_context_options& server_context_options::certificate_chain_file(std::string_view path) & {
  certificate_chain_file_ = std::string(path);
  return *this;
}

server_context_options&& server_context_options::certificate_chain_file(std::string_view path) && {
  certificate_chain_file(path);
  return std::move(*this);
}

server_context_options& server_context_options::private_key_file(std::string_view path) & {
  private_key_file_ = std::string(path);
  return *this;
}

server_context_options&& server_context_options::private_key_file(std::string_view path) && {
  private_key_file(path);
  return std::move(*this);
}

server_context_options& server_context_options::alpn(std::initializer_list<std::string_view> protocols) & {
  alpn_.clear();
  alpn_.reserve(protocols.size());
  for (const auto protocol : protocols) {
    alpn_.emplace_back(protocol);
  }
  return *this;
}

server_context_options&& server_context_options::alpn(std::initializer_list<std::string_view> protocols) && {
  alpn(protocols);
  return std::move(*this);
}

server_context_options& server_context_options::require_alpn(bool enabled) & noexcept {
  require_alpn_ = enabled;
  return *this;
}

server_context_options&& server_context_options::require_alpn(bool enabled) && noexcept {
  require_alpn(enabled);
  return std::move(*this);
}

server_context_options& server_context_options::max_pending_write_bytes(std::size_t value) & noexcept {
  max_pending_write_bytes_ = value;
  return *this;
}

server_context_options&& server_context_options::max_pending_write_bytes(std::size_t value) && noexcept {
  max_pending_write_bytes(value);
  return std::move(*this);
}

server_context_options& server_context_options::max_pending_read_bytes(std::size_t value) & noexcept {
  max_pending_read_bytes_ = value;
  return *this;
}

server_context_options&& server_context_options::max_pending_read_bytes(std::size_t value) && noexcept {
  max_pending_read_bytes(value);
  return std::move(*this);
}

client_context_options& client_context_options::server_name(std::string_view name) & {
  server_name_ = std::string(name);
  return *this;
}

client_context_options&& client_context_options::server_name(std::string_view name) && {
  server_name(name);
  return std::move(*this);
}

client_context_options& client_context_options::default_verify_paths(bool enabled) & noexcept {
  default_verify_paths_ = enabled;
  return *this;
}

client_context_options&& client_context_options::default_verify_paths(bool enabled) && noexcept {
  default_verify_paths(enabled);
  return std::move(*this);
}

client_context_options& client_context_options::ca_file(std::string_view path) & {
  ca_file_ = std::string(path);
  return *this;
}

client_context_options&& client_context_options::ca_file(std::string_view path) && {
  ca_file(path);
  return std::move(*this);
}

client_context_options& client_context_options::ca_path(std::string_view path) & {
  ca_path_ = std::string(path);
  return *this;
}

client_context_options&& client_context_options::ca_path(std::string_view path) && {
  ca_path(path);
  return std::move(*this);
}

client_context_options& client_context_options::alpn(std::initializer_list<std::string_view> protocols) & {
  alpn_.clear();
  alpn_.reserve(protocols.size());
  for (const auto protocol : protocols) {
    alpn_.emplace_back(protocol);
  }
  return *this;
}

client_context_options&& client_context_options::alpn(std::initializer_list<std::string_view> protocols) && {
  alpn(protocols);
  return std::move(*this);
}

client_context_options& client_context_options::insecure_no_verify_peer(bool enabled) & noexcept {
  verify_peer_ = !enabled;
  return *this;
}

client_context_options&& client_context_options::insecure_no_verify_peer(bool enabled) && noexcept {
  insecure_no_verify_peer(enabled);
  return std::move(*this);
}

client_context_options& client_context_options::max_pending_write_bytes(std::size_t value) & noexcept {
  max_pending_write_bytes_ = value;
  return *this;
}

client_context_options&& client_context_options::max_pending_write_bytes(std::size_t value) && noexcept {
  max_pending_write_bytes(value);
  return std::move(*this);
}

client_context_options& client_context_options::max_pending_read_bytes(std::size_t value) & noexcept {
  max_pending_read_bytes_ = value;
  return *this;
}

client_context_options&& client_context_options::max_pending_read_bytes(std::size_t value) && noexcept {
  max_pending_read_bytes(value);
  return std::move(*this);
}

server_context::impl::impl(const server_context_options& options) : ctx(SSL_CTX_new(TLS_server_method())) {
  if (!ctx) {
    throw std::runtime_error(openssl_error_detail("failed to create TLS server context"));
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  alpn = encode_alpn(options.alpn_);
  require_alpn = options.require_alpn_;
  max_pending_write_bytes = options.max_pending_write_bytes_;
  max_pending_read_bytes = options.max_pending_read_bytes_;

  if (options.certificate_chain_file_.empty()) {
    throw std::invalid_argument("TLS server certificate chain file is required");
  }
  if (options.private_key_file_.empty()) {
    throw std::invalid_argument("TLS server private key file is required");
  }
  if (SSL_CTX_use_certificate_chain_file(ctx, options.certificate_chain_file_.c_str()) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS certificate chain"));
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, options.private_key_file_.c_str(), SSL_FILETYPE_PEM) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS private key"));
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    throw std::runtime_error(openssl_error_detail("TLS private key does not match certificate"));
  }

  SSL_CTX_set_alpn_select_cb(ctx, select_alpn, const_cast<impl*>(this));
}

server_context::impl::~impl() {
  SSL_CTX_free(ctx);
}

client_context::impl::impl(const client_context_options& options) : ctx(SSL_CTX_new(TLS_client_method())) {
  if (!ctx) {
    throw std::runtime_error(openssl_error_detail("failed to create TLS client context"));
  }

  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  alpn = encode_alpn(options.alpn_);
  server_name = options.server_name_;
  verify_peer = options.verify_peer_;
  max_pending_write_bytes = options.max_pending_write_bytes_;
  max_pending_read_bytes = options.max_pending_read_bytes_;

  SSL_CTX_set_verify(ctx, verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
  if (options.default_verify_paths_ && SSL_CTX_set_default_verify_paths(ctx) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load default TLS verify paths"));
  }
  if (!options.ca_file_.empty() && SSL_CTX_load_verify_locations(ctx, options.ca_file_.c_str(), nullptr) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS CA file"));
  }
  if (!options.ca_path_.empty() && SSL_CTX_load_verify_locations(ctx, nullptr, options.ca_path_.c_str()) != 1) {
    throw std::runtime_error(openssl_error_detail("failed to load TLS CA path"));
  }
}

client_context::impl::~impl() {
  SSL_CTX_free(ctx);
}

server_context::server_context(server_context_options options)
    : impl_(std::make_shared<impl>(options)) {}

server_context::~server_context() = default;

client_context::client_context(client_context_options options)
    : impl_(std::make_shared<impl>(options)) {}

client_context::~client_context() = default;

} // namespace uvp::tls
