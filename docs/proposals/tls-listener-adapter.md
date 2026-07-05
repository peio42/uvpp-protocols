# TLS Listener Adapter Proposal

Status: Implemented for generic listener composition

## Decision

`uvp::tls::listener` should be a generic adapter from one
`uvp::io::stream_listener` to another. It should not be an HTTP feature and it
should not own a special "TLS socket" abstraction.

The adapter accepts lower byte streams, performs a server-side TLS handshake on
each accepted stream, and emits only decrypted `uvp::io::byte_stream` instances
to the upper accept callback.

```text
io::stream_listener
  -> tls::listener
    -> io::stream_listener
      -> HTTP, SMTP, MQTT, custom protocol
```

This keeps `HTTP over TLS` equivalent to any other protocol over TLS. HTTP sees
the same clear byte stream shape it already consumes for plain TCP and Unix
sockets.

## Context

The core TLS proposal defines the lower-level operation:

```cpp
uvp::tls::accept(
  std::move(lower_stream),
  server_context,
  [](uvp::tls::handshake_result result) {
    if (!result) {
      return;
    }

    auto clear = std::move(result).stream();
  });
```

The listener adapter is a convenience around that operation:

```text
lower.listen(on_lower_accept)
  -> tls::accept(lower_stream, context, on_handshake)
    -> upper_on_accept(clear_stream)
```

It exists because server protocols usually want to listen on an already-secure
stream source without handling TLS handshakes manually for every connection.

## Current State

- Implemented: `uvp::tls::listener` converts a lower
  `uvp::io::stream_listener` into a secure `uvp::io::stream_listener`.
- Implemented: successful handshakes emit only clear `uvp::io::byte_stream`
  values to the upper accept callback.
- Implemented: handshake timeout, pending handshake limit, listener close while
  handshakes are pending, and lifetime-safe close of refused lower streams.
- Implemented: HTTP server accepts TLS listeners through its existing generic
  listener overload.
- Follow-up: server-side SNI context selection and client certificate policy
  are tracked in [TLS policy and identity](tls-policy-and-identity.md).
  Listener pause/resume backpressure, certificate reload, and HTTP convenience
  helpers remain separate later work.

## Public Shape

Suggested API:

```cpp
auto context = uvp::tls::server_context{}
  .certificate_chain_file("server.crt")
  .private_key_file("server.key")
  .alpn({"http/1.1"});

auto plain = uvp::io::tcp_listener{loop}
  .bind("0.0.0.0", 443);

uvp::io::stream_listener secure =
  uvp::tls::listener{
    std::move(plain),
    std::move(context),
    uvp::tls::listener_options{}
      .handshake_timeout(std::chrono::seconds{10})
      .max_pending_handshakes(1024)
  };

uvp::http::server http(loop);
http.listen(std::move(secure));
```

`tls::listener` should convert to `uvp::io::stream_listener` so upper protocols
use the existing `listen(io::stream_listener)` path.

## Listen Semantics

The TLS listener should call the upper accept callback only after the TLS
handshake succeeds.

This is the key behavior:

- lower listener accepts encrypted byte streams;
- TLS listener owns the handshake;
- upper listener callback receives clear byte streams;
- upper protocols never see partial TLS handshakes or ciphertext.

Handshake failure is per-connection failure. It must not stop the lower
listener unless the lower listener itself fails or is closed.

## Accept Errors

There are two classes of accept errors:

- lower listener errors, such as TCP accept failures;
- TLS handshake errors, such as malformed records, certificate policy failure,
  ALPN failure, timeout, or cancellation.

The TLS listener should surface handshake failures through the same
`io::accept_result` error path used by `io::stream_listener`, while preserving a
TLS error category or source code for diagnostics. This keeps the upper
listener contract uniform.

An upper protocol such as HTTP can then choose to log and ignore accept errors
without knowing whether they came from TCP or TLS.

## Ownership

`tls::listener` owns:

- the lower `io::stream_listener`;
- the server context shared by new handshakes;
- all pending TLS handshakes that have not yet emitted a clear stream or failed;
- listener-level timers and limits.

Once a handshake succeeds and a clear `io::byte_stream` is emitted, ownership
of that stream moves to the upper protocol. Closing the TLS listener should not
close already-emitted streams.

## Server Context Lifetime

The listener should own or share stable context state. Users should not have to
keep a referenced `server_context` alive beside the listener.

Recommended shape:

- `server_context` is movable and cheap to pass by value, likely through PIMPL
  or shared internal state;
- `tls::listener` stores its own copy/move of that context state;
- in-flight handshakes keep the context state alive until they finish.

This avoids dangling context references and makes listener construction match
other owning uvpp-protocols APIs.

## Pending Handshake Backpressure

The listener should have an explicit `max_pending_handshakes` option.

Without this, a peer can open many TCP connections and never finish TLS,
forcing the process to retain handshake objects, BIO buffers, timers, and lower
streams indefinitely.

When the limit is reached, possible policies are:

- close the newly accepted lower stream immediately;
- report an accept error and close it;
- temporarily pause lower accepts if the lower listener supports it later.

Initial recommendation: fail and close newly accepted streams once the limit is
reached. This is simple, deterministic, and does not require extending
`io::stream_listener` with pause/resume semantics in the first milestone.

## Handshake Timeout

The TLS listener should configure a handshake timeout.

If a client does not complete the TLS handshake before the timeout:

- cancel the pending handshake;
- close the lower stream;
- emit one accept error with a TLS timeout code, unless the listener is already
  closing and the final policy suppresses shutdown noise.

The timeout should be listener-level by default, with room for later SNI or
context-specific overrides.

## Close Semantics

`tls::listener::close()` should:

- close the lower listener so no new lower streams arrive;
- stop creating new handshakes;
- cancel pending handshakes owned by the listener;
- close lower streams for cancelled handshakes;
- leave already-emitted clear streams under upper-protocol ownership.

The close callback behavior is inherited from `io::stream_listener`. If the
current listener abstraction has only `close()` and no completion callback, TLS
should follow that shape until the shared IO contract changes.

## ALPN

ALPN is configured on `server_context`, but listener behavior must define what
happens when negotiation fails or is absent.

Recommended policy:

- by default, the handshake may complete with no selected ALPN;
- `require_alpn()` makes no match a handshake failure;
- server-side selection prefers the server's configured order among protocols
  proposed by the client;
- selected ALPN is available on the successful handshake result or resulting
  TLS stream metadata.

For HTTP, this lets the same secure listener later route connections to
HTTP/1.1 or HTTP/2 according to selected ALPN without making TLS depend on
HTTP.

## SNI

Initial slice can use one `server_context` for all handshakes.

The design should leave room for SNI context selection:

```cpp
uvp::tls::listener_options{}
  .on_server_name([](std::string_view name) {
    return context_for(name);
  });
```

Open questions for the follow-up design:

- does SNI selection live on `server_context` or `listener_options`;
- what type represents a selected context;
- what happens when the callback rejects or cannot find a name;
- whether the selected context can also change ALPN policy.

SNI selection is useful for multi-tenant HTTPS, but it does not need to block
the first listener adapter if the public API leaves room for it.

## Error Mapping

Handshake errors should map to `uvp::tls` errors while fitting the existing
`uvp::io::accept_result` surface.

Important cases:

- lower transport error during handshake;
- malformed TLS record;
- TLS protocol alert;
- unsupported TLS version or cipher configuration;
- ALPN required but not negotiated;
- handshake timeout;
- listener closing;
- pending handshake limit exceeded.

OpenSSL error queues should be drained inside TLS implementation code and
converted to stable uvpp-protocols errors before the callback is invoked.

## HTTP Integration

The HTTP server should need no special TLS-aware listen path:

```cpp
uvp::http::server http(loop);
http.listen(uvp::tls::listener{std::move(tcp), std::move(context)});
```

Optional HTTPS convenience helpers can be considered later in a composition
header, but the generic listener path should work first.

HTTP-specific concerns are tracked in
[HTTP TLS listener integration](http-tls-listener-integration.md).

## Test Scope

The listener adapter has tests for:

- successful TCP -> TLS -> clear stream acceptance;
- handshake timeout;
- pending handshake limit;
- listener close while handshakes are pending;
- selected ALPN metadata through the underlying handshake result;
- HTTP server accepting a TLS listener through the generic listener overload.

Still useful follow-up coverage:

- lower accept failure propagation through a controllable fake listener;
- accepted streams surviving listener close after emission;
- server-side SNI context selection once that API exists.

## Out Of Scope For First Implementation

- Server-side SNI context selection, if the base API leaves room for it.
- Listener pause/resume backpressure.
- Client certificate authentication policy beyond context-level primitives.
- Runtime certificate reloading.
- ACME, certificate storage, or deployment automation.
- HTTP-specific `listen_https()` convenience helpers.

## Source Documents

- [TLS support](tls-support.md)
- [HTTP TLS listener integration](http-tls-listener-integration.md)
- [Transport abstractions](../design/transport-abstractions.md)
- [Protocol composition](../design/protocol-composition.md)
