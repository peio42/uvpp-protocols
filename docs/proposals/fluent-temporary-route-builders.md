# Fluent Temporary Route Builders Proposal

Status: Draft

## Context

`uvp::http::server` already returns `server&` from route registration helpers,
so ordinary server-level chaining works:

```cpp
srv
  .get("/items", list_items)
  .get("/items/:id", show_item);
```

`route_group` and `route_resource` are lightweight value handles over an owning
router. They also support fluent chaining for common one-shot declarations:

```cpp
srv.group("/api/v1")
  .get("/items", list_items)
  .get("/items/:id", show_item);

srv.resource("/items/:id")
  .get(show_item)
  .put(uvp::http::body::text{}, update_item);
```

The subtle case is assigning from a chain that starts with a temporary builder:

```cpp
auto api = srv.group("/api/v1")
  .get("/items", list_items);
```

Without rvalue-qualified overloads, methods such as `route_group::get(...)`
return `route_group&`. That means the expression returns an lvalue reference to
the temporary builder rather than preserving value-category through the chain.
Users can avoid the issue by naming the group first:

```cpp
auto api = srv.group("/api/v1");
api.get("/items", list_items);
```

This proposal tracks whether the API should support the temporary-assignment
style directly.

## Current State

- Implemented: server-level fluent chaining.
- Implemented: one-shot fluent chaining on `route_group` and `route_resource`.
- Documented: users who want to keep a group for later should name it before
  chaining route declarations.
- Not implemented: rvalue-qualified overloads that preserve temporary builders
  through assignment.

## Proposed Direction

If user feedback shows that temporary-assignment chains are common, add
ref-qualified overloads for fluent builder methods:

```cpp
template<class Handler>
route_group& get(std::string_view pattern, Handler&& handler) &;

template<class Handler>
route_group&& get(std::string_view pattern, Handler&& handler) &&;
```

The lvalue overload keeps the current behavior:

```cpp
auto api = srv.group("/api/v1");
api.get("/items", list_items); // returns route_group&
```

The rvalue overload makes temporary assignment explicit:

```cpp
auto api = srv.group("/api/v1")
  .get("/items", list_items); // returns route_group&&, then moves into api
```

The same pattern would apply to `route_resource`.

## Implementation Notes

This is header-only API polish. It should not touch the router trie, HTTP
server dispatch, hooks, body policies, or connection lifecycle.

To avoid duplicating registration logic, each public overload can delegate to a
small private helper:

```cpp
template<class Handler>
void add_get(std::string_view pattern, Handler&& handler);

template<class Handler>
route_group& get(std::string_view pattern, Handler&& handler) & {
  add_get(pattern, std::forward<Handler>(handler));
  return *this;
}

template<class Handler>
route_group&& get(std::string_view pattern, Handler&& handler) && {
  add_get(pattern, std::forward<Handler>(handler));
  return std::move(*this);
}
```

The real cost is API surface area: every fluent method with both body-policy
and inferred-body overloads may need `&` and `&&` forms. This affects:

- `route_group`: route verbs, `route`, hooks, `group`, and `resource`;
- `route_resource`: route verbs and `route`.

## Out Of Scope

- Changing route matching behavior.
- Changing ownership of `server`, `router`, `route_group`, or
  `route_resource`.
- Adding new route declaration concepts beyond preserving fluent temporary
  value category.

## Recommendation

Keep this as optional polish until there is evidence that users need to assign
from temporary builder chains. The current two-line style is clear and safe:

```cpp
auto api = srv.group("/api/v1");
api.get("/items", list_items);
```

## Source Documents

- [Route groups and hooks](../archive/route-groups-and-hooks.md)
- [HTTP server user documentation](../user/http-server.md)
