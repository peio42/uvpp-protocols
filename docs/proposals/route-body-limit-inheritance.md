# Route Body Limit Inheritance Proposal

Status: Reflection

## Context

`server_options::max_body_bytes(...)` is the effective default request body
limit for the HTTP server. That value must be greater than zero because it is
used directly when a route does not override the limit.

`route_options::max_body_bytes(...)` currently stores a `std::size_t`, and the
default value `0` means "inherit `server_options::max_body_bytes()`". The same
sentinel is carried through `router::match_result` and resolved by the server
connection before enforcing the body limit.

## Problem

The sentinel is internal but visible through the public `route_options`
accessor. That makes `route_options{}.max_body_bytes()` return `0`, and an
explicit call to `.max_body_bytes(0)` is indistinguishable from leaving the route
unset.

That weakens validation symmetry:

- server-level `max_body_bytes(0)` is invalid;
- route-level `max_body_bytes(0)` currently means inherit;
- a route that accepts no request body should use `body::none{}`, not a zero
  body limit.

## Direction

Replace the `0` sentinel with an explicit unset state, likely
`std::optional<std::size_t>`, in the internal route metadata.

Possible public shape:

```cpp
struct route_options {
  route_options& max_body_bytes(std::size_t value) &;
  route_options&& max_body_bytes(std::size_t value) &&;

  [[nodiscard]] std::optional<std::size_t> max_body_bytes() const noexcept;
};
```

With that model:

- default-constructed `route_options` has no route-level body limit override;
- `route_options::max_body_bytes(0)` throws `std::invalid_argument`;
- the server falls back to `server_options::max_body_bytes()` when the optional
  value is empty;
- `body::none{}` remains the way to declare routes that do not consume a body.

## Open Questions

- Should the public getter return `std::optional<std::size_t>`, or should the
  unset state remain internal and expose a separate `has_max_body_bytes()`?
- Should `router::match_result` expose the optional value, or should it carry the
  already-resolved effective limit?
- Should `body_timeout` use the same optional representation so route-level
  inheritance is modeled consistently?

## Migration Sketch

1. Change internal route metadata from `std::size_t` sentinel fields to explicit
   optional fields.
2. Keep route matching behavior unchanged: unset route limits inherit the server
   defaults.
3. Make `route_options::max_body_bytes(0)` throw once no public behavior depends
   on using `0` as an inherited value.
4. Update tests to cover default inheritance, explicit route overrides, and
   rejection of zero route-level limits.

## Related Audit

This follows the code-quality audit finding about `server_options::max_body_bytes`
validation. The immediate server-level issue is resolved; this proposal tracks
whether the route-level sentinel should be replaced by an explicit unset state.
