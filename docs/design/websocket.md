# WebSocket Design

## Scope

Milestone 3 adds server-side WebSocket support on top of the HTTP/1.1 upgrade
path. The first implementation should cover:

- HTTP upgrade integration;
- RFC 6455 server handshake;
- text, binary, ping, pong, and close frames;
- per-session callbacks;
- configurable message and pending-write limits;
- ping/pong and close timeouts;
- a backpressure-aware send queue.

Client-side WebSocket support can follow the same session and framing model
later, but it is not required for the first server milestone.

## Dependency Direction

HTTP owns request parsing, routing, and the upgrade hook. WebSocket owns
handshake validation after the hook accepts the request, frame parsing, masking,
message assembly, control frames, close state, and WebSocket writes.

The dependency direction is:

```text
uvp::websocket -> uvp::http upgrade API, uvpp
uvp::http      -> no dependency on uvp::websocket
```

`uvp::http::server` should expose an upgrade hook that can be used by
WebSocket, CONNECT, or future upgrade-style protocols without linking those
modules into HTTP.

## Public API Shape

The intended server-side API is:

```cpp
uvp::http::server srv(loop);

srv.upgrade("/events", [](uvp::http::upgrade_request& req) {
  return uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .on_text([](uvp::websocket::session& ws, std::string_view message) {
      ws.text(message);
    })
    .on_binary([](uvp::websocket::session& ws, std::span<const std::byte> message) {
      ws.binary(message);
    }));
});
```

`uvp::websocket::accept()` returns a move-only `uvp::websocket::session`, not a
raw `uvp::io::byte_stream`. The session is the WebSocket protocol owner: it
keeps the frame parser, assembled message state, write queue, timers,
subprotocol selection, and close state together.

The HTTP upgrade callback returns the accepted protocol owner to the HTTP
server. After the `101 Switching Protocols` response is queued, the HTTP session
must stop HTTP parsing and transfer the underlying transport to the returned
owner.

## Protocols Above WebSocket

Applications can run another protocol above WebSocket in two ways.

Message-oriented protocols should accept the WebSocket session directly:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  return uvp::myproto::session::accept(std::move(ws),
    uvp::myproto::server_options{});
});
```

Byte-stream-oriented protocols should request an explicit adapter from the
WebSocket session:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  return uvp::myproto::session::accept(
    std::move(ws).into_byte_stream(),
    uvp::myproto::server_options{});
});
```

`into_byte_stream()` consumes the WebSocket session and returns a
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

`uvp::websocket::session` should provide:

- `text(std::string_view)`;
- `binary(std::span<const std::byte>)`;
- `ping(std::span<const std::byte> payload = {})`;
- `pong(std::span<const std::byte> payload = {})`;
- `close(close_code code = close_code::normal, std::string_view reason = {})`;
- callback registration through `accept_options` for text, binary, ping, pong,
  close, and errors;
- accessors for local and remote endpoints through the underlying transport.

The session owns write buffers until uvpp write completion callbacks run.
Queued payload bytes count against a configurable pending-write limit. Messages
larger than the configured maximum should close the session with an appropriate
WebSocket close code.

## Framing Notes

The first implementation should implement RFC 6455 framing directly rather than
adding a WebSocket dependency. The server must:

- require client-to-server frames to be masked;
- never mask server-to-client frames;
- handle fragmented data messages;
- require control frames to be unfragmented and at most 125 bytes;
- automatically respond to ping with pong unless the user callback overrides
  that policy;
- perform a close handshake and enforce a close timeout.

The implementation should keep SHA-1 and base64 helpers private to the
WebSocket module unless a later shared utility need appears.

