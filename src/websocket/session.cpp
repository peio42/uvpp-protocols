#include <uvpp/protocols/websocket/session.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "detail/handshake.hpp"

namespace uvp::websocket {

namespace {

constexpr std::size_t read_buffer_compaction_threshold = 4U * 1024U;

enum class opcode : std::uint8_t {
  continuation = 0x0,
  text = 0x1,
  binary = 0x2,
  close = 0x8,
  ping = 0x9,
  pong = 0xA,
};

std::error_code protocol_error() {
  return std::make_error_code(std::errc::protocol_error);
}

std::error_code not_connected_error() {
  return std::make_error_code(std::errc::not_connected);
}

char lower_ascii(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

std::string lowercase(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (char ch : value) {
    result.push_back(lower_ascii(ch));
  }
  return result;
}

std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool token_list_contains(std::string_view header, std::string_view expected) {
  expected = trim(expected);
  while (!header.empty()) {
    const auto comma = header.find(',');
    auto token = trim(header.substr(0, comma));
    if (token.size() == expected.size()) {
      bool equal = true;
      for (std::size_t index = 0; index < token.size(); ++index) {
        if (lower_ascii(token[index]) != lower_ascii(expected[index])) {
          equal = false;
          break;
        }
      }
      if (equal) {
        return true;
      }
    }
    if (comma == std::string_view::npos) {
      break;
    }
    header.remove_prefix(comma + 1);
  }
  return false;
}

bool valid_handshake(const uvp::http::upgrade_request& req) {
  return req.method() == uvp::http::method::get &&
         lowercase(req.header("upgrade")) == "websocket" &&
         token_list_contains(req.header("connection"), "upgrade") &&
         req.header("sec-websocket-version") == "13" &&
         !trim(req.header("sec-websocket-key")).empty();
}

std::string bad_request_response() {
  return "HTTP/1.1 400 Bad Request\r\n"
         "connection: close\r\n"
         "content-length: 12\r\n"
         "\r\n"
         "bad request\n";
}

std::string handshake_response(const uvp::http::upgrade_request& req, std::string_view subprotocol) {
  std::string response;
  response += "HTTP/1.1 101 Switching Protocols\r\n";
  response += "upgrade: websocket\r\n";
  response += "connection: Upgrade\r\n";
  response += "sec-websocket-accept: ";
  response += detail::websocket_accept_value(trim(req.header("sec-websocket-key")));
  response += "\r\n";
  if (!subprotocol.empty()) {
    response += "sec-websocket-protocol: ";
    response += subprotocol;
    response += "\r\n";
  }
  response += "\r\n";
  return response;
}

void append_u16(std::string& output, unsigned short value) {
  output.push_back(static_cast<char>((value >> 8U) & 0xffU));
  output.push_back(static_cast<char>(value & 0xffU));
}

void append_u64(std::string& output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

std::string make_frame(opcode code, std::span<const std::byte> payload) {
  std::string frame;
  frame.reserve(payload.size() + 10U);
  frame.push_back(static_cast<char>(0x80U | static_cast<unsigned char>(code)));
  if (payload.size() <= 125U) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= std::numeric_limits<unsigned short>::max()) {
    frame.push_back(static_cast<char>(126U));
    append_u16(frame, static_cast<unsigned short>(payload.size()));
  } else {
    frame.push_back(static_cast<char>(127U));
    append_u64(frame, static_cast<std::uint64_t>(payload.size()));
  }
  frame.append(reinterpret_cast<const char*>(payload.data()), payload.size());
  return frame;
}

std::string make_close_payload(close_code code, std::string_view reason) {
  std::string payload;
  append_u16(payload, static_cast<unsigned short>(code));
  payload += reason;
  return payload;
}

std::span<const std::byte> bytes_view(const std::string& value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

} // namespace

struct session::state : public std::enable_shared_from_this<state> {
  struct pending_write {
    std::string payload;
    uvp::io::write_callback on_write;
    bool close_after = false;
  };

  explicit state(accept_options options) : options(std::move(options)) {}

  void start(uvp::io::byte_stream accepted_stream, std::span<const std::byte> extra_bytes = {}, bool detached = false) {
    if (closed) {
      accepted_stream.close();
      return;
    }
    if (detached) {
      keep_alive = shared_from_this();
    }
    stream = std::move(accepted_stream);
    if (!extra_bytes.empty()) {
      read_buffer.insert(read_buffer.end(), extra_bytes.begin(), extra_bytes.end());
      parse_frames();
    }
    read_start();
  }

  void read_start() {
    if (closed || !stream) {
      return;
    }
    auto self = weak_from_this();
    stream.read_start([self](uvp::io::read_result result) {
      if (auto state = self.lock()) {
        state->on_read(std::move(result));
      }
    });
  }

  void on_read(uvp::io::read_result result) {
    if (closed) {
      return;
    }
    if (result.eof()) {
      fail(not_connected_error());
      return;
    }
    if (!result) {
      fail(result.error().code(), false);
      return;
    }

    const auto bytes = result.bytes();
    read_buffer.insert(read_buffer.end(), bytes.begin(), bytes.end());
    parse_frames();
  }

  void parse_frames() {
    while (!closed) {
      if (read_buffer.size() - read_offset < 2U) {
        compact_read_buffer();
        return;
      }

      const auto first = static_cast<unsigned char>(read_buffer[read_offset]);
      const auto second = static_cast<unsigned char>(read_buffer[read_offset + 1U]);
      const bool fin = (first & 0x80U) != 0U;
      const bool masked = (second & 0x80U) != 0U;
      const auto raw_opcode = static_cast<unsigned char>(first & 0x0fU);
      std::uint64_t length = second & 0x7fU;
      std::size_t offset = read_offset + 2U;

      if ((first & 0x70U) != 0U || !masked) {
        close_with_error(close_code::protocol_error, protocol_error());
        return;
      }

      if (length == 126U) {
        if (read_buffer.size() < offset + 2U) {
          return;
        }
        length = (static_cast<std::uint64_t>(read_buffer[offset]) << 8U) |
                 static_cast<std::uint64_t>(read_buffer[offset + 1U]);
        offset += 2U;
      } else if (length == 127U) {
        if (read_buffer.size() < offset + 8U) {
          return;
        }
        length = 0U;
        for (std::size_t index = 0; index < 8U; ++index) {
          length = (length << 8U) | static_cast<std::uint64_t>(read_buffer[offset + index]);
        }
        offset += 8U;
      }

      if (length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
          length > static_cast<std::uint64_t>(options.max_message_bytes())) {
        close_with_error(close_code::message_too_large, std::make_error_code(std::errc::message_size));
        return;
      }

      if (read_buffer.size() < offset + 4U + static_cast<std::size_t>(length)) {
        return;
      }

      std::array<std::byte, 4> mask{
        read_buffer[offset],
        read_buffer[offset + 1U],
        read_buffer[offset + 2U],
        read_buffer[offset + 3U],
      };
      offset += 4U;

      std::vector<std::byte> payload(static_cast<std::size_t>(length));
      for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = read_buffer[offset + index] ^ mask[index % 4U];
      }
      read_offset = offset + payload.size();
      compact_read_buffer();

      handle_frame(fin, raw_opcode, payload);
    }
  }

  void compact_read_buffer() {
    if (read_offset == 0U) {
      return;
    }
    if (read_offset == read_buffer.size()) {
      read_buffer.clear();
      read_offset = 0U;
      return;
    }
    if (read_offset < read_buffer_compaction_threshold && read_offset <= read_buffer.size() / 2U) {
      return;
    }

    read_buffer.erase(read_buffer.begin(), read_buffer.begin() + static_cast<std::ptrdiff_t>(read_offset));
    read_offset = 0U;
  }

  void handle_frame(bool fin, unsigned char raw_opcode, std::span<const std::byte> payload) {
    const bool control = (raw_opcode & 0x08U) != 0U;
    if (control && (!fin || payload.size() > 125U)) {
      close_with_error(close_code::protocol_error, protocol_error());
      return;
    }

    switch (static_cast<opcode>(raw_opcode)) {
    case opcode::text:
    case opcode::binary:
      handle_data_frame(fin, static_cast<opcode>(raw_opcode), payload);
      return;
    case opcode::continuation:
      handle_continuation(fin, payload);
      return;
    case opcode::ping:
      dispatch_ping(payload);
      return;
    case opcode::pong:
      dispatch_pong(payload);
      return;
    case opcode::close:
      dispatch_close(payload);
      return;
    default:
      close_with_error(close_code::protocol_error, protocol_error());
      return;
    }
  }

  void handle_data_frame(bool fin, opcode data_opcode, std::span<const std::byte> payload) {
    if (fragmented_opcode) {
      close_with_error(close_code::protocol_error, protocol_error());
      return;
    }
    if (fin) {
      dispatch_message(data_opcode, payload);
      return;
    }

    fragmented_opcode = data_opcode;
    fragmented_message.assign(payload.begin(), payload.end());
  }

  void handle_continuation(bool fin, std::span<const std::byte> payload) {
    if (!fragmented_opcode) {
      close_with_error(close_code::protocol_error, protocol_error());
      return;
    }
    if (fragmented_message.size() + payload.size() > options.max_message_bytes()) {
      close_with_error(close_code::message_too_large, std::make_error_code(std::errc::message_size));
      return;
    }

    fragmented_message.insert(fragmented_message.end(), payload.begin(), payload.end());
    if (!fin) {
      return;
    }

    const auto data_opcode = *fragmented_opcode;
    fragmented_opcode.reset();
    auto message = std::move(fragmented_message);
    fragmented_message.clear();
    dispatch_message(data_opcode, message);
  }

  void dispatch_message(opcode data_opcode, std::span<const std::byte> payload) {
    auto handle = session{shared_from_this()};
    if (byte_mode) {
      if (data_opcode != opcode::binary) {
        close_with_error(close_code::protocol_error, protocol_error());
        return;
      }
      if (byte_read) {
        byte_read(uvp::io::read_result{payload});
      }
      return;
    }

    if (data_opcode == opcode::text) {
      if (on_text) {
        on_text(handle, std::string_view{reinterpret_cast<const char*>(payload.data()), payload.size()});
      }
      return;
    }

    if (on_binary) {
      on_binary(handle, payload);
    }
  }

  void dispatch_ping(std::span<const std::byte> payload) {
    auto handle = session{shared_from_this()};
    if (on_ping) {
      on_ping(handle, payload);
    }
    if (options.auto_pong()) {
      send_frame(opcode::pong, payload);
    }
  }

  void dispatch_pong(std::span<const std::byte> payload) {
    auto handle = session{shared_from_this()};
    if (on_pong) {
      on_pong(handle, payload);
    }
  }

  void dispatch_close(std::span<const std::byte> payload) {
    if (closed) {
      return;
    }

    close_code code = close_code::normal;
    std::string_view reason;
    if (payload.size() == 1U) {
      close_with_error(close_code::protocol_error, protocol_error());
      return;
    }
    if (payload.size() >= 2U) {
      const auto raw_code =
        (static_cast<unsigned short>(payload[0]) << 8U) |
        static_cast<unsigned short>(payload[1]);
      code = static_cast<close_code>(raw_code);
      reason = std::string_view{reinterpret_cast<const char*>(payload.data() + 2U), payload.size() - 2U};
    }

    auto handle = session{shared_from_this()};
    if (on_close) {
      on_close(handle, code, reason);
    }
    if (byte_read) {
      byte_read(uvp::io::read_result{{}, {}, true});
    }

    if (!close_sent) {
      close_sent = true;
      queue(make_frame(opcode::close, payload), true);
    } else {
      close_transport();
    }
  }

  void send_text(std::string_view message) {
    send_frame(opcode::text, std::as_bytes(std::span{message.data(), message.size()}));
  }

  void send_binary(std::span<const std::byte> message, uvp::io::write_callback on_write = {}) {
    send_frame(opcode::binary, message, std::move(on_write));
  }

  bool send_frame(opcode code, std::span<const std::byte> payload, uvp::io::write_callback on_write = {}) {
    if (closed || !stream) {
      if (on_write) {
        on_write(uvp::io::stream_error{not_connected_error()});
      }
      return false;
    }
    if (pending_write_bytes + payload.size() > options.max_pending_write_bytes()) {
      if (on_write) {
        on_write(uvp::io::stream_error{std::make_error_code(std::errc::operation_would_block)});
      }
      close_with_error(close_code::internal_error, std::make_error_code(std::errc::operation_would_block));
      return false;
    }
    queue(make_frame(code, payload), false, std::move(on_write));
    return true;
  }

  void close(close_code code, std::string_view reason) {
    if (closed || close_sent) {
      return;
    }
    close_sent = true;
    const auto payload = make_close_payload(code, reason);
    queue(make_frame(opcode::close, bytes_view(payload)), true);
  }

  void close_with_error(close_code code, std::error_code error) {
    notify_error(error);
    close(code, {});
  }

  void fail(std::error_code error, bool close_socket = true) {
    notify_error(error);
    if (byte_read) {
      byte_read(uvp::io::read_result{{}, uvp::io::stream_error{error}});
    }
    if (close_socket) {
      close_transport();
    }
  }

  void notify_error(std::error_code error) {
    auto handle = session{shared_from_this()};
    if (on_error) {
      on_error(handle, error);
    }
  }

  void queue(std::string payload, bool close_after, uvp::io::write_callback on_write = {}) {
    pending_write_bytes += payload.size();
    writes.push_back(pending_write{std::move(payload), std::move(on_write), close_after});
    flush_next();
  }

  void flush_next() {
    if (closed || writing || writes.empty() || !stream) {
      return;
    }
    writing = true;
    auto self = weak_from_this();
    stream.write(bytes_view(writes.front().payload), [self](uvp::io::stream_error error) {
      if (auto state = self.lock()) {
        state->on_write(std::move(error));
      }
    });
  }

  void on_write(uvp::io::stream_error error) {
    writing = false;
    if (closed) {
      return;
    }
    if (error) {
      if (!writes.empty() && writes.front().on_write) {
        writes.front().on_write(error);
      }
      fail(error.code());
      return;
    }

    const bool close_after = !writes.empty() && writes.front().close_after;
    auto on_write = !writes.empty() ? std::move(writes.front().on_write) : uvp::io::write_callback{};
    if (!writes.empty()) {
      pending_write_bytes -= std::min(pending_write_bytes, writes.front().payload.size());
      writes.pop_front();
    }
    if (on_write) {
      on_write({});
    }

    if (close_after) {
      close_transport();
      return;
    }

    flush_next();
  }

  void close_transport(uvp::io::close_callback on_close = {}) {
    auto self = shared_from_this();
    if (closed) {
      if (on_close) {
        on_close();
      }
      return;
    }
    closed = true;
    writes.clear();
    pending_write_bytes = 0;
    try {
      if (stream) {
        stream.read_stop();
      }
    } catch (...) {
    }
    if (stream) {
      stream.close([self, callback = std::move(on_close)]() mutable {
        if (callback) {
          callback();
        }
        self->keep_alive.reset();
      });
    } else if (on_close) {
      on_close();
      keep_alive.reset();
    } else {
      keep_alive.reset();
    }
  }

  uv::loop& loop() noexcept {
    return stream.loop();
  }

  uvp::io::endpoint local_endpoint() const {
    return stream ? stream.local_endpoint() : uvp::io::endpoint{};
  }

  uvp::io::endpoint remote_endpoint() const {
    return stream ? stream.remote_endpoint() : uvp::io::endpoint{};
  }

  accept_options options;
  session::text_callback on_text;
  session::binary_callback on_binary;
  session::control_callback on_ping;
  session::control_callback on_pong;
  session::close_callback on_close;
  session::error_callback on_error;
  uvp::io::byte_stream stream;
  std::vector<std::byte> read_buffer;
  std::size_t read_offset = 0;
  std::deque<pending_write> writes;
  std::size_t pending_write_bytes = 0;
  bool writing = false;
  bool closed = false;
  bool close_sent = false;

  std::optional<opcode> fragmented_opcode;
  std::vector<std::byte> fragmented_message;

  bool byte_mode = false;
  uvp::io::read_callback byte_read;
  std::shared_ptr<state> keep_alive;
};

class websocket_byte_stream final : public uvp::io::byte_stream::concept_ {
public:
  explicit websocket_byte_stream(std::shared_ptr<session::state> state)
      : state_(std::move(state)) {}

  uv::loop& loop() noexcept override {
    return state_->loop();
  }

  void read_start(uvp::io::read_callback on_read) override {
    state_->byte_read = std::move(on_read);
  }

  void read_stop() override {
    state_->byte_read = {};
  }

  void write(std::span<const std::byte> bytes, uvp::io::write_callback on_write) override {
    if (!state_ || state_->closed) {
      if (on_write) {
        on_write(uvp::io::stream_error{not_connected_error()});
      }
      return;
    }
    state_->send_binary(bytes, std::move(on_write));
  }

  void close(uvp::io::close_callback on_close) override {
    if (state_) {
      state_->close_transport(std::move(on_close));
    } else if (on_close) {
      on_close();
    }
  }

  uvp::io::endpoint local_endpoint() const override {
    return state_ ? state_->local_endpoint() : uvp::io::endpoint{};
  }

  uvp::io::endpoint remote_endpoint() const override {
    return state_ ? state_->remote_endpoint() : uvp::io::endpoint{};
  }

  uv::tcp* tcp() noexcept override {
    return nullptr;
  }

  uv::pipe* pipe() noexcept override {
    return nullptr;
  }

private:
  std::shared_ptr<session::state> state_;
};

accept_options& accept_options::max_message_bytes(std::size_t value) & {
  max_message_bytes_ = value;
  return *this;
}

accept_options&& accept_options::max_message_bytes(std::size_t value) && {
  max_message_bytes(value);
  return std::move(*this);
}

accept_options& accept_options::max_pending_write_bytes(std::size_t value) & {
  max_pending_write_bytes_ = value;
  return *this;
}

accept_options&& accept_options::max_pending_write_bytes(std::size_t value) && {
  max_pending_write_bytes(value);
  return std::move(*this);
}

accept_options& accept_options::subprotocol(std::string_view value) & {
  subprotocol_ = value;
  return *this;
}

accept_options&& accept_options::subprotocol(std::string_view value) && {
  subprotocol(value);
  return std::move(*this);
}

accept_options& accept_options::auto_pong(bool value) & noexcept {
  auto_pong_ = value;
  return *this;
}

accept_options&& accept_options::auto_pong(bool value) && noexcept {
  auto_pong(value);
  return std::move(*this);
}

session::session(std::shared_ptr<state> state, bool owns_lifetime)
    : state_(std::move(state)), owns_lifetime_(owns_lifetime) {}

session::~session() {
  release_owned();
}

session::session(session&& other) noexcept
    : state_(std::move(other.state_)),
      owns_lifetime_(std::exchange(other.owns_lifetime_, false)) {}

session& session::operator=(session&& other) noexcept {
  if (this != &other) {
    release_owned();
    state_ = std::move(other.state_);
    owns_lifetime_ = std::exchange(other.owns_lifetime_, false);
  }
  return *this;
}

void session::release_owned() noexcept {
  if (!owns_lifetime_ || !state_) {
    return;
  }
  owns_lifetime_ = false;
  try {
    state_->close_transport();
  } catch (...) {
  }
}

void session::text(std::string_view message) {
  if (state_) {
    state_->send_text(message);
  }
}

void session::binary(std::span<const std::byte> message) {
  if (state_) {
    state_->send_binary(message);
  }
}

void session::ping(std::span<const std::byte> payload) {
  if (state_) {
    state_->send_frame(opcode::ping, payload);
  }
}

void session::pong(std::span<const std::byte> payload) {
  if (state_) {
    state_->send_frame(opcode::pong, payload);
  }
}

void session::close(close_code code, std::string_view reason) {
  if (state_) {
    state_->close(code, reason);
  }
}

session& session::on_text(text_callback callback) & {
  if (state_) {
    state_->on_text = std::move(callback);
  }
  return *this;
}

session&& session::on_text(text_callback callback) && {
  on_text(std::move(callback));
  return std::move(*this);
}

session& session::on_binary(binary_callback callback) & {
  if (state_) {
    state_->on_binary = std::move(callback);
  }
  return *this;
}

session&& session::on_binary(binary_callback callback) && {
  on_binary(std::move(callback));
  return std::move(*this);
}

session& session::on_ping(control_callback callback) & {
  if (state_) {
    state_->on_ping = std::move(callback);
  }
  return *this;
}

session&& session::on_ping(control_callback callback) && {
  on_ping(std::move(callback));
  return std::move(*this);
}

session& session::on_pong(control_callback callback) & {
  if (state_) {
    state_->on_pong = std::move(callback);
  }
  return *this;
}

session&& session::on_pong(control_callback callback) && {
  on_pong(std::move(callback));
  return std::move(*this);
}

session& session::on_close(close_callback callback) & {
  if (state_) {
    state_->on_close = std::move(callback);
  }
  return *this;
}

session&& session::on_close(close_callback callback) && {
  on_close(std::move(callback));
  return std::move(*this);
}

session& session::on_error(error_callback callback) & {
  if (state_) {
    state_->on_error = std::move(callback);
  }
  return *this;
}

session&& session::on_error(error_callback callback) && {
  on_error(std::move(callback));
  return std::move(*this);
}

uvp::io::endpoint session::local_endpoint() const {
  return state_ ? state_->local_endpoint() : uvp::io::endpoint{};
}

uvp::io::endpoint session::remote_endpoint() const {
  return state_ ? state_->remote_endpoint() : uvp::io::endpoint{};
}

uvp::io::byte_stream session::into_byte_stream() && {
  if (!state_) {
    return {};
  }
  owns_lifetime_ = false;
  state_->byte_mode = true;
  return uvp::io::byte_stream{std::make_unique<websocket_byte_stream>(std::move(state_))};
}

session::operator bool() const noexcept {
  return static_cast<bool>(state_) && !state_->closed;
}

session accept(uvp::http::upgrade_request& req, accept_options options) {
  if (!valid_handshake(req)) {
    req.reject(bad_request_response());
    return {};
  }

  auto state = std::make_shared<session::state>(std::move(options));
  auto handle = session{state, true};
  const auto response = handshake_response(req, state->options.subprotocol());
  std::vector<std::byte> extra_bytes(req.extra_bytes().begin(), req.extra_bytes().end());
  req.accept(response, [state, extra_bytes = std::move(extra_bytes)](uvp::io::byte_stream stream) {
    state->start(std::move(stream), extra_bytes);
  });
  return handle;
}

session accept_detached(uvp::http::upgrade_request& req, accept_options options) {
  if (!valid_handshake(req)) {
    req.reject(bad_request_response());
    return {};
  }

  auto state = std::make_shared<session::state>(std::move(options));
  auto handle = session{state};
  const auto response = handshake_response(req, state->options.subprotocol());
  std::vector<std::byte> extra_bytes(req.extra_bytes().begin(), req.extra_bytes().end());
  req.accept(response, [state, extra_bytes = std::move(extra_bytes)](uvp::io::byte_stream stream) {
    state->start(std::move(stream), extra_bytes, true);
  });
  return handle;
}

uvp::io::byte_stream accept_byte_stream(uvp::http::upgrade_request& req, accept_options options) {
  auto websocket = accept(req, std::move(options));
  return std::move(websocket).into_byte_stream();
}

} // namespace uvp::websocket
