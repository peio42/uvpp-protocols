#pragma once

#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <uvpp/protocols/http/detail/route_path.hpp>
#include <uvpp/protocols/http/method.hpp>
#include <uvpp/protocols/http/multipart.hpp>
#include <uvpp/protocols/http/request.hpp>
#include <uvpp/protocols/http/response.hpp>
#include <uvpp/protocols/http/route_params.hpp>

namespace uvp::http {

enum class hook_result {
  next,
  stop,
};

enum class response_outcome {
  completed,
  cancelled,
  error,
};

struct request_snapshot {
  http::method method = http::method::unknown;
  std::string target;
  std::string path;
  std::string query;
  std::string matched_pattern;
  route_params params;
  http::connection_info connection;
};

struct response_info {
  const request_snapshot& request;
  unsigned int status = static_cast<unsigned int>(http::status::ok);
  const http::headers& response_headers;
  std::size_t response_body_size = 0;
  response_outcome outcome = response_outcome::completed;
};

using hook_type = std::function<hook_result(request&, response&)>;
using response_hook_type = std::function<void(const response_info&)>;
using response_hook_handle = std::shared_ptr<const response_hook_type>;
using exception_handler_type = std::function<void(request&, response&, std::exception_ptr)>;

struct route_options {
  route_options& max_body_bytes(std::size_t value) & noexcept {
    max_body_bytes_ = value;
    return *this;
  }

  route_options&& max_body_bytes(std::size_t value) && noexcept {
    max_body_bytes(value);
    return std::move(*this);
  }

  [[nodiscard]] std::size_t max_body_bytes() const noexcept { return max_body_bytes_; }

  route_options& body_timeout(std::chrono::milliseconds value) & {
    if (value.count() <= 0) {
      throw std::invalid_argument("body_timeout must be greater than zero");
    }
    body_timeout_ = value;
    return *this;
  }

  route_options&& body_timeout(std::chrono::milliseconds value) && {
    body_timeout(value);
    return std::move(*this);
  }

  [[nodiscard]] std::chrono::milliseconds body_timeout() const noexcept { return body_timeout_; }

private:
  std::size_t max_body_bytes_ = 0;
  std::chrono::milliseconds body_timeout_{0};
};

namespace detail {

enum class body_mode {
  none,
  bytes,
  text,
  json,
  stream,
  multipart_stream,
  multipart_form,
};

using route_handler_type = std::function<void(request&, response&, std::span<const std::byte>, request_body_stream*)>;

template<class Handler>
route_handler_type wrap_none_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_bytes_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_text_handler(Handler&& handler);
template<class T, class Handler>
route_handler_type wrap_json_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_stream_handler(Handler&& handler);
template<class Handler>
route_handler_type wrap_multipart_stream_handler(Handler&& handler, multipart_stream_options options = {});
template<class Handler>
route_handler_type wrap_multipart_form_handler(Handler&& handler, multipart_form_options options = {});
template<class Handler>
constexpr auto infer_body_policy();
template<class Handler>
hook_type wrap_hook(Handler&& handler);
template<class Handler>
response_hook_type wrap_response_hook(Handler&& handler);
} // namespace detail

class route_group;
class route_resource;

class router {
public:
  using handler_type = detail::route_handler_type;

  struct match_result {
    const handler_type* handler = nullptr;
    detail::body_mode body = detail::body_mode::none;
    std::size_t max_body_bytes = 0;
    std::chrono::milliseconds body_timeout{0};
    std::string_view pattern;
    bool invalid_path = false;
    route_params params;
    std::vector<const hook_type*> on_request_hooks;
    std::vector<const hook_type*> pre_handler_hooks;
    std::vector<response_hook_handle> on_response_hooks;
    const exception_handler_type* exception_handler = nullptr;

    [[nodiscard]] bool ok() const noexcept { return handler != nullptr; }
    explicit operator bool() const noexcept { return ok(); }
  };

  struct fallback_result {
    const handler_type* not_found_handler = nullptr;
    const exception_handler_type* exception_handler = nullptr;
    route_params params;

    [[nodiscard]] bool has_not_found() const noexcept { return not_found_handler != nullptr; }
  };

  explicit router(route_path_matching matching = route_path_matching::percent_decoded_segments) noexcept
      : matching_(matching) {}

  [[nodiscard]] http::route_path_matching path_matching() const noexcept { return matching_; }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::none policy, Handler&& handler) {
    (void)policy;
    return add_route(
      method_value,
      pattern,
      detail::body_mode::none,
      0,
      options.body_timeout(),
      detail::wrap_none_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::none policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::bytes policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::bytes,
      options.max_body_bytes(),
      options.body_timeout(),
      detail::wrap_bytes_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::bytes policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::text policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::text,
      options.max_body_bytes(),
      options.body_timeout(),
      detail::wrap_text_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::text policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class T, class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::json<T> policy, Handler&& handler) {
    (void)policy;
    return add_route(
      method_value,
      pattern,
      detail::body_mode::json,
      options.max_body_bytes(),
      options.body_timeout(),
      detail::wrap_json_handler<T>(std::forward<Handler>(handler)));
  }

  template<class T, class Handler>
  router& route(method method_value, std::string_view pattern, body::json<T> policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::stream policy, Handler&& handler) {
    return add_route(
      method_value,
      pattern,
      detail::body_mode::stream,
      options.max_body_bytes(),
      options.body_timeout(),
      detail::wrap_stream_handler(std::forward<Handler>(handler)));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::stream policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::multipart_stream policy, Handler&& handler) {
    const auto body_limit =
      options.max_body_bytes() == 0 ? policy.options().limits().max_total_bytes : options.max_body_bytes();
    return add_route(
      method_value,
      pattern,
      detail::body_mode::multipart_stream,
      body_limit,
      options.body_timeout(),
      detail::wrap_multipart_stream_handler(std::forward<Handler>(handler), policy.options()));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::multipart_stream policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, body::multipart_form policy, Handler&& handler) {
    const auto body_limit =
      options.max_body_bytes() == 0 ? policy.options().limits.max_total_bytes : options.max_body_bytes();
    return add_route(
      method_value,
      pattern,
      detail::body_mode::multipart_form,
      body_limit,
      options.body_timeout(),
      detail::wrap_multipart_form_handler(std::forward<Handler>(handler), policy.options()));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, body::multipart_form policy, Handler&& handler) {
    return route(method_value, pattern, route_options{}, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& route(method method_value, std::string_view pattern, route_options options, Handler&& handler) {
    return route(
      method_value,
      pattern,
      options,
      detail::infer_body_policy<Handler>(),
      std::forward<Handler>(handler));
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

  template<class Handler>
  router& on_response(Handler&& handler) {
    return add_on_response_hook({}, detail::wrap_response_hook(std::forward<Handler>(handler)));
  }

  [[nodiscard]] route_group group(std::string_view prefix);
  [[nodiscard]] route_resource resource(std::string_view pattern);
  router& mount(std::string_view prefix, router&& mounted);

  template<class BodyPolicy, class Handler>
  router& get(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::get, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& get(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::get, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& get(std::string_view pattern, Handler&& handler) {
    return route(method::get, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& post(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::post, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& post(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::post, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& post(std::string_view pattern, Handler&& handler) {
    return route(method::post, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& put(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::put, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& put(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::put, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& put(std::string_view pattern, Handler&& handler) {
    return route(method::put, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& patch(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::patch, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& patch(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::patch, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& patch(std::string_view pattern, Handler&& handler) {
    return route(method::patch, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& delete_(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::delete_, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& delete_(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::delete_, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& delete_(std::string_view pattern, Handler&& handler) {
    return route(method::delete_, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& head(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::head, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& head(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::head, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& head(std::string_view pattern, Handler&& handler) {
    return route(method::head, pattern, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& options(std::string_view pattern, BodyPolicy policy, Handler&& handler) {
    return route(method::options, pattern, policy, std::forward<Handler>(handler));
  }

  template<class BodyPolicy, class Handler>
  router& options(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    return route(method::options, pattern, options, policy, std::forward<Handler>(handler));
  }

  template<class Handler>
  router& options(std::string_view pattern, Handler&& handler) {
    return route(method::options, pattern, std::forward<Handler>(handler));
  }

  [[nodiscard]] match_result match(method method_value, std::string_view path) const;
  [[nodiscard]] fallback_result fallback(std::string_view path) const;
  [[nodiscard]] const handler_type* find(method method_value, std::string_view path) const;
  [[nodiscard]] std::vector<method> allowed_methods(std::string_view path) const;
  [[nodiscard]] bool empty() const noexcept { return route_count_ == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return route_count_; }

private:
  struct route_target {
    detail::body_mode body;
    std::size_t max_body_bytes;
    std::chrono::milliseconds body_timeout;
    handler_type handler;
    std::string pattern;
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
    std::vector<response_hook_handle> on_response_hooks;
    std::optional<handler_type> not_found_handler;
    std::optional<exception_handler_type> exception_handler;
  };

  router& add_route(
    method method_value,
    std::string_view pattern,
    detail::body_mode body,
    std::size_t max_body_bytes,
    std::chrono::milliseconds body_timeout,
    handler_type handler);
  router& add_on_request_hook(std::string_view prefix, hook_type hook);
  router& add_pre_handler_hook(std::string_view prefix, hook_type hook);
  router& add_on_response_hook(std::string_view prefix, response_hook_type hook);
  router& add_not_found_handler(std::string_view prefix, handler_type handler);
  router& add_exception_handler(std::string_view prefix, exception_handler_type handler);
  [[nodiscard]] std::size_t ensure_prefix_node(std::string_view prefix);
  void validate_mount_node(const router& mounted, std::size_t destination_index, std::size_t source_index) const;
  void prefix_route_patterns(std::string_view prefix);
  void merge_mount_node(router& mounted, std::size_t destination_index, std::size_t source_index);
  [[nodiscard]] std::size_t move_mount_subtree(router& mounted, std::size_t source_index);
  [[nodiscard]] const route_target* match_node(
    std::size_t node_index,
    std::size_t method_index,
    std::span<const std::string> segments,
    std::size_t segment_index,
    route_params& params,
    std::vector<const hook_type*>& on_request_hooks,
    std::vector<const hook_type*>& pre_handler_hooks,
    std::vector<response_hook_handle>& on_response_hooks,
    const exception_handler_type*& exception_handler) const;

  std::vector<trie_node> nodes_{{}};
  std::size_t route_count_ = 0;
  http::route_path_matching matching_ = http::route_path_matching::percent_decoded_segments;

  [[nodiscard]] match_result match(method method_value, const detail::route_path& path) const;
  [[nodiscard]] fallback_result fallback(const detail::route_path& path) const;
  [[nodiscard]] std::vector<method> allowed_methods(const detail::route_path& path) const;

  friend class server;
  friend class route_group;
  friend class route_resource;
};

class route_resource {
public:
  template<class Handler>
  route_resource& route(method method_value, Handler&& handler) {
    router_->route(method_value, pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& route(method method_value, BodyPolicy policy, Handler&& handler) {
    router_->route(method_value, pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& route(method method_value, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->route(method_value, pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& get(Handler&& handler) {
    router_->get(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& get(BodyPolicy policy, Handler&& handler) {
    router_->get(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& get(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->get(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& post(Handler&& handler) {
    router_->post(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& post(BodyPolicy policy, Handler&& handler) {
    router_->post(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& post(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->post(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& put(Handler&& handler) {
    router_->put(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& put(BodyPolicy policy, Handler&& handler) {
    router_->put(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& put(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->put(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& patch(Handler&& handler) {
    router_->patch(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& patch(BodyPolicy policy, Handler&& handler) {
    router_->patch(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& patch(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->patch(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& delete_(Handler&& handler) {
    router_->delete_(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& delete_(BodyPolicy policy, Handler&& handler) {
    router_->delete_(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& delete_(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->delete_(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& head(Handler&& handler) {
    router_->head(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& head(BodyPolicy policy, Handler&& handler) {
    router_->head(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& head(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->head(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class Handler>
  route_resource& options(Handler&& handler) {
    router_->options(pattern_, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& options(BodyPolicy policy, Handler&& handler) {
    router_->options(pattern_, policy, std::forward<Handler>(handler));
    return *this;
  }

  template<class BodyPolicy, class Handler>
  route_resource& options(route_options options, BodyPolicy policy, Handler&& handler) {
    router_->options(pattern_, options, policy, std::forward<Handler>(handler));
    return *this;
  }

private:
  route_resource(router& owner, std::string pattern) : router_(&owner), pattern_(std::move(pattern)) {}

  router* router_ = nullptr;
  std::string pattern_;

  friend class router;
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

  template<class Handler>
  route_group& on_response(Handler&& handler) {
    router_->add_on_response_hook(prefix_, detail::wrap_response_hook(std::forward<Handler>(handler)));
    return *this;
  }

  template<class Handler>
  route_group& not_found(Handler&& handler) {
    router_->add_not_found_handler(prefix_, detail::wrap_none_handler(std::forward<Handler>(handler)));
    return *this;
  }

  template<class Handler>
  route_group& on_exception(Handler&& handler) {
    router_->add_exception_handler(prefix_, exception_handler_type(std::forward<Handler>(handler)));
    return *this;
  }

  [[nodiscard]] route_group group(std::string_view prefix) const;
  [[nodiscard]] route_resource resource(std::string_view pattern) const;

  route_group& mount(std::string_view prefix, router&& mounted) {
    router_->mount(route_pattern(prefix), std::move(mounted));
    return *this;
  }

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

  template<class BodyPolicy, class Handler>
  route_group& route(
    method method_value,
    std::string_view pattern,
    route_options options,
    BodyPolicy policy,
    Handler&& handler) {
    router_->route(method_value, route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& get(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->get(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& post(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->post(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& put(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->put(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& patch(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->patch(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& delete_(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->delete_(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& head(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->head(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

  template<class BodyPolicy, class Handler>
  route_group& options(std::string_view pattern, route_options options, BodyPolicy policy, Handler&& handler) {
    router_->options(route_pattern(pattern), options, policy, std::forward<Handler>(handler));
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

inline std::string_view trim_http_ows(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

inline bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto left = static_cast<unsigned char>(lhs[index]);
    const auto right = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

inline bool ascii_iends_with(std::string_view value, std::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         ascii_iequals(value.substr(value.size() - suffix.size()), suffix);
}

inline bool is_json_content_type(std::string_view content_type) noexcept {
  const auto parameter_offset = content_type.find(';');
  auto media_type = trim_http_ows(content_type.substr(0, parameter_offset));
  if (media_type.empty()) {
    return false;
  }

  if (ascii_iequals(media_type, "application/json")) {
    return true;
  }

  const auto slash_offset = media_type.find('/');
  return slash_offset != std::string_view::npos &&
         slash_offset + 1 < media_type.size() &&
         ascii_iends_with(media_type.substr(slash_offset + 1), "+json");
}

template<class T, class Handler>
router::handler_type wrap_json_handler(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  using decoded_type = std::decay_t<T>;
  static_assert(
    std::is_invocable_v<stored_handler&, request&, response&, const decoded_type&>,
    "HTTP JSON route handler must accept (request&, response&, const T&) or a compatible value parameter");

  return [handler = stored_handler(std::forward<Handler>(handler))](
           request& req,
           response& res,
           std::span<const std::byte> body,
           request_body_stream*) mutable {
    if (!is_json_content_type(req.header("content-type"))) {
      res.status(status::unsupported_media_type).text("unsupported media type\n");
      return;
    }

    uvp::json parsed;
    try {
      const auto text = std::string_view{
        reinterpret_cast<const char*>(body.data()),
        body.size(),
      };
      parsed = uvp::json::parse(text.begin(), text.end());
    } catch (const uvp::json::parse_error&) {
      res.status(status::bad_request).text("invalid json\n");
      return;
    }

    if constexpr (std::is_same_v<decoded_type, uvp::json>) {
      handler(req, res, parsed);
    } else {
      std::optional<decoded_type> decoded;
      try {
        decoded.emplace(parsed.get<decoded_type>());
      } catch (const uvp::json::exception&) {
        res.status(status::unprocessable_content).text("invalid json body\n");
        return;
      }
      handler(req, res, *decoded);
    }
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

inline http::status multipart_error_status(const uvp::error& error) noexcept {
  if (error.code == make_error_code(errc::unsupported_media_type)) {
    return status::unsupported_media_type;
  }
  if (error.code == make_error_code(errc::multipart_limit_exceeded)) {
    return status::payload_too_large;
  }
  return status::bad_request;
}

template<class Handler>
router::handler_type wrap_multipart_stream_handler(Handler&& handler, multipart_stream_options options) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler)), options = std::move(options)](
           request& req,
           response& res,
           std::span<const std::byte>,
           request_body_stream* body) mutable {
    http::multipart_stream multipart{*body, req.header("content-type"), options};
    if (!multipart.valid()) {
      res.status(multipart_error_status(multipart.error())).text(multipart.error().detail + "\n");
      return;
    }

    handler(req, res, multipart);
    if (!multipart.has_error_handler() && !res.ended() && !res.deferred() && !res.streaming()) {
      res.status(status::internal_server_error).text("multipart on_error handler required\n");
    }
  };
}

template<class Handler>
router::handler_type wrap_multipart_form_handler(Handler&& handler, multipart_form_options options) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler)), options = std::move(options)](
           request& req,
           response& res,
           std::span<const std::byte> body,
           request_body_stream*) mutable {
    auto form = parse_multipart_form(req.header("content-type"), body, options);
    if (!form) {
      res.status(multipart_error_status(form.error())).text(form.error().detail + "\n");
      return;
    }

    handler(req, res, form.value());
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
      "(request&, response&, request_body_stream&). "
      "Use an explicit body policy for JSON and multipart routes.");
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

template<class Handler>
response_hook_type wrap_response_hook(Handler&& handler) {
  using stored_handler = std::decay_t<Handler>;
  return [handler = stored_handler(std::forward<Handler>(handler))](const response_info& info) mutable {
    if constexpr (std::is_invocable_r_v<void, stored_handler&, const response_info&>) {
      handler(info);
    } else {
      static_assert(std::is_invocable_r_v<void, stored_handler&, const response_info&>,
        "HTTP response hook must accept (const response_info&) and return void");
    }
  };
}

} // namespace detail

} // namespace uvp::http
