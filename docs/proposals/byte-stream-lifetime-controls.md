# Byte Stream Lifetime Controls Proposal

Status: Proposed for Milestone 6

## Context

`uvpp` already exposes libuv handle reference controls through its handle layer:
handles can be `ref()`'d, `unref()`'d, and queried with `has_ref()`. The missing
piece for `uvpp-protocols` is that `uvp::io::byte_stream` type-erases TCP, pipe,
TLS, WebSocket, and future transport adapters without preserving those lifetime
controls.

That becomes visible in the HTTP client connection pool. An idle keep-alive
connection is still an open libuv handle. If it remains referenced, `loop.run()`
continues waiting for it even though the application has no active request. If
the HTTP pool closes all idle connections eagerly to avoid that, pooling becomes
less useful and less representative of normal client behavior.

The transport abstraction needs a small, explicit way to forward libuv
reference semantics without exposing concrete TCP, TLS, or WebSocket internals.

## Goals

- Let protocol owners mark idle transports as non-blocking for loop liveness.
- Keep the public API transport-shaped rather than TCP-specific.
- Preserve composition through TCP, pipe, TLS, WebSocket byte streams, and later
  protocol adapters.
- Keep shutdown ownership explicit: `unref()` must not close a stream.
- Allow HTTP keep-alive pooling to stay opt-in while behaving cleanly when
  enabled.
- Avoid introducing a generic native-handle escape hatch just for reference
  management.

## Non-Goals

- Replacing uvpp's handle model.
- Adding reference counting to protocol objects.
- Making `uv_loop_close()` succeed while open idle handles still exist.
- Hiding connection-pool shutdown from the HTTP client API.
- Exposing OpenSSL, TCP, pipe, or WebSocket implementation details through
  `byte_stream`.

## Proposed Public API

Add handle-liveness controls to `uvp::io::byte_stream`:

```cpp
namespace uvp::io {

class byte_stream {
public:
  void ref();
  void unref();
  [[nodiscard]] bool has_ref() const;
};

}
```

Semantics should mirror uvpp/libuv handle references:

- `ref()` asks the loop to keep running while this stream is open;
- `unref()` allows the loop to stop if no other referenced handles or requests
  remain;
- `has_ref()` reports the current reference state when the underlying transport
  can expose it;
- calling these functions on an invalid `byte_stream` is either a no-op for
  `ref()`/`unref()` and `false` for `has_ref()`, or follows the existing invalid
  stream error policy if the surrounding IO API already chooses stricter
  behavior.

`unref()` is not a close operation. An idle stream that has been unreferenced is
still open and must still be closed explicitly by its owner before loop teardown.

## Implementation Direction

Extend the `byte_stream` type-erased concept with:

- `ref()`;
- `unref()`;
- `has_ref()`.

Concrete adapters should implement them as follows:

- TCP and pipe byte streams forward directly to the underlying uvpp handle.
- TLS byte streams forward to the lower `byte_stream` because the lower stream
  owns the real libuv handle that affects loop liveness.
- WebSocket byte-stream adapters forward to the underlying transport/session
  owner when converted into a byte stream.
- Test and in-memory adapters may keep an internal boolean if no libuv handle is
  present.

If a future adapter owns multiple libuv handles, `ref()` and `unref()` should
apply to the handles that determine whether the stream keeps the loop alive.
For example, a transport with an internal timer may leave that timer referenced
if the timer is responsible for closing the idle stream after a bounded timeout.

## HTTP Client Pool Usage

The HTTP/1.1 connection pool should use these controls when pooling is enabled:

- on release to the idle pool, call `stream.unref()`;
- on checkout from the pool, call `stream.ref()` before reusing the connection;
- on explicit pool shutdown, close every idle stream;
- on idle timeout, close the stream even if it is unreferenced.

Timer policy should remain deliberate:

- keeping the idle timeout timer referenced lets `loop.run()` wait until the
  pool naturally closes idle connections;
- unreferencing both the stream and the timer lets `loop.run()` return
  immediately, but requires explicit `client.close_idle_connections()` before
  `loop.close()`.

The first implementation should prefer predictable tests and shutdown behavior
over clever automatic detachment. If needed, a later client option can decide
whether idle timeout timers are referenced or unreferenced.

## API Impact

This proposal affects the IO foundation, not only HTTP:

- HTTP client keep-alive can reuse connections without accidentally extending
  the active lifetime of an otherwise idle loop.
- TLS remains a transparent transport adapter because liveness controls pass
  through the encrypted stream to the lower byte stream.
- WebSocket client and future protocol clients get the same pooling or idle
  session machinery without depending on TCP internals.
- Server-side accepted streams can expose the same controls for advanced
  protocols, though ordinary HTTP server sessions should continue to own their
  active session lifetime explicitly.

## Tests

Add focused coverage for:

- `byte_stream` TCP adapter forwards `ref()`, `unref()`, and `has_ref()`;
- TLS byte stream forwards reference state to its lower stream;
- HTTP pool unreferences idle streams and references them again on reuse;
- explicit `client.close_idle_connections()` closes unreferenced idle streams;
- idle timeout closes pooled streams without double-completion or leaked handles.

Where direct loop-liveness assertions are brittle, tests should inspect
`has_ref()` through controlled adapters or verify that reuse and close callbacks
still behave exactly once.

## Open Questions

- Should invalid `byte_stream::ref()` and `unref()` be no-ops, or should they
  assert/fail consistently with other invalid transport operations?
- Should HTTP expose a client option for referenced vs unreferenced idle timeout
  timers?
- Do listener abstractions need the same controls now, or should listener
  `ref()`/`unref()` wait for a concrete server-side use case?

## Source Documents

- [HTTP client](http-client.md)
- [DNS resolution](dns-resolution.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
- [ADR 0001](../adr/0001-explicit-uvpp-loop-and-visible-ownership.md)
