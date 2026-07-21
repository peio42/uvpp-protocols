# Protocol Composition

## Purpose

Many real deployments are protocol stacks, not isolated protocols:

```text
TCP -> HTTP
TCP -> TLS -> HTTP
TCP -> TLS -> HTTP -> WebSocket
TCP -> TLS -> HTTP -> WebSocket -> MQTT
```

`uvpp-protocols` should make those stacks explicit and readable. A user should
see which layer owns the network socket, which layer performs encryption, which
layer parses HTTP, and which layer owns the application protocol.

## Core Rule

Higher-level protocols should depend on a stream-like transport abstraction, not
on one specific lower protocol implementation.

That keeps these combinations possible:

- HTTP over plain TCP;
- HTTP over TLS;
- WebSocket over plain HTTP;
- WebSocket over HTTPS;
- MQTT over TCP;
- MQTT over TLS;
- MQTT over WebSocket.

The public API should present protocol composition as wrapping or accepting a
transport, not as hidden global configuration.

## Transport Concept

The shared transport boundary should be small. The first concrete abstraction is
documented in [Transport abstractions](transport-abstractions.md). A transport
must be able to:

- start reads;
- write byte buffers asynchronously;
- close gracefully or forcefully;
- expose the owning loop;
- report peer/local endpoint metadata when available;
- surface asynchronous errors and close events.

The boundary can begin as concrete overloads for uvpp stream types and evolve
toward a named concept or type-erased transport when more modules need it.

Possible internal shape:

```cpp
namespace uvp::io {

class byte_stream {
public:
  uv::loop& loop() noexcept;
  void read_start(read_callback cb);
  void write(std::span<const std::byte> bytes, write_callback cb);
  void close(close_callback cb);
};

}
```

This type does not have to be the first implementation. It documents the
contract protocol modules are trying to share.

## Server Stacks

For common cases, the high-level module should keep the terse API:

```cpp
uvp::http::server srv(loop);
srv.listen("127.0.0.1", 8080);
```

That is shorthand for a plain TCP listener feeding HTTP sessions.

TLS should be presented as an explicit listener or acceptor layer. In that
model, HTTP does not know OpenSSL or mbedTLS. It only receives accepted byte
streams from the TLS listener.

The initial TLS API is recorded in [TLS support](../archive/tls-support.md)
and [HTTP TLS listener integration](../archive/http-tls-listener-integration.md).

## Upgrade Stacks

Some stacks are created dynamically by protocol negotiation. WebSocket is the
main example: it starts as HTTP, then an accepted request upgrades the
connection.

The HTTP module exposes upgrade hooks without depending on the WebSocket
module:

```cpp
srv.upgrade("/events", [](uvp::http::upgrade_request& req) {
  uvp::websocket::accept_detached(req)
    .on_text([](uvp::websocket::session& ws, std::string_view msg) {
      ws.text(msg);
    });
});
```

The dependency direction is:

```text
uvp::websocket -> uvp::http upgrade API
uvp::http      -> no dependency on uvp::websocket
```

## Protocols Over WebSocket

Protocols can run above WebSocket either by consuming the WebSocket session
directly or by consuming an explicit byte-stream adapter derived from that
session.

`uvp::websocket::accept()` returns a `[[nodiscard]]` owning session handle. A
caller that wants callback-only, fire-and-forget behavior must request it
explicitly with `uvp::websocket::accept_detached()`.

Message-oriented protocols should accept `uvp::websocket::session` directly.
That keeps WebSocket message boundaries, text/binary distinction, close reasons,
and subprotocol details visible to the upper protocol:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  uvp::myproto::session::accept(std::move(ws),
    uvp::myproto::server_options{});
});
```

Byte-stream-oriented protocols should request a named adapter from the
WebSocket session. `uvp::websocket::accept()` still returns a
`uvp::websocket::session`; conversion to `uvp::io::byte_stream` is explicit and
consumes the WebSocket session:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto ws = uvp::websocket::accept(req, uvp::websocket::accept_options{}
    .subprotocol("myproto"));

  uvp::myproto::session::accept(
    std::move(ws).into_byte_stream(),
    uvp::myproto::server_options{});
});
```

For common byte-oriented protocol endpoints, a convenience helper can combine
acceptance and conversion:

```cpp
srv.upgrade("/myproto", [](uvp::http::upgrade_request& req) {
  auto stream = uvp::websocket::accept_byte_stream(req,
    uvp::websocket::accept_options{}
      .subprotocol("myproto"));

  uvp::myproto::session::accept(std::move(stream),
    uvp::myproto::server_options{});
});
```

`accept_byte_stream()` should be equivalent to
`accept(req, options).into_byte_stream()`. It is a convenience for protocols
that intentionally treat WebSocket as a binary byte transport, not a replacement
for the message-oriented `uvp::websocket::session` API.

The byte-stream adapter maps outbound writes to binary WebSocket frames and
delivers inbound binary message payloads as ordered bytes. Text frames are not
part of that byte-stream contract; protocols that need text frames or message
metadata should consume `uvp::websocket::session` directly.

MQTT client support is a useful future example of a byte-oriented protocol over
WebSocket. The important design point is that MQTT client code owns MQTT state,
WebSocket owns WebSocket framing, TLS owns encryption, and TCP owns the socket.
No layer should pretend to be the other.

MQTT client work is tracked in [MQTT client](../proposals/mqtt-client.md).

## Client Stacks

Client APIs should follow the same composition pattern when added. High-level
URL helpers are acceptable, but they should expand into explicit transport
choices internally.

Client-side work is tracked in [shared URL module](../archive/shared-url-module.md),
[DNS resolution](../archive/dns-resolution.md),
[HTTP client](../archive/http-client.md), and
[WebSocket client](../proposals/websocket-client.md).

## Ownership Across Layers

Layer ownership should be strict:

- the lowest active transport owns the native socket or handle;
- TLS owns handshake/encryption state and wraps the lower byte stream;
- HTTP owns HTTP parser state, routing, and request/response lifecycle;
- WebSocket owns frame parsing, masking, ping/pong, and message boundaries;
- MQTT client code owns MQTT packets, subscriptions, QoS state, and keep-alive.

Closing an upper layer should close or release the layers below according to a
documented policy. A close initiated by a lower layer must notify every upper
layer exactly once.

## Dependency Direction

Preferred dependency graph:

```text
uvp::tls        -> uvpp
uvp::http       -> uvpp
uvp::websocket  -> uvp::http upgrade API, uvpp
uvp::mqtt       -> shared byte stream API
```

Avoid hard dependencies such as:

```text
uvp::http -> uvp::tls
uvp::http -> uvp::websocket
uvp::tls  -> uvp::http
```

Convenience headers may compose modules:

```cpp
#include <uvpp/protocols/https.hpp>
#include <uvpp/protocols/mqtt_websocket.hpp>
```

Those headers should remain adapters. They should not move ownership or
protocol logic out of the canonical modules.
