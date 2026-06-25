#include "handshake.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace uvp::websocket::detail {

namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t sha1_digest_size = 20;

std::array<std::byte, sha1_digest_size> sha1(std::string_view message) {
  std::array<std::byte, sha1_digest_size> digest{};
  unsigned int digest_size = 0;

  const auto ok = EVP_Digest(
    message.data(),
    message.size(),
    reinterpret_cast<unsigned char*>(digest.data()),
    &digest_size,
    EVP_sha1(),
    nullptr);

  if (ok != 1 || digest_size != digest.size()) {
    throw std::runtime_error("OpenSSL SHA-1 digest failed");
  }

  return digest;
}

std::string base64(std::string_view bytes) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((bytes.size() + 2U) / 3U) * 4U);

  for (std::size_t index = 0; index < bytes.size(); index += 3U) {
    const auto value_at = [&](std::size_t offset) {
      return static_cast<unsigned int>(static_cast<unsigned char>(bytes[offset]));
    };
    const auto b0 = value_at(index);
    const auto b1 = index + 1U < bytes.size() ? value_at(index + 1U) : 0U;
    const auto b2 = index + 2U < bytes.size() ? value_at(index + 2U) : 0U;
    const auto value = (b0 << 16U) | (b1 << 8U) | b2;

    output.push_back(alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(alphabet[(value >> 12U) & 0x3fU]);
    output.push_back(index + 1U < bytes.size() ? alphabet[(value >> 6U) & 0x3fU] : '=');
    output.push_back(index + 2U < bytes.size() ? alphabet[value & 0x3fU] : '=');
  }

  return output;
}

std::string base64(std::array<std::byte, sha1_digest_size> digest) {
  return base64(std::string_view{
    reinterpret_cast<const char*>(digest.data()),
    digest.size(),
  });
}

} // namespace

std::string websocket_accept_value(std::string_view key) {
  auto input = std::string(key);
  input += websocket_guid;
  return base64(sha1(input));
}

} // namespace uvp::websocket::detail
