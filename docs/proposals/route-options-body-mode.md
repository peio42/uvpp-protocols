# Route Options Body Mode Proposal

Status: Reflection

## Context

HTTP routes currently express request body behavior with two adjacent concepts:

- `body::*` policies describe how the request body is consumed:
  `none`, `bytes`, `text`, or `stream`;
- `route_options` carries route-level operational settings such as
  `max_body_bytes`.

After moving body limits out of `body::*`, the remaining question is whether the
body consumption mode itself should also move into `route_options`.

The possible direction would be:

```cpp
srv.post(
  "/upload",
  uvp::http::route_options{}
    .max_body_bytes(20 * 1024 * 1024)
    .body(uvp::http::body::stream{}),
  upload_handler);
```

instead of:

```cpp
srv.post(
  "/upload",
  uvp::http::route_options{}.max_body_bytes(20 * 1024 * 1024),
  uvp::http::body::stream{},
  upload_handler);
```

## Design Question

Should `route_options` become the single route contract object, carrying both
operational settings and the request body consumption mode?

This would make route declarations read as:

1. HTTP method;
2. route path;
3. route options;
4. handler.

That shape is attractive because every route-level concern is grouped in one
value object.

## Important Constraint

The body mode is not only runtime metadata. It currently drives compile-time
handler wrapping:

- `body::none{}` expects `(request&, response&)`;
- `body::bytes{}` expects `(request&, response&, std::span<const std::byte>)`;
- `body::text{}` expects `(request&, response&, std::string_view)`;
- `body::stream{}` expects `(request&, response&, request_body_stream&)`.

A route options design must preserve these static checks. A purely runtime
field such as `route_options::body_mode(detail::body_mode::text)` would weaken
the API because mismatches between options and handler signatures would become
runtime errors or confusing template failures.

## Possible Shape

The safer direction is a typed route-options builder:

```cpp
auto options = uvp::http::route_options{}
  .max_body_bytes(64 * 1024)
  .body(uvp::http::body::text{});

srv.post("/message", options, update_message);
```

Conceptually, `.body(...)` would return a type that carries the body policy in
its type:

```cpp
route_options<body::text>
```

The exact public type name does not need to be exposed, but the route overloads
would need to receive enough static type information to select the right
handler wrapper.

## Interaction With Inference

The current concise form should remain possible:

```cpp
srv.post("/message", [](uvp::http::request&, uvp::http::response&, std::string_view) {});
```

That declaration can still infer `body::text{}` from the handler signature. The
open question is how inference combines with route options:

```cpp
srv.post(
  "/message",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  [](uvp::http::request&, uvp::http::response&, std::string_view) {});
```

This could continue to mean: infer the body mode from the handler, and apply
the route-level options.

If the user provides an explicit body in the options, the handler signature must
match that body mode.

## Benefits

- Route-level configuration has one obvious home.
- Body limits, future body timeouts, and body consumption mode appear together.
- The public route overload set may become smaller once the typed builder is in
  place.
- Documentation can describe a single route options object instead of a
  separate options-plus-body-policy pair.

## Costs And Risks

- `route_options` becomes more template-heavy.
- Error messages may become harder to keep friendly if the body mode is encoded
  in builder types.
- The implementation may become more complex than the current explicit
  `route_options, body_policy, handler` overloads.
- A runtime-only implementation would be simpler but should be avoided because
  it would weaken static handler validation.
- The value category and lifetime of typed route options need care. Reusing a
  named options object should remain ergonomic.

## Migration Sketch

If adopted, the migration could happen in small steps:

1. Keep the current API working while adding typed `.body(...)` route options.
2. Teach `router`, `server`, `route_group`, and `route_resource` overloads to
   accept typed route options.
3. Update docs to prefer `.body(...)` inside `route_options`.
4. Decide whether the separate `route_options, body_policy, handler` overloads
   should remain as convenience syntax or be removed before API stabilization.

## Recommendation

Keep this as an API reflection item for now.

The concept is coherent, but the implementation should not use runtime body
mode flags. It is worth revisiting once `route_options` has more than one or
two route-level settings, especially after timeout enforcement clarifies the
future body/request timeout vocabulary.
