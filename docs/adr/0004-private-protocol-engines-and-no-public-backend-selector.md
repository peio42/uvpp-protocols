# ADR 0004: Private Protocol Engines and No Public Backend Selector

Status: Accepted

Date: 2026-06-27

## Context

Protocol parsing and framing are security-sensitive and full of edge cases.
Where mature engines exist, `uvpp-protocols` should spend its complexity budget
on uvpp integration, ownership, API shape, and backpressure rather than
reimplementing every protocol state machine from scratch.

At the same time, third-party libraries should not dictate the public C++ API
or own uvpp sockets, timers, buffers, and session lifetimes.

## Decision

External protocol engines stay behind module-private adapters or `detail/`
implementation boundaries. Public APIs use uvpp-protocols and uvpp types, not
third-party types.

The HTTP/1.1 parser is `llhttp`. It is integrated as a private synchronous state
machine under the HTTP implementation. It does not own sockets, the uvpp loop,
timers, output buffers, response objects, or session lifetime.

Do not expose a public or build-level backend selector until the project has at
least two real backends that it is willing to support and test.

## Consequences

- CMake may decide how `llhttp` is supplied, but not advertise HTTP/1 parser
  choice as a product feature.
- HTTP/2, HTTP/3, TLS, database client adapters, and other future engines
  should follow the same boundary unless a module intentionally exposes an
  adapter API.
- Public headers remain insulated from backend churn.
- Backend replacement remains possible later, but only when it is backed by a
  real support and testing commitment.

## References

- [Dependency decisions](../design/dependency-decisions.md)
- [HTTP server design](../design/http-server.md)
- [API principles](../design/api-principles.md)
