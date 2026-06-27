# Route Path Decoding Proposal

Status: Draft

## Context

The HTTP design currently says that route matching should operate on the
decoded path component. The implementation currently matches route patterns
against the raw path string, so encoded path segments such as `%20` remain
encoded for static route matching and route parameters.

The desired behavior needs one important constraint: percent-decoding must not
change the route segment structure. In particular, `%2F` must not become a path
separator before routing, otherwise `/files/a%2Fb` would be treated like
`/files/a/b`.

## Current State

- Implemented: router trie, static segments, named parameters, wildcard tails,
  method-aware `405`, `HEAD` fallback, automatic `OPTIONS`, and upgrade route
  matching.
- Implemented: query parameters decode `%XX` escapes and treat `+` as a space.
- Not implemented: configurable route path decoding.
- Current behavior: route matching and captured route params use raw path
  segments.

## Decision

Default route matching should use percent-decoded path segments:

1. split the raw path on literal `/`;
2. percent-decode each segment independently;
3. match route pattern segments against the decoded request segments;
4. return decoded values from `req.params()`;
5. never let an encoded slash create a new route segment.

Applications that need strict raw matching should be able to opt into raw mode.

The default should be `percent_decoded_segments`, because that is the expected
behavior for application routing and it matches the direction already described
by the design documentation.

## Proposed API

Expose a small route path matching mode:

```cpp
namespace uvp::http {

enum class route_path_matching {
  raw,
  percent_decoded_segments,
};

} // namespace uvp::http
```

Expose it through server options:

```cpp
uvp::http::server srv(
  loop,
  uvp::http::server_options{}
    .route_path_matching(uvp::http::route_path_matching::percent_decoded_segments));
```

The default option value should be
`route_path_matching::percent_decoded_segments`.

The public `router` should also know its matching mode, because `router::match`
and `router::allowed_methods` are public APIs:

```cpp
uvp::http::router router{
  uvp::http::route_path_matching::percent_decoded_segments,
};
```

Exact names may still be adjusted during implementation, but the API should
keep the choice explicit and avoid boolean options.

## User-Facing Behavior

With the default mode:

- route pattern `/users/:id` matches request path `/users/alice%20smith`;
- `req.params().get("id")` returns `alice smith`;
- route pattern `/files/:name` matches request path `/files/a%2Fb`;
- `req.params().get("name")` returns `a/b`;
- route pattern `/files/:name` does not treat `/files/a%2Fb` as
  `/files/a/b`;
- `+` remains a literal plus sign in path segments;
- invalid percent escapes are kept literally instead of rejecting the request.

In raw mode:

- route matching uses the raw path segments;
- captured route parameters keep raw `%XX` sequences;
- this is useful for proxy-like code, signature-sensitive routes, and
  applications that want to interpret path encoding themselves.

`request::target()` remains the raw HTTP request target. `request::path()`
should also remain the raw path component for now, to avoid mixing routing
normalization with request inspection. A decoded path accessor can be proposed
later if user code needs one.

## Implementation Plan

1. Add `route_path_matching` to the public HTTP API.
2. Add `server_options::route_path_matching(...)` and default it to
   `percent_decoded_segments`.
3. Add a `router` constructor or option that stores the matching mode.
4. Normalize route pattern segments according to the router mode when routes
   are registered.
5. Normalize request path segments according to the router mode when matching.
6. Keep percent-decoding segment-local:
   - split on literal `/` first;
   - decode `%XX` inside each segment;
   - do not treat `+` specially;
   - keep invalid percent escapes literally.
7. Apply the same matching mode to upgrade routes by extending
   `detail::route_pattern_matches`.
8. Pass the server option into both the normal router and upgrade matching.
9. Update user and design documentation.

## Tests

Add router-level tests for:

- static route matching with encoded spaces;
- parameter capture with encoded spaces;
- wildcard capture with encoded spaces;
- encoded slash inside a parameter;
- encoded slash inside a wildcard tail;
- literal `+` in path segments;
- invalid percent escape preserved literally;
- raw mode preserving current behavior;
- `allowed_methods` using the configured matching mode.

Add integration tests for:

- a normal HTTP route receiving a decoded parameter;
- a route with `%2F` inside a parameter not changing segment boundaries;
- `HEAD` fallback and automatic `OPTIONS` with decoded segment matching.

If practical, add one WebSocket/upgrade route test to verify that upgrade route
matching uses the same path decoding mode as regular routes.

## Out Of Scope

- URL normalization beyond percent-decoding path segments.
- Dot-segment resolution such as `.` and `..`.
- Unicode normalization.
- Treating `+` as a space in paths.
- Rejecting malformed percent escapes.
- Changing `request::path()` to return a decoded path.

## Documentation Updates

Update:

- [HTTP server user documentation](../user/http-server.md)
- [HTTP server design](../design/http-server.md)

The documentation should state the default, the raw opt-in mode, and the fact
that encoded slashes do not create route segments.

## Source Documents

- [Roadmap](../roadmap.md)
- [HTTP server design](../design/http-server.md)
- [HTTP server user documentation](../user/http-server.md)
