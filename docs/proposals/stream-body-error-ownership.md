# Stream Body Error Ownership Proposal

Status: Draft

## Context

`body::stream{}` dispatches the route handler after request headers are
received. From that point on, the application receives body chunks through
`request_body_stream` callbacks.

The server currently enforces `route_options::max_body_bytes(...)` and
`server_options::max_body_bytes()` while reading chunks. If the limit is
exceeded, it emits `request_body_stream::on_error(...)` and also sends an
automatic `413 Payload Too Large`.

Multipart streaming now uses a stricter ownership rule: once the handler has
received `multipart_stream&`, route and parser errors are delivered to
`mp.on_error()`, and the application owns the response.

## Problem

Plain `body::stream{}` has the same broad lifecycle shape as multipart
streaming: the handler has already received both `request_body_stream&` and
`response&` when the body limit can be exceeded.

That leaves two possible authorities for the response:

- the framework, which can synthesize `413 Payload Too Large`;
- the application, which can react from `body.on_error(...)`.

The current automatic `413` behavior is simple and useful, but it is not fully
aligned with the "handler owns the response after streaming dispatch" rule used
by multipart streaming.

## Options

### Option A: Keep Automatic 413

Treat `route_body_limit` as a low-level HTTP transport guard for
`body::stream{}`. The server keeps emitting `on_error(...)` and sending an
automatic `413`.

This is the current behavior. It is easy to reason about for generic streaming
routes that do not install robust error handlers, but it means `response&` is
not exclusively application-owned after handler entry.

### Option B: Application-Owned Streaming Errors

After a `body::stream{}` handler is entered, route and server body-limit errors
only call `request_body_stream::on_error(...)`. The application must complete
the response if it wants to send one.

This matches multipart streaming and avoids competing responses. It requires
applications to install `on_error(...)` whenever they need an application-level
error body or status.

## Recommendation

Keep the current `body::stream{}` behavior until there is a deliberate API
change. Prefer Option B for consistency, but land it with documentation,
integration tests, and a migration note because existing applications may rely
on automatic `413` responses.

## Tests Needed For Option B

- route body limit exceeded after a `body::stream{}` handler is entered calls
  `request_body_stream::on_error(...)`;
- the server does not synthesize a competing `413`;
- an application response sent from `on_error(...)` is delivered;
- if the application does not respond, the connection is closed or timed out in
  a documented way.

## Related

- [Multipart handling](multipart-handling.md)
- [Route body limit inheritance](route-body-limit-inheritance.md)
