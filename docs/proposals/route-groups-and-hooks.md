# Route Groups and Hooks Proposal

Status: Draft

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
  `not_found`, and `on_error`.
- Not implemented: route groups, shared prefixes, group-level hooks, and
  subtree-level hooks.

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

Execution order should be root to leaf:

1. server-level `on_request` hooks;
2. parent group hooks;
3. child group hooks;
4. route-level hooks;
5. final route handler;
6. observational response hooks, leaf to root or root to leaf only after the
   order has a documented use case.

The initial implementation should avoid generic Express-style `next()` chains.
They are flexible, but they blur body policy timing, response ownership, and
deferred or streaming response behavior.

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
