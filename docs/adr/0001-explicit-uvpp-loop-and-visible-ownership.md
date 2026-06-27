# ADR 0001: Explicit uvpp Loop and Visible Ownership

Status: Accepted

Date: 2026-06-27

## Context

`uvpp-protocols` is a higher-level protocol layer built on top of uvpp. It
should help applications avoid reimplementing protocol machinery, but it should
not hide uvpp's event-loop and lifetime model behind framework-style global
state.

libuv and uvpp APIs are explicitly asynchronous. Handles, callbacks, buffers,
timers, and close operations have lifetimes that must remain understandable to
the caller.

## Decision

Protocol owners are constructed from an explicit `uv::loop` or from an explicit
stream-like transport. The project does not provide a hidden default loop.

Asynchronous ownership must be visible in the type, the function name, or the
module documentation. Protocol modules may own higher-level state when that is
the point of the abstraction, such as accepted sessions, parsers, write queues,
request body buffers, and timers.

APIs that borrow data must document the lifetime of the borrowed view. APIs that
retain data must copy it, move it, or clearly document the caller's lifetime
obligation.

## Consequences

- Simple examples still show the loop directly, for example
  `uvp::http::server srv(loop)`.
- Protocol modules may be more explicit than a web framework, but behavior is
  easier to reason about during shutdown, cancellation, and callback execution.
- Higher-level session objects are allowed, as long as they do not pretend that
  asynchronous state is local or synchronous.
- Future modules should avoid APIs that appear synchronous while secretly
  blocking or retaining borrowed state.

## References

- [Project scope](../design/project-scope.md)
- [API principles](../design/api-principles.md)
- [Design overview](../design/index.md)
