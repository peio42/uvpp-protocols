# Roadmap

## Target

- Stable HTTP/WebSocket server foundation for small services, local agents, and
  protocol gateways.

## Current focus

- Milestone 3: HTTP routing and application polish
  - Middleware chain
  - Route groups and subtree hooks
  - Existing routing/error handling polish
  - See: [routing and middleware polish](proposals/routing-and-middleware-polish.md),
    [route groups and hooks](proposals/route-groups-and-hooks.md)

## Next

- Milestone 4: HTTP payload and streaming helpers
  - Typed JSON request bodies
  - Multipart handling
  - Server-Sent Events helper
  - Static file helper
  - See: [typed JSON body policy](proposals/typed-json-body-policy.md),
    [multipart handling](proposals/multipart-handling.md),
    [SSE support](proposals/sse-support.md),
    [static file helper](proposals/static-file-helper.md)

- Milestone 5: TLS transport support
  - TLS stream adapter
  - TLS listener adapter
  - HTTP over TLS through listener composition
  - See: [TLS support](proposals/tls-support.md),
    [HTTP TLS listener integration](proposals/http-tls-listener-integration.md)

## Later

- Milestone 6: Client-side foundations
  - Shared URL module
  - HTTP client
  - WebSocket client
  - See: [shared URL module](proposals/shared-url-module.md),
    [HTTP client](proposals/http-client.md),
    [WebSocket client](proposals/websocket-client.md)

- Milestone 7: Protocol client modules
  - SMTP client
  - Redis client
  - MQTT client over TCP, TLS, or WebSocket
  - See: [SMTP client](proposals/smtp-support.md),
    [Redis support](proposals/redis-support.md),
    [MQTT client](proposals/mqtt-client.md)

## Separate Design Needed

- HTTP/2 design spike
- HTTP/3 and QUIC
- Database client adapters
- See: [HTTP/2 support](proposals/http2-support.md),
  [HTTP/3 and QUIC support](proposals/http3-quic-support.md),
  [database client adapters](proposals/database-adapters.md)
