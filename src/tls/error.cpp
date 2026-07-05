#include <uvpp/protocols/tls/error.hpp>

#include <string>

namespace uvp::tls {

namespace {

class tls_error_category final : public std::error_category {
public:
  const char* name() const noexcept override {
    return "uvpp-protocols.tls";
  }

  std::string message(int value) const override {
    switch (static_cast<errc>(value)) {
    case errc::ok:
      return "success";
    case errc::openssl_error:
      return "OpenSSL error";
    case errc::handshake_failed:
      return "TLS handshake failed";
    case errc::verification_failed:
      return "TLS peer verification failed";
    case errc::protocol_error:
      return "TLS protocol error";
    case errc::closed:
      return "TLS stream is closed";
    }

    return "unknown TLS error";
  }
};

} // namespace

const std::error_category& error_category() noexcept {
  static const tls_error_category category;
  return category;
}

std::error_code make_error_code(errc value) noexcept {
  return {static_cast<int>(value), error_category()};
}

} // namespace uvp::tls
