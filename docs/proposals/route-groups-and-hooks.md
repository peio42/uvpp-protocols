# Route Groups and Hooks Proposal

Status: Partially implemented

## Context

The HTTP router uses a segment trie and already supports route parameters,
wildcards, method-aware matching, `not_found`, and `on_error`. The design notes
reserve future route ergonomics for route groups and hooks attached to groups or
subtrees.

This proposal keeps that work aligned with the library's current shape:

- route declarations remain explicit and readable;
- request body policies remain part of each route contract;
- hooks use typed protocol objects rather than raw parser or libuv state;
- advanced behavior is visible at the call site instead of hidden in a global
  application framework.

## Current State

- Implemented: route parameters, wildcard tails, method-aware matching,
  `not_found`, `on_error`, route groups, shared prefixes, and inherited
  `on_request`/`pre_handler`/`on_response` hooks, plus `resource()` route
  builders for exact multi-method endpoints.
- Not implemented: route-level hooks separate from group hooks, mountable
  routers, resource route builders, scoped fallbacks, parameter constraints,
  and matched route pattern accessors.

## Draft Scope

- Define route group construction and prefix behavior.
- Decide how group hooks relate to global middleware.
- Preserve route matching priority and validation rules.
- Keep route registration readable for small services.
- Identify adjacent routing ergonomics worth borrowing from established HTTP
  routers without turning the HTTP module into an application framework.

## Out Of Scope

- Full application framework.
- Dependency injection.
- Authorization policy language.
- Regex-heavy route languages and optional segment grammars.
- Plugin systems for application-level concerns.

## Proposed Direction

Route groups should start as lightweight registration views over an owning
`server` or `router`. A group has a prefix and optional group-scoped hooks or
fallbacks. It does not own accepted connections, parser state, response slots,
or transport state.

```cpp
auto api = srv.group("/api/v1");

api.get("/health", [](uvp::http::request&, uvp::http::response& res) {
  res.text("ok\n");
});

api.post(
  "/items",
  uvp::http::body::text{},
  [](uvp::http::request&, uvp::http::response& res, std::string_view body) {
    res.status(uvp::http::status::created).text(body);
  });
```

Nested groups concatenate normalized path segments:

```cpp
auto api = srv.group("/api");
auto v1 = api.group("/v1");
v1.get("/items/:id", show_item);
```

The registered route is `/api/v1/items/:id`; the trie matching priority stays
unchanged: static segments, named parameters, then wildcard tail segments.

## Hook Model

The first hook set should be deliberately small:

- `on_request`: runs after headers are parsed and a route is matched, before
  request body buffering or streaming is allowed to continue.
- `pre_handler`: runs immediately before the final route handler. For buffered
  body policies this means after the body is available; for streaming policies
  this means after headers.
- `on_response`: observational hook after the response has been completed by
  application code, before serialization/write where practical.

Hook names should communicate lifecycle phase. A hook that can short-circuit
must return an explicit result instead of relying on implicit `next()` behavior.

```cpp
enum class hook_result {
  next,
  stop,
};

auto admin = srv.group("/admin");
admin.on_request([](uvp::http::request& req, uvp::http::response& res) {
  if (req.header("authorization").empty()) {
    res.status(uvp::http::status::unauthorized).text("unauthorized\n");
    return uvp::http::hook_result::stop;
  }
  return uvp::http::hook_result::next;
});
```

Request-side execution order should be root to leaf:

1. server-level `on_request` hooks;
2. parent group hooks;
3. child group hooks;
4. route-level hooks;
5. final route handler.

The initial implementation should avoid generic Express-style `next()` chains.
They are flexible, but they blur body policy timing, response ownership, and
deferred or streaming response behavior.

### `on_response` Behavior

`on_response` is an observational hook. It cannot mutate
the response status, headers, body, streaming state, or connection behavior.
Response mutation belongs in `on_request`, `pre_handler`, the final handler, or
a separately designed `before_response` hook if a real use case appears.

The hook receives a snapshot rather than `request&`. Buffered request
handlers keep the request alive only while the handler runs, and deferred or
streaming responses may complete after that stack frame is gone. Holding a
borrowed `request&` until response completion would make the lifetime model
surprising.

Implemented shape:

```cpp
enum class response_outcome {
  completed,
  cancelled,
  error,
};

struct request_snapshot {
  method method;
  std::string target;
  std::string path;
  std::string query;
  route_params params;
  connection_info connection;
};

struct response_info {
  const request_snapshot& request;
  unsigned int status = 200;
  const headers& response_headers;
  std::size_t response_body_size = 0;
  response_outcome outcome = response_outcome::completed;
};

using response_hook_type = std::function<void(const response_info&)>;
```

`request_snapshot` is captured after route matching and before the
request can be destroyed. It intentionally excludes request body bytes and raw
request headers at first; those can be added later only if there is a clear
logging or metrics need and the copying cost remains explicit.

`on_response` runs exactly once for every route handler that starts a
response lifecycle:

- buffered response: after application code completes the response, before
  serialization if practical;
- deferred response: when the deferred response completes;
- streaming response: when the stream ends, is cancelled, or fails;
- hook short-circuit response: after the short-circuit response completes;
- connection close before completion: once with `response_outcome::cancelled`
  or `response_outcome::error`, depending on whether the close is graceful or
  caused by an error.

Response hook execution uses leaf-to-root order. This mirrors unwinding:
the most specific route/group observes first, then parent groups, then global
server hooks. Request-side hooks keep root-to-leaf order because they prepare
the request before the handler; response-side hooks observe completion after
the handler.

Exceptions thrown by `on_response` do not change the already completed
response. They also should not call application `on_error`, because that would
invite double responses or hook loops. The first implementation can swallow
them; a later diagnostics hook may expose hook failures if needed.

`on_response` is different from a possible `on_finish` hook. `on_response`
observes application completion. `on_finish` would observe transport completion
after bytes are written or the connection closes. This proposal only defines
`on_response`; transport-finish metrics can be proposed separately.

## Borrowed Router Ideas

The following ideas are worth importing in priority order, adapted to
`uvpp-protocols` conventions.

### 1. Resource Route Builder

Multiple methods for the same path should be declarable without repeating the
path:

```cpp
srv.resource("/items/:id")
  .get(show_item)
  .put(uvp::http::body::text{}, update_item)
  .delete_(delete_item);
```

This is registration sugar over existing method-specific routes. It should not
change matching behavior.

Status: implemented as `router::resource(...)`, `server::resource(...)`, and
`route_group::resource(...)`.

`resource()` is intentionally distinct from `group()`: a group describes a
subtree and can own hooks or nested paths, while a resource describes one exact
endpoint with several methods. For example:

```cpp
srv.group("/api/v1")
  .resource("/items/:id")
  .get(show_item)
  .put(uvp::http::body::text{}, update_item);
```

### Optional: Fluent Temporary Overloads

`server` already returns `server&` from route registration helpers, and
`route_group`/`route_resource` are lightweight value handles, so common fluent
chains are already supported:

```cpp
srv.group("/api/v1")
  .get("/items", list_items)
  .get("/items/:id", show_item);
```

If future users routinely assign from chained temporary groups, the API can add
ref-qualified overloads returning `route_group&&` or `route_resource&&` for
rvalue receivers. That would make fluent temporary assignment more explicit,
but it is optional polish rather than required for the current implementation.

### 2. Mountable Routers

Applications should be able to compose independently declared routers:

```cpp
uvp::http::router api;
api.get("/items", list_items);

srv.mount("/api/v1", std::move(api));
```

Mounting can either merge tries or replay route registrations with a prefix.
The first implementation should prefer the simpler approach unless route
metadata makes replay too lossy.

### 3. Lifecycle Hooks

Hooks should map to concrete HTTP server phases, not to a generic middleware
black box. `on_request`, `pre_handler`, and `on_response` cover the common
cases of authentication, request logging, and response metrics while keeping
body policy timing explicit.

### 4. Per-route Options

Route-level options should extend the existing body limit metadata:

```cpp
srv.post(
  "/upload",
  uvp::http::route_options{}
    .max_body_bytes(20 * 1024 * 1024)
    .request_timeout(std::chrono::seconds{30}),
  uvp::http::body::stream{},
  upload_handler);
```

This keeps operational concerns near the route declaration and avoids
server-wide limits that are either too strict for uploads or too loose for
small endpoints.

### 5. Lightweight Validation

Validation should start as explicit route metadata or body policy behavior, not
as a hidden application validation framework:

```cpp
srv.get(
  "/items/:id",
  uvp::http::param_constraints{}.uint_("id"),
  show_item);
```

Typed JSON validation belongs with the future `body::json<T>` policy, where the
body contract is already explicit.

### 6. Parameter Constraints

Common parameter constraints should be supported without introducing a regex
route language:

```cpp
srv.get("/items/:id", uvp::http::param_constraints{}.uint_("id"), show_item);
srv.get("/files/*path", uvp::http::param_constraints{}.non_empty("path"), serve_file);
```

Constraint failures should produce a predictable client error or fall through
according to a documented policy. Falling through can surprise users when two
routes differ only by constraints, so a direct `400` or `404` policy should be
chosen explicitly.

### 7. Scoped Fallbacks

Groups should be able to customize missing-route and error behavior for a
subtree:

```cpp
srv.group("/api")
  .not_found(api_not_found)
  .on_error(api_error);
```

The global `not_found` and `on_error` handlers remain the default fallback.
Scoped handlers apply only when the matched path is inside the group prefix.

### 8. Matched Route Pattern

`request` should expose the matched route pattern for logging and metrics:

```cpp
std::string_view pattern = req.matched_pattern(); // "/items/:id"
```

This avoids emitting high-cardinality metrics for concrete paths such as
`/items/1`, `/items/2`, and `/items/3`.

## Implementation Notes

Route groups with prefixes can be implemented without changing request
dispatch. They are mostly registration-time path normalization.

Hooks that run before body buffering need router support. The router should
return matched route metadata that includes the inherited hook chain and the
matched pattern. The server can then execute `on_request` before accepting a
large buffered body or wiring a streaming body callback.

The existing `route_target` already stores body mode, body limit, and handler.
Future route metadata can live beside those fields:

```cpp
struct route_target {
  detail::body_mode body;
  std::size_t max_body_bytes;
  route_options options;
  handler_type handler;
  std::string pattern;
};
```

Group hooks and scoped fallbacks can be stored on trie nodes. Match traversal
then accumulates the inherited chain from root to leaf without changing static,
parameter, and wildcard priority.

Upgrade routes currently use a separate upgrade route list. Groups should be
able to prefix `upgrade()` declarations, but request hooks should not
automatically apply to upgrades until an explicit `upgrade_request` hook model
exists.

## Compatibility

Existing route declarations should remain source-compatible. Services that do
not use groups, hooks, route options, or constraints should keep the same
runtime behavior.

The main observable changes should be additive:

- new registration helpers on `server`, `router`, and route group views;
- optional route metadata in match results;
- optional hook execution when configured;
- optional request accessors such as `matched_pattern()`.

## Source Documents

- [HTTP server design](../design/http-server.md)
