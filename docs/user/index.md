# uvpp-protocols User Documentation

`uvpp-protocols` provides event-loop-friendly protocol modules on top of
[`uvpp`](https://github.com/peio42/uvpp).

Start here:

- [DNS](dns.md): asynchronous host/service resolution and cancellable address
  candidate operations.
- [HTTP server](http-server.md): routing, request body policies, responses,
  streaming, and errors.
- [URL](url.md): owning parsed URLs, default ports, origins, and HTTP request
  targets for client-side protocol code.
- [TLS](tls.md): stream and listener adapters, client verification, ALPN, and
  HTTP composition.
- [WebSocket](websocket.md): HTTP upgrade routes, WebSocket sessions, and
  byte-stream adaptation.

For implementation details, see the stable [design notes](../design/index.md).
