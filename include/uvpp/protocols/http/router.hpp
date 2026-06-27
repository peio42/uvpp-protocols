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

enum class hook_result {
  next,
  stop,
};

using hook_type = std::function<hook_result(request&, response&)>;

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
template<class Handler>
hook_type wrap_hook(Handler&& handler);
} // namespace detail

class route_group;

class router {
public:
  using handler_type = detail::route_handler_type;

  struct match_result {
    const handler_type* handler = nullptr;
    detail::body_mode body = detail::body_mode::none;
    std::size_t max_body_bytes = 0;
    route_params params;
    std::vector<const hook_type*> on_request_hooks;
    std::vector<const hook_type*> pre_handler_hooks;

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

  template<class Handler>
  router& on_request(Handler&& handler) {
    return add_on_request_hook({}, detail::wrap_hook(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& pre_handler(Handler&& handler) {
    return add_pre_handler_hook({}, detail::wrap_hook(std::forward<Handler>(handler)));
  }

  [[nodiscard]] route_group group(std::string_view prefix);

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
  [[nodiscard]] std::vector<method> allowed_methods(std::string_view path) const;
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
    std::vector<hook_type> on_request_hooks;
    std::vector<hook_type> pre_handler_hooks;
  };

  router& add_route(
    method method_value,
    std::string_view pattern,
    detail::body_mode body,
    std::size_t max_body_bytes,
    handler_type handler);
  router& add_on_request_hook(std::string_view prefix, hook_type hook);
  router& add_pre_handler_hook(std::string_view prefix, hook_type hook);
  [[nodiscard]] std::size_t ensure_prefix_node(std::string_view prefix);
  [[nodiscard]] const route_target* match_node(
    std::size_t node_index,
    std::size_t method_index,
    std::string_view path,
    route_params& params,
    std::vector<const hook_type*>& on_request_hooks,
    std::vector<const hook_type*>& pre_handler_hooks) const;

  std::vector<trie_node> nodes_{{}};
  std::size_t route_count_ = 0;

  friend class route_group;
};

class route_group {
public:
  template<class Handler>
  route_group& on_request(Handler&& handler) {
    router_->add_on_request_hook(prefix_, detail::wrap_hook(std::forward<Handler>(handler)));
    return *this;
  }

  template<class Handler>
  route_group& pre_handler(Handler&& handler) {
    router_->add_pre_handler_hook(prefix_, detail::wrap_hook(std::forward<Handler>(handler)));
    return *this;
  }

  [[nodiscard]] route_group group(std::string_view prefix) const;

  template<class Handler>
  route_group& route(method method_value, std::string_view pattern, Handler&& handler) {
    router_->route(method_value, route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& route(method method_value, std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->route(method_value, route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& get(std::string_view pattern, Handler&& handler) {
    router_->get(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& get(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->get(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& post(std::string_view pattern, Handler&& handler) {
    router_->post(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& post(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->post(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& put(std::string_view pattern, Handler&& handler) {
    router_->put(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& put(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->put(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& patch(std::string_view pattern, Handler&& handler) {
    router_->patch(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& patch(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->patch(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& delete_(std::string_view pattern, Handler&& handler) {
    router_->delete_(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& delete_(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->delete_(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& head(std::string_view pattern, Handler&& handler) {
    router_->head(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& head(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->head(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_group& options(std::string_view pattern, Handler&& handler) {
    router_->options(route_pattern(pattern), std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_group& options(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    router_->options(route_pattern(pattern), policy, std::forward<Handler>(handler));
    return *this;
  }

private:
  route_group(router& owner, std::string prefix) : router_(&owner), prefix_(std::move(prefix)) {}

  [[nodiscard]] std::string route_pattern(std::string_view pattern) const;

  router* router_ = nullptr;
  std::string prefix_;

  friend class router;
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

template<class Handler>
hook_type wrap_hook(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](request& req, response& res) mutable {
    if constexpr (std::is_invocable_r_v<hook_result, stored_handler&, request&, response&>) {
      return handler(req, res);
    } else if constexpr (
      std::is_invocable_v<stored_handler&, request&, response&> &&
      std::is_void_v<std::invoke_result_t<stored_handler&, request&, response&>>) {
      handler(req, res);
      return hook_result::next;
    } else {
      static_assert(std::is_invocable_r_v<hook_result, stored_handler&, request&, response&>,
        "HTTP hook must accept (request&, response&) and return hook_result or void");
      return hook_result::next;
    }
  };
}

} // namespace detail

} // namespace uvp::http
