# Route-Level Hooks Proposal

Status: Optional reflection

## Context

Route groups and server-level hooks are implemented. They cover the common
cases where a whole subtree needs authentication, request logging, preparation,
or response metrics.

The archived route groups proposal mentioned route-level hooks in the generic
execution order, but the implemented API deliberately stopped at server and
group scopes. A route-level hook would attach only to one method/pattern pair.

## Question

Should individual route declarations support hooks that run between inherited
group hooks and the final handler?

Possible shape:

```cpp
srv.get(
  "/items/:id",
  uvp::http::route_options{}
    .on_request(check_item_access)
    .pre_handler(load_item_context),
  show_item);
```

## Current Leaning

Do not implement yet.

The value is real but modest. Applications can already express route-specific
behavior directly in the handler or by extracting a small helper function. For
cross-cutting behavior, groups remain clearer and keep route declarations
compact.

If this becomes useful later, prefer extending `route_options` rather than
adding more positional overloads to every verb, group, resource, and server
entry point.

## Design Constraints

- Route-level hooks must not change matching priority.
- Request-side hooks should run after inherited server/group hooks and before
  the handler.
- Response observers, if supported per route, should run before group and
  server observers in the existing leaf-to-root order.
- Hook storage should stay in route metadata beside body mode, body limit,
  handler, and matched pattern.

## Source Documents

- [Archived route groups and hooks](../archive/route-groups-and-hooks.md)
