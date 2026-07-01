# WebSocket Design

## Scope

Server-side WebSocket support is implemented on top of the HTTP/1.1 upgrade
path. The current implementation covers:

- HTTP upgrade integration;
- RFC 6455 server handshake;
- text, binary, ping, pong, and close frames;
- per-session callbacks;
- configurable message and pending-write limits;
- configurable automatic pong responses;
- a backpressure-aware send queue.

Ping/pong scheduling and close-handshake timeouts remain follow-up
refinements. The implementation responds to ping frames automatically by
default and lets advanced users opt out with `accept_options::auto_pong(false)`.
The session closes the transport after sending an application-initiated close
frame.

Client-side WebSocket support is not implemented yet; it is tracked in
[WebSocket client](../proposals/websocket-client.md).

## Dependency Direction

HTTP owns request parsing, routing, and the upgrade hook. WebSocket owns
handshake validation after the hook accepts the request, frame parsing, masking,
message assembly, control frames, close state, and WebSocket writes.

The dependency direction is:

```text
uvp::websocket -> uvp::http upgrade API, uvpp
uvp::http      -> no dependency on uvp::websocket
```

`uvp::http::server` exposes an upgrade hook that can be used by WebSocket or
future upgrade-style protocols without linking those modules into HTTP.

## Public API Shape

The server-side API is:

```cpp
uvp::http::server srv(loop);

srv.upgrade("/events", [](uvp::http::upgrade_request& req) {
  uvp::websocket::accept_detached(req)
    .on_text([](uvp::websocket::session& ws, std::string_view message) {
      ws.text(message);
    })
    .on_binary([](uvp::websocket::session& ws, std::span<const std::byte> message) {
      ws.binary(message);
    });
});
```

`uvp::websocket::accept()` returns a move-only `uvp::websocket::session`, not a
raw `uvp::io::byte_stream`. The session is the WebSocket protocol owner: it
keeps the frame parser, assembled message state, write queue, timers,
subprotocol selection, and close state together.

This is intentional because WebSocket is message-oriented. A raw byte stream
cannot represent text vs binary messages, fragmented message assembly,
ping/pong, close codes and reasons, masking, or negotiated subprotocol details.
When an upper protocol wants ordered bytes, it must opt into the byte-stream
mapping explicitly.

After the `101 Switching Protocols` response is queued, the HTTP session stops
HTTP parsing and transfers the underlying transport to the accepted protocol
owner. `uvp::websocket::accept()` returns a `[[nodiscard]]` owning application
handle; the caller must keep that handle, move it into a higher-level protocol,
or convert it into a byte stream. Destroying the owning handle closes the
WebSocket session.

Callback-only endpoints that intentionally do not keep a handle should call
`uvp::websocket::accept_detached()`. The detached form is the only WebSocket
acceptance path where the internal session state keeps itself alive while the
upgraded transport remains open. It returns a non-owning session handle so
callbacks can still be registered directly on the session.

## Protocols Above WebSocket

Applications can run another protocol above WebSocket in two ways.

Message-oriented protocols should accept the WebSocket session directly:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  uvp::myproto::session::accept(std::move(ws),
    uvp::myproto::server_options{});
});
```

Byte-stream-oriented protocols should request an explicit adapter from the
WebSocket session:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  uvp::myproto::session::accept(
    std::move(ws).into_byte_stream(),
    uvp::myproto::server_options{});
});
```

For the common case where the upper protocol only wants a byte stream, a helper
may combine acceptance and conversion:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto stream = uvp::websocket::accept_byte_stream(req,
    uvp::websocket::accept_options{}
      .subprotocol("myproto"));

  uvp::myproto::session::accept(std::move(stream),
    uvp::myproto::server_options{});
});
```

`accept_byte_stream(req, options)` is a convenience equivalent to
`accept(req, options).into_byte_stream()`. It should not replace
`uvp::websocket::accept()` as the primary API, because applications that need
WebSocket semantics should receive a `uvp::websocket::session`.

`into_byte_stream()` consumes the WebSocket session handle and returns a
`uvp::io::byte_stream` adapter whose lower layer is WebSocket framing. This
keeps ownership explicit: after conversion, the custom protocol owns the
transport abstraction and no longer has direct access to WebSocket-specific
callbacks.

The byte-stream adapter uses binary WebSocket messages as transport payload.
Outbound writes are sent as binary frames. Inbound binary message payloads are
delivered as ordered bytes. Text messages are not part of the byte-stream
contract and should either close the WebSocket with a protocol error or be
handled by a higher-level session that accepts `uvp::websocket::session`
directly.

Protocols that need message boundaries, text frames, close reasons, ping/pong
visibility, or negotiated subprotocol details should consume
`uvp::websocket::session` rather than converting it to `uvp::io::byte_stream`.

## HTTP Upgrade Contract

The HTTP module needs a small generic upgrade surface:

- route matching for upgrade targets, separate from normal HTTP route handlers;
- an `upgrade_request` view with method, target, path, query, headers, route
  params, connection metadata, and access to any bytes already read after the
  HTTP headers;
- a result type that either rejects the upgrade with an HTTP response or accepts
  it and takes ownership of the connection;
- a documented rule that an accepted upgrade ends the HTTP request/response
  lifecycle for that connection.

When `llhttp` reports `HPE_PAUSED_UPGRADE`, the HTTP session should dispatch to
the matching upgrade handler instead of the normal route table. If no upgrade
handler matches, HTTP should reject the request with a normal error response.

Any bytes already read beyond the HTTP upgrade headers must be handed to the
accepted protocol owner before the next socket read is started. This matters
when the client sends the first WebSocket frame in the same TCP packet as the
upgrade request.

## Session Behavior

`uvp::websocket::session` provides:

- `text(std::string_view)`;
- `binary(std::span<const std::byte>)`;
- `ping(std::span<const std::byte> payload = {})`;
- `pong(std::span<const std::byte> payload = {})`;
- `close(close_code code = close_code::normal, std::string_view reason = {})`;
- callback registration on the session for text, binary, ping, pong, close, and
  errors;
- `accept_options::auto_pong(bool)` to keep the default automatic ping response or make
  `on_ping` responsible for replying;
- accessors for local and remote endpoints through the underlying transport;
- `into_byte_stream()` for binary byte-stream protocols over WebSocket.

The session owns write buffers until uvpp write completion callbacks run.
Queued payload bytes count against a configurable pending-write limit. Messages
larger than the configured maximum should close the session with an appropriate
WebSocket close code.

## Framing Notes

The first implementation implements RFC 6455 framing directly rather than
adding a WebSocket dependency. The server must:

- require client-to-server frames to be masked;
- never mask server-to-client frames;
- handle fragmented data messages;
- require control frames to be unfragmented and at most 125 bytes;
- automatically respond to ping with pong unless `auto_pong(false)` makes the
  application responsible for that protocol obligation;
- perform a close handshake. Timeout enforcement can be added when the session
  grows timer-owned liveness policies.

The implementation should keep the accept-value helper private to the WebSocket
module unless a later shared utility need appears. SHA-1 should be delegated to
OpenSSL `Crypto` through the high-level EVP digest API, with OpenSSL headers and
types kept out of public uvpp-protocols headers. In the current monolithic
target, this is a private implementation dependency; once package targets are
split, it belongs to the WebSocket target only.

Inbound bytes are accumulated in a contiguous `std::vector<std::byte>` plus a
read offset. The parser advances the offset as complete frames are consumed and
compacts the vector only when all bytes have been consumed or when the consumed
prefix becomes large enough to matter. This avoids an O(n) erase at the front
of the buffer for every frame while preserving cache-friendly indexed parsing.
