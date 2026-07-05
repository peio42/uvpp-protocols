#include <uvpp/protocols/http/server.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <uvpp/handles/timer.hpp>
#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/io/tcp_listener.hpp>

#include "detail/http1_state_machine.hpp"
#include "detail/route_matching.hpp"

namespace uvp::http {

namespace {

std::pair<std::string, std::string> split_path_query(std::string_view target) {
  const auto query_offset = target.find('?');
  if (query_offset == std::string_view::npos) {
    return {std::string(target), {}};
  }

  return {
    std::string(target.substr(0, query_offset)),
    std::string(target.substr(query_offset + 1)),
  };
}

std::vector<std::byte> body_bytes(std::string_view body) {
  std::vector<std::byte> bytes(body.size());
  if (!body.empty()) {
    std::memcpy(bytes.data(), body.data(), body.size());
  }
  return bytes;
}

bool contains_method(std::span<const method> methods, method value) noexcept {
  return std::find(methods.begin(), methods.end(), value) != methods.end();
}

std::string allow_header_value(std::vector<method> methods) {
  if (contains_method(methods, method::get) && !contains_method(methods, method::head)) {
    methods.push_back(method::head);
  }
  if (!contains_method(methods, method::options)) {
    methods.push_back(method::options);
  }
  std::sort(methods.begin(), methods.end(), [](method lhs, method rhs) {
    return static_cast<unsigned int>(lhs) < static_cast<unsigned int>(rhs);
  });

  std::string value;
  for (auto method_value : methods) {
    if (!value.empty()) {
      value += ", ";
    }
    value += to_string(method_value);
  }
  return value;
}

std::string_view reason_phrase_for(unsigned int status_code) noexcept {
  return reason_phrase(static_cast<status>(status_code));
}

bool header_name_equals(std::string_view name, std::string_view expected) noexcept {
  return headers::names_equal(name, expected);
}

std::string serialize_response_head(
  const response& response,
  bool keep_alive,
  const server_options& options,
  bool chunked,
  bool suppress_body = false) {
  const auto status_code = response.status_code();

  std::ostringstream out;
  out << "HTTP/1.1 " << status_code << ' ' << reason_phrase_for(status_code) << "\r\n";

  bool has_content_length = false;
  bool has_connection = false;
  bool has_server = false;

  for (const auto& [name, value] : response.headers()) {
    if (header_name_equals(name, "content-length")) {
      has_content_length = true;
    }
    if (header_name_equals(name, "connection")) {
      has_connection = true;
    }
    if (header_name_equals(name, "server")) {
      has_server = true;
    }
  }

  for (const auto& [name, value] : response.headers()) {
    if (header_name_equals(name, "content-length") && chunked) {
      continue;
    }
    if (header_name_equals(name, "transfer-encoding")) {
      continue;
    }
    out << name << ": " << value << "\r\n";
  }

  if (chunked && !suppress_body) {
    out << "transfer-encoding: chunked\r\n";
  } else if (!has_content_length) {
    auto body = std::string(response.body());
    if (status_code == static_cast<unsigned int>(status::no_content)) {
      body.clear();
    }
    out << "content-length: " << body.size() << "\r\n";
  }
  if (!has_connection) {
    out << "connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
  }
  if (options.server_header() && !has_server) {
    out << "server: uvpp-protocols\r\n";
  }

  out << "\r\n";
  return out.str();
}

std::string serialize_response(
  const response& response,
  bool keep_alive,
  const server_options& options,
  bool suppress_body = false) {
  const auto status_code = response.status_code();
  auto body = std::string(response.body());
  if (status_code == static_cast<unsigned int>(status::no_content)) {
    body.clear();
  }

  auto out = serialize_response_head(response, keep_alive, options, false, suppress_body);
  if (!suppress_body) {
    out += body;
  }
  return out;
}

std::string serialize_chunk(std::string payload) {
  std::ostringstream out;
  out << std::hex << payload.size() << "\r\n";
  out << payload;
  out << "\r\n";
  return out.str();
}

std::span<const std::byte> as_bytes(std::string& value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

bool is_streaming_body_mode(detail::body_mode mode) noexcept {
  return mode == detail::body_mode::stream || mode == detail::body_mode::multipart_stream;
}

} // namespace

struct server::impl {
  explicit impl(server& owner) : owner(owner) {}

  struct pending_write {
    std::string payload;
    bool close_after = false;
    bool counts_response = false;
    std::function<void(uvp::io::byte_stream)> upgrade;
  };

  struct response_slot {
    response res;
    request_snapshot request;
    std::vector<response_hook_handle> on_response_hooks;
    bool completed = false;
    bool close_after = false;
    bool suppress_body = false;
    bool streaming = false;
    bool stream_headers_queued = false;
    bool stream_chunked = true;
    bool stream_backpressured = false;
    bool stream_ended = false;
    bool response_hooks_ran = false;
    std::size_t stream_body_bytes = 0;
    std::deque<pending_write> stream_writes;
  };

  class session : public std::enable_shared_from_this<session> {
  public:
    session(impl& owner, uvp::io::byte_stream stream)
        : owner_(owner),
          stream_(std::move(stream)),
          timeout_timer_(owner.owner.loop()) {
      parser_.limits(detail::http1_limits{
        owner_.owner.options_.max_header_bytes(),
        owner_.owner.options_.max_header_count(),
      });
    }

    void start() {
      start_timeout(timeout_phase::header, owner_.owner.options_.header_timeout());
      start_read();
    }

    void start_read() {
      if (closed_) {
        return;
      }
      auto self = weak_from_this();
      stream_.read_start([self](uvp::io::read_result result) {
        if (auto session = self.lock()) {
          session->on_read(std::move(result));
        }
      });
    }

    void close() {
      if (closed_) {
        return;
      }
      closed_ = true;
      close_timeout_timer();
      cancel_pending_responses();
      try {
        stream_.read_stop();
      } catch (...) {
      }
      auto self = weak_from_this();
      stream_.close([self] {
        if (auto session = self.lock()) {
          session->owner_.remove_session(session.get());
        }
      });
    }

  private:
    enum class timeout_phase {
      none,
      header,
      body,
      idle,
    };

    void on_read(uvp::io::read_result result) {
      if (closed_ || timeout_closing_) {
        return;
      }

      if (result.eof()) {
        notify_stream_errors(std::make_error_code(std::errc::connection_reset));
        close();
        return;
      }

      if (!result) {
        notify_stream_errors(result.error().code());
        close();
        return;
      }

      if (timeout_phase_ == timeout_phase::idle) {
        stop_timeout();
        start_timeout(timeout_phase::header, owner_.owner.options_.header_timeout());
      } else if (timeout_phase_ == timeout_phase::none && !active_request_) {
        start_timeout(timeout_phase::header, owner_.owner.options_.header_timeout());
      }

      const auto bytes = result.bytes();
      auto parse_result = parser_.parse(std::string_view{
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size(),
      });

      if (parse_result.code == detail::http1_parse_result::status::error) {
        stop_timeout();
        send_error(status::bad_request, false);
        return;
      }

      if (parse_result.code == detail::http1_parse_result::status::upgrade) {
        process_parser_events(true);
        const auto parsed_bytes = std::min(parse_result.parsed_bytes, bytes.size());
        handle_upgrade(bytes.subspan(parsed_bytes));
        return;
      }

      process_parser_events();
    }

    struct active_request_state {
      detail::http1_message headers;
      router::match_result route;
      detail::route_path parsed_path;
      request req;
      request_body_stream body_stream;
      std::shared_ptr<response_slot> slot;
      std::size_t received_body_bytes = 0;
      bool rejected = false;
    };

    bool request_has_body(const detail::http1_message& message) const {
      const auto content_length = message.headers.get("content-length");
      if (!content_length.empty() && content_length != "0") {
        return true;
      }
      return !message.headers.get("transfer-encoding").empty();
    }

    std::size_t route_body_limit(const router::match_result& route) const noexcept {
      return route.max_body_bytes == 0 ? owner_.owner.options_.max_body_bytes() : route.max_body_bytes;
    }

    std::chrono::milliseconds route_body_timeout(const router::match_result& route) const noexcept {
      return route.body_timeout.count() == 0 ? owner_.owner.options_.body_timeout() : route.body_timeout;
    }

    connection_info connection() const {
      return connection_info{stream_.local_endpoint(), stream_.remote_endpoint()};
    }

    router::match_result match_request_route(method method_value, const detail::route_path& path) const {
      auto route_match = owner_.owner.router_.match(method_value, path);
      if (!route_match && method_value == method::head) {
        route_match = owner_.owner.router_.match(method::get, path);
      }
      return route_match;
    }

    void dispatch_automatic_method_response(
      request& req,
      response& res,
      std::vector<method> allowed_methods) {
      const auto allow = allow_header_value(std::move(allowed_methods));
      if (req.method() == method::options) {
        res.status(status::no_content).header("allow", allow).end();
        return;
      }

      res.status(status::method_not_allowed)
        .header("allow", allow)
        .text("method not allowed\n");
    }

    void process_parser_events(bool stop_before_complete = false) {
      const auto& events = parser_.events();
      while (!closed_ && !body_processing_paused_ && handled_events_ < events.size()) {
        if (stop_before_complete && events[handled_events_].event_type() == detail::http1_event::type::complete) {
          return;
        }
        handle_event(events[handled_events_++]);
      }
    }

    void handle_event(const detail::http1_event& event) {
      switch (event.event_type()) {
      case detail::http1_event::type::headers:
        handle_headers(event.message());
        break;
      case detail::http1_event::type::body:
        handle_body_chunk(event.body());
        break;
      case detail::http1_event::type::complete:
        handle_message_complete(event.message());
        break;
      }
    }

    void handle_headers(const detail::http1_message& message) {
      stop_timeout();
      const auto [path, query] = split_path_query(message.target);
      const auto parsed_path = detail::parse_route_path(path);
      if (!parsed_path.valid) {
        active_request_ = std::make_unique<active_request_state>();
        active_request_->headers = message;
        active_request_->rejected = true;
        send_error(status::bad_request, false);
        return;
      }
      auto req = request{
        message.method,
        message.target,
        path,
        query,
        message.headers,
        {},
        route_params{},
        connection(),
        parsed_path.decoded_segments,
      };
      auto route_match = match_request_route(req.method(), parsed_path);
      if (route_match) {
        req.params_ = route_match.params;
        req.matched_pattern_ = route_match.pattern;
      }

      active_request_ = std::make_unique<active_request_state>();
      active_request_->headers = message;
      active_request_->route = std::move(route_match);
      active_request_->parsed_path = parsed_path;
      active_request_->req = std::move(req);

      if (!active_request_->route || !is_streaming_body_mode(active_request_->route.body)) {
        if (active_request_->route && active_request_->route.body == detail::body_mode::none && request_has_body(message)) {
          active_request_->rejected = true;
          send_error(status::bad_request, false);
          return;
        }
        start_body_timeout_if_needed();
        if (active_request_->route) {
          const auto keep_alive = owner_.owner.options_.keep_alive() && message.keep_alive && !request_has_body(message);
          if (!run_on_request_hooks(*active_request_, !keep_alive, active_request_->req.method() == method::head)) {
            stop_timeout();
          }
        }
        return;
      }

      const auto keep_alive = owner_.owner.options_.keep_alive() && message.keep_alive;
      start_body_timeout_if_needed();
      active_request_->slot = create_response_slot(!keep_alive, active_request_->req.method() == method::head);
      if (!active_request_->slot) {
        active_request_->rejected = true;
        stop_timeout();
        return;
      }
      configure_response_observation(*active_request_->slot, active_request_->req, active_request_->route.on_response_hooks);

      if (!run_on_request_hooks(*active_request_, !keep_alive, active_request_->req.method() == method::head)) {
        stop_timeout();
        return;
      }

      auto self = weak_from_this();
      active_request_->body_stream.on_pause_resume(
        [self] {
          if (auto session = self.lock()) {
            session->pause_request_body();
          }
        },
        [self] {
          if (auto session = self.lock()) {
            session->resume_request_body();
          }
        });

      dispatch_stream_request(*active_request_);
    }

    void dispatch_stream_request(active_request_state& active) {
      dispatching_response_ = true;
      struct dispatch_guard {
        session& self;
        ~dispatch_guard() {
          self.dispatching_response_ = false;
          self.flush_response_slots();
        }
      } guard{*this};

      response& res = active.slot->res;

      try {
        if (!run_hooks(active.route.pre_handler_hooks, active.req, res)) {
          finish_response_if_needed(res);
          active.rejected = true;
          return;
        }
        (*active.route.handler)(active.req, res, {}, &active.body_stream);
      } catch (...) {
        handle_exception(active.req, res, std::current_exception(), active.route.exception_handler);
        active.rejected = true;
      }
    }

    void handle_body_chunk(const std::string& chunk) {
      if (!active_request_ || active_request_->rejected) {
        return;
      }

      refresh_body_timeout();
      auto& active = *active_request_;
      active.received_body_bytes += chunk.size();
      if (active.route && active.received_body_bytes > route_body_limit(active.route)) {
        active.rejected = true;
        stop_timeout();
        if (is_streaming_body_mode(active.route.body)) {
          active.body_stream.emit_error(std::make_error_code(std::errc::message_size));
          if (active.route.body == detail::body_mode::multipart_stream) {
            return;
          }
        }
        send_error(status::payload_too_large, false);
        return;
      }

      if (active.route && is_streaming_body_mode(active.route.body)) {
        active.body_stream.emit_data(std::as_bytes(std::span{chunk.data(), chunk.size()}));
        if (active.body_stream.paused()) {
          body_processing_paused_ = true;
        }
      }
    }

    void handle_message_complete(const detail::http1_message& message) {
      if (!active_request_) {
        handle_message(message);
        return;
      }

      auto active = std::move(active_request_);
      stop_timeout();
      if (active->rejected) {
        return;
      }

      if (active->route && is_streaming_body_mode(active->route.body)) {
        active->body_stream.emit_end();
        return;
      }

      handle_buffered_message(
        message,
        std::move(active->route),
        active->parsed_path,
        std::move(active->slot));
    }

    void handle_buffered_message(
      const detail::http1_message& message,
      router::match_result route_match,
      const detail::route_path& parsed_path,
      std::shared_ptr<response_slot> existing_slot = {}) {
      const auto [path, query] = split_path_query(message.target);
      const auto keep_alive = owner_.owner.options_.keep_alive() && message.keep_alive;
      auto slot = std::move(existing_slot);
      if (!slot) {
        slot = create_response_slot(!keep_alive, message.method == method::head);
      }
      if (!slot) {
        return;
      }
      dispatching_response_ = true;
      struct dispatch_guard {
        session& self;
        ~dispatch_guard() {
          self.dispatching_response_ = false;
          self.flush_response_slots();
        }
      } guard{*this};

      request req{
        message.method,
        message.target,
        path,
        query,
        message.headers,
        body_bytes(message.body),
        route_params{},
        connection(),
        parsed_path.decoded_segments,
      };

      response& res = slot->res;

      if (!route_match) {
        auto fallback = owner_.owner.router_.fallback(parsed_path);
        req.params_ = std::move(fallback.params);
        try {
          auto allowed_methods = owner_.owner.router_.allowed_methods(parsed_path);
          if (!allowed_methods.empty()) {
            dispatch_automatic_method_response(req, res, std::move(allowed_methods));
          } else if (fallback.not_found_handler) {
            (*fallback.not_found_handler)(req, res, {}, nullptr);
            if (!res.ended() && !res.deferred() && !res.streaming()) {
              res.end();
            }
          } else if (owner_.owner.not_found_handler_) {
            owner_.owner.not_found_handler_(req, res, {}, nullptr);
            if (!res.ended() && !res.deferred() && !res.streaming()) {
              res.end();
            }
          } else {
            res.status(status::not_found).text("not found\n");
          }
        } catch (...) {
          handle_exception(req, res, std::current_exception(), fallback.exception_handler);
          return;
        }
        if (!res.ended() && !res.deferred() && !res.streaming()) {
          res.end();
        }
        return;
      }

      req.params_ = std::move(route_match.params);
      req.matched_pattern_ = route_match.pattern;
      const auto* exception_handler = route_match.exception_handler;
      configure_response_observation(*slot, req, route_match.on_response_hooks);

      try {
        if (!run_hooks(route_match.pre_handler_hooks, req, res)) {
          finish_response_if_needed(res);
          return;
        }
        auto body = req.body_bytes();
        (*route_match.handler)(req, res, body, nullptr);
        finish_response_if_needed(res);
      } catch (...) {
        handle_exception(req, res, std::current_exception(), exception_handler);
        return;
      }
    }

    void handle_message(const detail::http1_message& message) {
      const auto [path, query] = split_path_query(message.target);
      (void)query;
      const auto parsed_path = detail::parse_route_path(path);
      if (!parsed_path.valid) {
        send_error(status::bad_request, false);
        return;
      }
      handle_buffered_message(message, match_request_route(message.method, parsed_path), parsed_path);
    }

    void pause_request_body() {
      stop_timeout();
      body_processing_paused_ = true;
      try {
        stream_.read_stop();
      } catch (...) {
      }
    }

    void resume_request_body() {
      if (closed_) {
        return;
      }
      body_processing_paused_ = false;
      start_body_timeout_if_needed();
      process_parser_events();
      if (!closed_ && !body_processing_paused_) {
        start_read();
      }
    }

    void handle_exception(
      request& req,
      response& res,
      std::exception_ptr error,
      const exception_handler_type* scoped_handler = nullptr) {
      res.reset();
      const auto* handler = scoped_handler ? scoped_handler : &owner_.owner.exception_handler_;
      if (*handler) {
        try {
          (*handler)(req, res, std::move(error));
          if (!res.ended() && !res.deferred() && !res.streaming()) {
            res.end();
          }
        } catch (...) {
          res.reset();
          res.status(status::internal_server_error).text("internal server error\n");
        }
      } else {
        res.status(status::internal_server_error).text("internal server error\n");
      }
    }

    bool run_on_request_hooks(active_request_state& active, bool close_after, bool suppress_body) {
      if (active.route.on_request_hooks.empty()) {
        return true;
      }

      if (!active.slot) {
        active.slot = create_response_slot(close_after, suppress_body);
        if (!active.slot) {
          active.rejected = true;
          return false;
        }
      }
      configure_response_observation(*active.slot, active.req, active.route.on_response_hooks);

      dispatching_response_ = true;
      struct dispatch_guard {
        session& self;
        ~dispatch_guard() {
          self.dispatching_response_ = false;
          self.flush_response_slots();
        }
      } guard{*this};

      response& res = active.slot->res;
      try {
        if (!run_hooks(active.route.on_request_hooks, active.req, res)) {
          finish_response_if_needed(res);
          active.rejected = true;
          return false;
        }
      } catch (...) {
        handle_exception(active.req, res, std::current_exception(), active.route.exception_handler);
        active.rejected = true;
        return false;
      }

      return true;
    }

    bool run_hooks(const std::vector<const hook_type*>& hooks, request& req, response& res) {
      for (const auto* hook : hooks) {
        const auto result = (*hook)(req, res);
        if (result == hook_result::stop || res.ended() || res.deferred() || res.streaming()) {
          return false;
        }
      }
      return true;
    }

    void finish_response_if_needed(response& res) {
      if (!res.ended() && !res.deferred() && !res.streaming()) {
        res.end();
      }
    }

    request_snapshot snapshot_request(const request& req) const {
      return request_snapshot{
        req.method(),
        std::string(req.target()),
        std::string(req.path()),
        std::string(req.query()),
        std::string(req.matched_pattern()),
        req.params(),
        req.connection(),
      };
    }

    void configure_response_observation(
      response_slot& slot,
      const request& req,
      std::vector<response_hook_handle> hooks) {
      slot.request = snapshot_request(req);
      slot.on_response_hooks = std::move(hooks);
    }

    std::size_t response_body_size(const response_slot& slot) const noexcept {
      if (slot.streaming) {
        return slot.stream_body_bytes;
      }
      if (slot.res.status_code() == static_cast<unsigned int>(status::no_content)) {
        return 0;
      }
      return slot.res.body().size();
    }

    void run_response_hooks(response_slot& slot, response_outcome outcome) noexcept {
      if (slot.response_hooks_ran) {
        return;
      }
      slot.response_hooks_ran = true;

      if (slot.on_response_hooks.empty()) {
        return;
      }

      const auto info = response_info{
        slot.request,
        slot.res.status_code(),
        slot.res.headers(),
        response_body_size(slot),
        outcome,
      };

      for (auto hook = slot.on_response_hooks.rbegin(); hook != slot.on_response_hooks.rend(); ++hook) {
        try {
          (**hook)(info);
        } catch (...) {
        }
      }
    }

    void send_error(status status_code, bool keep_alive) {
      response res;
      res.status(status_code).text(std::string(reason_phrase(status_code)) + "\n");
      enqueue(serialize_response(res, keep_alive, owner_.owner.options_), !keep_alive);
    }

    void handle_upgrade(std::span<const std::byte> extra_bytes) {
      if (!active_request_) {
        send_error(status::bad_request, false);
        return;
      }

      const auto [path, query] = split_path_query(active_request_->headers.target);
      const auto parsed_path = detail::parse_route_path(path);
      if (!parsed_path.valid) {
        send_error(status::bad_request, false);
        return;
      }
      route_params params;
      const server::upgrade_route* route = nullptr;
      for (const auto& candidate : owner_.owner.upgrade_routes_) {
        params = {};
        if (detail::route_pattern_matches(
              candidate.parsed_pattern,
              parsed_path,
              owner_.owner.options_.route_path_matching(),
              params)) {
          route = &candidate;
          break;
        }
      }

      if (!route) {
        send_error(status::not_found, false);
        return;
      }

      auto req = upgrade_request{
        active_request_->headers.method,
        active_request_->headers.target,
        path,
        query,
        active_request_->headers.headers,
        std::move(params),
        connection(),
        extra_bytes,
        [self = weak_from_this()](std::string response, upgrade_request::accept_callback on_accept) {
          if (auto session = self.lock()) {
            session->accept_upgrade(std::move(response), std::move(on_accept));
          }
        },
        parsed_path.decoded_segments,
      };

      try {
        route->handler(req);
      } catch (...) {
        send_error(status::internal_server_error, false);
      }
    }

    void accept_upgrade(std::string response, upgrade_request::accept_callback on_accept) {
      close_timeout_timer();
      try {
        stream_.read_stop();
      } catch (...) {
      }
      writes_.push_back(pending_write{std::move(response), false, false, std::move(on_accept)});
      flush_next();
    }

    std::shared_ptr<response_slot> create_response_slot(bool close_after, bool suppress_body = false) {
      const auto pending_responses = responses_.size() + pending_response_writes_;
      if (pending_responses >= owner_.owner.options_.max_pending_responses_per_connection()) {
        close();
        return {};
      }

      auto slot = std::make_shared<response_slot>();
      slot->close_after = close_after;
      slot->suppress_body = suppress_body;

      auto self = weak_from_this();
      std::weak_ptr<response_slot> weak_slot = slot;
      slot->res.on_complete([self, weak_slot] {
        auto session = self.lock();
        auto slot = weak_slot.lock();
        if (session && slot) {
          session->complete_response(std::move(slot));
        }
      });
      slot->res.on_stream_write([self, weak_slot](std::string payload) {
        auto session = self.lock();
        auto slot = weak_slot.lock();
        if (!session || !slot) {
          return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
        }
        return session->write_stream_chunk(*slot, std::move(payload));
      });
      slot->res.on_stream_end([self, weak_slot] {
        auto session = self.lock();
        auto slot = weak_slot.lock();
        if (session && slot) {
          session->end_stream(*slot);
        }
      });

      responses_.push_back(slot);
      return slot;
    }

    void complete_response(std::shared_ptr<response_slot> slot) {
      if (slot->completed) {
        return;
      }

      slot->completed = true;
      run_response_hooks(*slot, response_outcome::completed);
      if (!dispatching_response_) {
        flush_response_slots();
      }
    }

    void flush_response_slots() {
      while (!responses_.empty()) {
        auto slot = responses_.front();
        if (slot->streaming) {
          flush_stream_writes(*slot);
          if (!slot->completed || !slot->stream_writes.empty()) {
            return;
          }
          responses_.pop_front();
          if (slot->close_after) {
            cancel_pending_responses();
            return;
          }
          continue;
        }

        if (!slot->completed) {
          return;
        }

        responses_.pop_front();

        ++pending_response_writes_;
        enqueue(
          serialize_response(slot->res, !slot->close_after, owner_.owner.options_, slot->suppress_body),
          slot->close_after,
          true);
        if (slot->close_after) {
          cancel_pending_responses();
          return;
        }
      }
    }

    void cancel_pending_responses() noexcept {
      for (auto& slot : responses_) {
        if (!slot->completed) {
          slot->res.cancel();
          run_response_hooks(*slot, response_outcome::cancelled);
        }
      }
      responses_.clear();
    }

    void cancel_pending_responses(std::error_code) noexcept {
      for (auto& slot : responses_) {
        if (!slot->completed) {
          slot->res.cancel();
          run_response_hooks(*slot, response_outcome::error);
        }
      }
      responses_.clear();
    }

    void enqueue(std::string payload, bool close_after, bool counts_response = false) {
      pending_write_bytes_ += payload.size();
      writes_.push_back(pending_write{std::move(payload), close_after, counts_response, {}});
      flush_next();
    }

    void queue_stream_write(response_slot& slot, std::string payload, bool close_after = false) {
      pending_write_bytes_ += payload.size();
      slot.stream_writes.push_back(pending_write{std::move(payload), close_after, false, {}});
    }

    [[nodiscard]] std::size_t low_watermark() const noexcept {
      return owner_.owner.options_.max_pending_write_bytes() / 2;
    }

    stream_write_result write_stream_chunk(response_slot& slot, std::string payload) {
      if (closed_ || slot.completed || slot.stream_ended) {
        return stream_write_result::rejected(std::make_error_code(std::errc::not_connected));
      }
      if (slot.stream_backpressured) {
        return stream_write_result::rejected(std::make_error_code(std::errc::operation_would_block));
      }

      slot.streaming = true;
      if (slot.suppress_body) {
        return stream_write_result::ready();
      }

      if (!slot.stream_headers_queued) {
        slot.stream_chunked = !slot.res.headers().contains("content-length");
        slot.res.commit_headers();
        queue_stream_write(
          slot,
          serialize_response_head(
            slot.res,
            !slot.close_after,
            owner_.owner.options_,
            slot.stream_chunked,
            slot.suppress_body));
        slot.stream_headers_queued = true;
      }

      if (!slot.suppress_body && !payload.empty()) {
        slot.stream_body_bytes += payload.size();
        queue_stream_write(slot, slot.stream_chunked ? serialize_chunk(std::move(payload)) : std::move(payload));
      }

      flush_response_slots();

      if (pending_write_bytes_ >= owner_.owner.options_.max_pending_write_bytes()) {
        slot.stream_backpressured = true;
        return stream_write_result::backpressure();
      }
      return stream_write_result::ready();
    }

    void end_stream(response_slot& slot) {
      if (closed_ || slot.completed || slot.stream_ended) {
        return;
      }

      slot.streaming = true;
      slot.stream_ended = true;
      if (!slot.stream_headers_queued) {
        slot.stream_chunked = !slot.res.headers().contains("content-length");
        slot.res.commit_headers();
        queue_stream_write(
          slot,
          serialize_response_head(
            slot.res,
            !slot.close_after,
            owner_.owner.options_,
            slot.stream_chunked,
            slot.suppress_body),
          slot.suppress_body && slot.close_after);
        slot.stream_headers_queued = true;
      }

      if (!slot.suppress_body && slot.stream_chunked) {
        queue_stream_write(slot, "0\r\n\r\n", slot.close_after);
      }
      slot.res.complete_stream();
      if (!dispatching_response_) {
        flush_response_slots();
      }
    }

    void flush_stream_writes(response_slot& slot) {
      while (!slot.stream_writes.empty()) {
        auto write = std::move(slot.stream_writes.front());
        slot.stream_writes.pop_front();
        writes_.push_back(std::move(write));
      }
      flush_next();
    }

    void flush_next() {
      if (closed_ || writing_ || writes_.empty()) {
        return;
      }

      writing_ = true;
      auto self = weak_from_this();
      stream_.write(as_bytes(writes_.front().payload), [self](uvp::io::stream_error error) {
        if (auto session = self.lock()) {
          session->on_write(std::move(error));
        }
      });
    }

    void on_write(uvp::io::stream_error error) {
      writing_ = false;
      if (closed_) {
        return;
      }

      if (error) {
        notify_stream_errors(error.code());
        cancel_pending_responses(error.code());
        close();
        return;
      }

      const auto close_after = !writes_.empty() && writes_.front().close_after;
      auto upgrade = !writes_.empty() ? std::move(writes_.front().upgrade) : upgrade_request::accept_callback{};
      if (!writes_.empty()) {
        pending_write_bytes_ -= std::min(pending_write_bytes_, writes_.front().payload.size());
        if (writes_.front().counts_response && pending_response_writes_ > 0) {
          --pending_response_writes_;
        }
        writes_.pop_front();
      }

      if (upgrade) {
        closed_ = true;
        auto upgraded_stream = std::move(stream_);
        owner_.remove_session(this);
        upgrade(std::move(upgraded_stream));
        return;
      }

      notify_drains();

      if (close_after) {
        close();
        return;
      }

      flush_next();
      maybe_start_idle_timeout();
    }

    void notify_drains() {
      if (pending_write_bytes_ > low_watermark()) {
        return;
      }

      for (auto& slot : responses_) {
        if (!slot->stream_backpressured) {
          continue;
        }
        slot->stream_backpressured = false;
        slot->res.notify_stream_drain();
      }
    }

    void start_timeout(timeout_phase phase, std::chrono::milliseconds duration) {
      if (closed_ || timeout_timer_closed_) {
        return;
      }
      stop_timeout();
      timeout_phase_ = phase;
      timeout_timer_active_ = true;
      auto self = weak_from_this();
      timeout_timer_.start(duration, [self, phase](uv::timer&) {
        if (auto session = self.lock()) {
          session->on_timeout(phase);
        }
      });
    }

    void stop_timeout() noexcept {
      if (timeout_timer_closed_) {
        timeout_phase_ = timeout_phase::none;
        timeout_timer_active_ = false;
        return;
      }
      if (timeout_timer_active_) {
        try {
          timeout_timer_.stop();
        } catch (...) {
        }
      }
      timeout_phase_ = timeout_phase::none;
      timeout_timer_active_ = false;
    }

    void close_timeout_timer() noexcept {
      if (timeout_timer_closed_) {
        return;
      }
      stop_timeout();
      timeout_timer_closed_ = true;
      if (auto self = weak_from_this().lock()) {
        timeout_timer_.close([self](uv::timer&) {});
      } else {
        timeout_timer_.close();
      }
    }

    void on_timeout(timeout_phase phase) {
      if (closed_ || timeout_timer_closed_ || phase != timeout_phase_) {
        return;
      }
      timeout_timer_active_ = false;
      timeout_phase_ = timeout_phase::none;

      switch (phase) {
      case timeout_phase::header:
        timeout_closing_ = true;
        try {
          stream_.read_stop();
        } catch (...) {
        }
        send_error(status::request_timeout, false);
        break;
      case timeout_phase::body: {
        const auto error = std::make_error_code(std::errc::timed_out);
        timeout_closing_ = true;
        if (active_request_ && active_request_->route && is_streaming_body_mode(active_request_->route.body)) {
          active_request_->body_stream.emit_error(error);
        }
        notify_stream_errors(error);
        cancel_pending_responses(error);
        close();
        break;
      }
      case timeout_phase::idle:
        close();
        break;
      case timeout_phase::none:
        break;
      }
    }

    void start_body_timeout_if_needed() {
      if (!active_request_ || active_request_->rejected || body_processing_paused_ || !request_has_body(active_request_->headers)) {
        return;
      }
      if (active_request_->route) {
        start_timeout(timeout_phase::body, route_body_timeout(active_request_->route));
      } else {
        start_timeout(timeout_phase::body, owner_.owner.options_.body_timeout());
      }
    }

    void refresh_body_timeout() {
      if (timeout_phase_ != timeout_phase::body) {
        return;
      }
      start_body_timeout_if_needed();
    }

    void maybe_start_idle_timeout() {
      if (closed_ || timeout_closing_ || timeout_phase_ != timeout_phase::none || !owner_.owner.options_.keep_alive()) {
        return;
      }
      if (active_request_ || body_processing_paused_ || writing_ || !writes_.empty() || !responses_.empty()) {
        return;
      }
      if (pending_response_writes_ != 0 || handled_events_ < parser_.events().size()) {
        return;
      }
      start_timeout(timeout_phase::idle, owner_.owner.options_.idle_timeout());
    }

    void notify_stream_errors(std::error_code error) {
      for (auto& slot : responses_) {
        if (slot->streaming && !slot->completed) {
          slot->res.notify_stream_error(error);
        }
      }
    }

    impl& owner_;
    uvp::io::byte_stream stream_;
    uv::timer timeout_timer_;
    detail::http1_state_machine parser_;
    std::size_t handled_events_ = 0;
    std::size_t pending_response_writes_ = 0;
    std::size_t pending_write_bytes_ = 0;
    std::unique_ptr<active_request_state> active_request_;
    std::deque<std::shared_ptr<response_slot>> responses_;
    std::deque<pending_write> writes_;
    bool writing_ = false;
    bool dispatching_response_ = false;
    bool body_processing_paused_ = false;
    bool closed_ = false;
    bool timeout_closing_ = false;
    bool timeout_timer_active_ = false;
    bool timeout_timer_closed_ = false;
    timeout_phase timeout_phase_ = timeout_phase::none;
  };

  void add_listener(uvp::io::stream_listener listener) {
    listeners_.push_back(std::move(listener));
    auto& stored = listeners_.back();
    stored.listen([this](uvp::io::accept_result result) {
      if (!result) {
        return;
      }

      auto accepted = std::make_shared<session>(*this, std::move(result).stream());
      sessions_.push_back(accepted);
      accepted->start();
    });
  }

  void close() {
    for (auto& listener : listeners_) {
      listener.close();
    }
    for (auto& session : sessions_) {
      session->close();
    }
  }

  void remove_session(session* session) {
    std::erase_if(sessions_, [session](const std::shared_ptr<impl::session>& candidate) {
      return candidate.get() == session;
    });
  }

  server& owner;
  std::vector<uvp::io::stream_listener> listeners_;
  std::vector<std::shared_ptr<session>> sessions_;
};

server::server(uv::loop& loop)
    : server(loop, server_options{}) {}

server::server(uv::loop& loop, server_options options)
    : loop_(&loop),
      options_(options),
      router_(options_.route_path_matching()),
      impl_(std::make_unique<impl>(*this)) {
  options_.validate();
}

server::~server() = default;

server& server::add_upgrade_route(std::string_view pattern, upgrade_handler_type handler) {
  auto parsed_pattern = detail::parse_route_path(pattern);
  if (!parsed_pattern.valid) {
    throw std::invalid_argument("HTTP upgrade route pattern contains invalid percent encoding");
  }

  upgrade_routes_.push_back(upgrade_route{
    std::move(parsed_pattern),
    std::move(handler),
  });
  return *this;
}

void server::listen(std::string_view host, unsigned int port) {
  auto listener = uvp::io::tcp_listener{*loop_};
  listener.bind(host, port);
  listen(std::move(listener));
}

void server::listen(uvp::io::stream_listener listener) {
  impl_->add_listener(std::move(listener));
}

void server::close() noexcept {
  if (impl_) {
    impl_->close();
  }
}

} // namespace uvp::http
