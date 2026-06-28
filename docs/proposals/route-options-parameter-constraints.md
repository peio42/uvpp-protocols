# Route Options Parameter Constraints Proposal

Status: Optional reflection

## Context

Route parameters and wildcard captures are already available through
`request::params()`. Applications can validate them today inside handlers or
hooks.

Some HTTP routers provide built-in parameter constraints such as numeric IDs,
non-empty wildcard tails, enum-like values, or custom predicates. This can make
common validation more declarative:

```cpp
srv.get("/items/:id", show_item);
```

could become:

```cpp
srv.get(
  "/items/:id",
  uvp::http::route_options{}
    .param("id", uvp::http::param::uint_{}),
  show_item);
```

However, the current library is intentionally small and already has hooks for
authentication and request validation. The added ergonomics may not justify
extra route registration overloads or deeper router complexity yet.

## Design Direction

If implemented, parameter constraints should live in `route_options`, not as a
new standalone positional argument.

This keeps route declarations from multiplying overloads across:

- `router`;
- `server`;
- `route_group`;
- `route_resource`;
- every HTTP method convenience function.

The route shape remains:

```cpp
srv.get(path, route_options, handler);
srv.post(path, route_options, body_policy, handler);
```

and future route-level metadata can share the same options object.

## Possible API

One possible fluent shape:

```cpp
srv.get(
  "/items/:id",
  uvp::http::route_options{}
    .param("id", uvp::http::param::uint_{})
    .param("slug", uvp::http::param::non_empty{}),
  show_item);
```

Another shape would group constraints explicitly:

```cpp
srv.get(
  "/items/:id",
  uvp::http::route_options{}
    .params(uvp::http::param_constraints{}.uint_("id")),
  show_item);
```

The exact spelling can wait. The important point is that constraints are route
metadata, not a new route registration axis.

## Matching Semantics

The lowest-cost design is:

- the router matches paths exactly as it does today;
- after a route is matched, constraints validate the captured params;
- if validation fails, the server returns a documented client error;
- the router does not continue searching for another candidate route.

This avoids turning constraints into part of the trie matching algorithm.

Open decision: the response should probably be either:

- `400 Bad Request`, because the route matched but a parameter is invalid; or
- `404 Not Found`, if the library wants invalid params to look like no
  resource exists.

`400` is more explicit and easier to debug. `404` can be useful for public
resource IDs but hides the reason.

## Avoided Design

Avoid this model:

```cpp
srv.get("/items/:id", uvp::http::param_constraints{}.uint_("id"), show_item);
```

It looks simple for one route, but it creates another optional positional
parameter that must be threaded through every method helper and route builder.
It also competes with `route_options`, which is already the route-level
extension point.

Also avoid constraint-driven fallback matching in a first implementation. If
constraint failure makes the router continue searching, then matching priority,
`allowed_methods()`, overlapping routes, and diagnostics all become more
complex.

## Implementation Impact

With `route_options`, the code impact should stay moderate:

- extend `route_options` with a constraint container;
- store constraints in `router::route_target`;
- copy or move constraints through mounted routers;
- expose them in `router::match_result`;
- validate `match_result.params` in the server after route matching and before
  `on_request` hooks;
- add tests for accepted params, rejected params, group/resource routes, and
  mounted routers.

The router trie would not need structural changes if constraint failure is a
request rejection rather than a matching fallback.

## Relationship With Hooks

This feature is optional because hooks already cover many validation use cases:

```cpp
api.on_request([](uvp::http::request& req, uvp::http::response& res) {
  if (!is_uint(req.params().get("id"))) {
    res.status(uvp::http::status::bad_request).text("invalid id\n");
    return uvp::http::hook_result::stop;
  }
  return uvp::http::hook_result::next;
});
```

Built-in constraints would mainly improve discoverability and reduce repeated
small validators. They should not grow into a general validation framework.

## Recommendation

Keep this as optional HTTP API polish.

The likely implementation cost is reasonable if constraints are stored in
`route_options` and evaluated after matching. The current user value is modest,
so it should wait until repeated real examples show that hook-based validation
is too noisy.
