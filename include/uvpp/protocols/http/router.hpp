#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

namespace detail {

enum class body_mode {
  none,
  bytes,
  text,
  stream,
};

using route_handler_type = std::function<void(request&, response&, std::span<const std::byte>, request_body_stream*)>;

template<class Handler>
route_handler_type wrap_none_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_bytes_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_text_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_stream_handler(Handler&& handler);
template<class Handler>
constexpr auto infer_body_policy();
} // namespace detail

class router {
public:
  using handler_type = detail::route_handler_type;

  struct match_result {
    const handler_type* handler = nullptr;
    detail::body_mode body = detail::body_mode::none;
    std::size_t max_body_bytes = 0;
    route_params params;

    [[nodiscard]] bool ok() const noexcept { return handler != nullptr; }
    explicit operator bool() const noexcept { return ok(); }
  };

  router() = default;

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::none policy, Handler&& handler) {
    (void)policy;
    return add_route(
      method_value,
      pattern,
      detail::body_mode::none,
      0,
      detail::wrap_none_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::bytes policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::bytes,
      policy.max_size,
      detail::wrap_bytes_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::text policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::text,
      policy.max_size,
      detail::wrap_text_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::stream policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::stream,
      policy.max_size,
      detail::wrap_stream_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, Handler&& handler) {
    return route(method_value, pattern, detail::infer_body_policy<Handler>(), std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& get(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::get, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& get(std::string_view pattern, Handler&& handler) {
    return route(method::get, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& post(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::post, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& post(std::string_view pattern, Handler&& handler) {
    return route(method::post, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& put(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::put, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& put(std::string_view pattern, Handler&& handler) {
    return route(method::put, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& patch(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::patch, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& patch(std::string_view pattern, Handler&& handler) {
    return route(method::patch, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& delete_(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::delete_, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& delete_(std::string_view pattern, Handler&& handler) {
    return route(method::delete_, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& head(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::head, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& head(std::string_view pattern, Handler&& handler) {
    return route(method::head, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& options(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::options, pattern, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& options(std::string_view pattern, Handler&& handler) {
    return route(method::options, pattern, std::forward<Handler>(handler));
  }

  [[nodiscard]] match_result match(method method_value, std::string_view path) const;
  [[nodiscard]] const handler_type* find(method method_value, std::string_view path) const;
  [[nodiscard]] bool empty() const noexcept { return route_count_ == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return route_count_; }

private:
  struct route_target {
    detail::body_mode body;
    std::size_t max_body_bytes;
    handler_type handler;
  };

  static constexpr std::size_t method_count_ = static_cast<std::size_t>(method::unknown) + 1U;

  struct named_child {
    std::string name;
    std::size_t node = 0;
  };

  struct trie_node {
    std::unordered_map<std::string, std::size_t> static_children;
    std::optional<named_child> parameter_child;
    std::optional<named_child> wildcard_child;
    std::array<std::optional<route_target>, method_count_> targets;
  };

  router& add_route(
    method method_value,
    std::string_view pattern,
    detail::body_mode body,
    std::size_t max_body_bytes,
    handler_type handler);
  [[nodiscard]] const route_target* match_node(
    std::size_t node_index,
    std::size_t method_index,
    std::string_view path,
    route_params& params) const;

  std::vector<trie_node> nodes_{{}};
  std::size_t route_count_ = 0;
};

namespace detail {

template<class Handler>
router::handler_type wrap_none_handler(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](
           request& req,
           response& res,
           std::span<const std::byte>,
           request_body_stream*) mutable {
    handler(req, res);
  };
}

template<class Handler>
router::handler_type wrap_bytes_handler(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](
           request& req,
           response& res,
           std::span<const std::byte> body,
           request_body_stream*) mutable {
    handler(req, res, body);
  };
}

template<class Handler>
router::handler_type wrap_text_handler(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](
           request& req,
           response& res,
           std::span<const std::byte> body,
           request_body_stream*) mutable {
    const auto text = std::string_view{
      reinterpret_cast<const char*>(body.data()),
      body.size(),
    };
    handler(req, res, text);
  };
}

template<class Handler>
router::handler_type wrap_stream_handler(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](
           request& req,
           response& res,
           std::span<const std::byte>,
           request_body_stream* body) mutable {
    handler(req, res, *body);
  };
}

template<class Handler>
constexpr auto infer_body_policy() {
  using handler_type = std::remove_reference_t<Handler>;
  if constexpr (std::is_invocable_v<handler_type&, request&, response&>) {
    return body::none{};
  } else if constexpr (std::is_invocable_v<handler_type&, request&, response&, std::span<const std::byte>>) {
    return body::bytes{};
  } else if constexpr (std::is_invocable_v<handler_type&, request&, response&, std::string_view>) {
    return body::text{};
  } else if constexpr (std::is_invocable_v<handler_type&, request&, response&, request_body_stream&>) {
    return body::stream{};
  } else {
    static_assert(std::is_invocable_v<handler_type&, request&, response&>,
      "HTTP route handler must accept (request&, response&), "
      "(request&, response&, std::span<const std::byte>), or "
      "(request&, response&, std::string_view), or "
      "(request&, response&, request_body_stream&)");
    return body::none{};
  }
}

} // namespace detail

} // namespace uvp::http
