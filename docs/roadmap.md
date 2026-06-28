# Roadmap

## Target

- Stable HTTP/WebSocket server foundation for small services, local agents, and
  protocol gateways.

## Current focus

- Milestone 3: HTTP routing and application polish
  - [Route groups and subtree hooks](proposals/route-groups-and-hooks.md)
  - [Route path decoding](proposals/route-path-decoding.md)
  - [HTTP timeout enforcement](proposals/http-timeout-enforcement.md)

## Next

- Milestone 4: HTTP payload and streaming helpers
  - [Typed JSON request bodies](proposals/typed-json-body-policy.md)
  - [Multipart handling](proposals/multipart-handling.md)
  - [Server-Sent Events helper](proposals/sse-support.md)
  - [Static file helper](proposals/static-file-helper.md)

- Milestone 5: TLS transport support
  - [TLS stream adapter](proposals/tls-support.md)
  - [TLS listener adapter](proposals/tls-support.md)
  - [HTTP over TLS through listener composition](proposals/http-tls-listener-integration.md)

## Later

- Client-side foundations
  - [Shared URL module](proposals/shared-url-module.md)
  - [HTTP client](proposals/http-client.md)
  - [WebSocket client](proposals/websocket-client.md)

- HTTP API polish
  - [Fluent temporary route builder overloads](proposals/fluent-temporary-route-builders.md)
  - [Route options body mode](proposals/route-options-body-mode.md)

- Protocol client modules
  - [SMTP client](proposals/smtp-support.md)
  - [Redis client](proposals/redis-support.md)
  - [MQTT client over TCP, TLS, or WebSocket](proposals/mqtt-client.md)

## Separate Design Needed

- [HTTP/2 design spike](proposals/http2-support.md)
- [HTTP/3 and QUIC](proposals/http3-quic-support.md)
- [Database client adapters](proposals/database-adapters.md)
