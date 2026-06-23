#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
    routes_.push_back(route_entry{
      method_value,
      std::string(pattern),
      detail::body_mode::none,
      0,
      detail::wrap_none_handler(std::forward<Handler>(handler)),
    });
    return *this;
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::bytes policy, Handler&& handler) {
    routes_.push_back(route_entry{
      method_value,
      std::string(pattern),
      detail::body_mode::bytes,
      policy.max_size,
      detail::wrap_bytes_handler(std::forward<Handler>(handler)),
    });
    return *this;
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::text policy, Handler&& handler) {
    routes_.push_back(route_entry{
      method_value,
      std::string(pattern),
      detail::body_mode::text,
      policy.max_size,
      detail::wrap_text_handler(std::forward<Handler>(handler)),
    });
    return *this;
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::stream policy, Handler&& handler) {
    routes_.push_back(route_entry{
      method_value,
      std::string(pattern),
      detail::body_mode::stream,
      policy.max_size,
      detail::wrap_stream_handler(std::forward<Handler>(handler)),
    });
    return *this;
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, Handler&& handler) {
    return route(method_value, pattern, detail::infer_body_policy<Handler>(), std::forward<Handler>(handler));
  }

#define UVP_HTTP_ROUTE_METHOD(name, method_name) \
  template<class BodyPolicy, class Handler> \
  router& name(std::string_view pattern, BodyPolicy policy, Handler&& handler) { \
    return route(method::method_name, pattern, policy, std::forward<Handler>(handler)); \
  } \
  template<class Handler> \
  router& name(std::string_view pattern, Handler&& handler) { \
    return route(method::method_name, pattern, std::forward<Handler>(handler)); \
  }

  UVP_HTTP_ROUTE_METHOD(get, get)
  UVP_HTTP_ROUTE_METHOD(post, post)
  UVP_HTTP_ROUTE_METHOD(put, put)
  UVP_HTTP_ROUTE_METHOD(patch, patch)
  UVP_HTTP_ROUTE_METHOD(delete_, delete_)
  UVP_HTTP_ROUTE_METHOD(head, head)
  UVP_HTTP_ROUTE_METHOD(options, options)

#undef UVP_HTTP_ROUTE_METHOD

  [[nodiscard]] match_result match(method method_value, std::string_view path) const;
  [[nodiscard]] const handler_type* find(method method_value, std::string_view path) const;
  [[nodiscard]] bool empty() const noexcept { return routes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return routes_.size(); }

private:
  struct route_entry {
    method method_value;
    std::string pattern;
    detail::body_mode body;
    std::size_t max_body_bytes;
    handler_type handler;
  };

  std::vector<route_entry> routes_;
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
