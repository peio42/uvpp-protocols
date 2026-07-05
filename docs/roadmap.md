# Roadmap

## Target

- Stable HTTP/WebSocket server foundation for small services, local agents, and
  protocol gateways.

## Current focus

- Milestone 5: TLS transport support
  - [ ] [TLS stream adapter](proposals/tls-support.md)
  - [ ] [TLS listener adapter](proposals/tls-listener-adapter.md)
  - [ ] [HTTP over TLS through listener composition](proposals/http-tls-listener-integration.md)

Keep completed Current focus proposals listed until the milestone is closed.
When a Current focus proposal is implemented, mark its line with `[x]` instead
of removing it. Replace the whole Current focus block only when starting the
next milestone or step.

## Next

- Client-side foundations
  - [Shared URL module](proposals/shared-url-module.md)
  - [DNS resolution](proposals/dns-resolution.md)
  - [HTTP client](proposals/http-client.md)

## Later

- Client protocol modules
  - [WebSocket client](proposals/websocket-client.md)

- HTTP API polish
  - [Fluent temporary route builder overloads](proposals/fluent-temporary-route-builders.md)
  - [Route-level hooks](proposals/route-level-hooks.md)
  - [Route options body mode](proposals/route-options-body-mode.md)
  - [Route options parameter constraints](proposals/route-options-parameter-constraints.md)
  - [Stream body error ownership](proposals/stream-body-error-ownership.md)

- Protocol client modules
  - [SMTP client](proposals/smtp-support.md)
  - [Redis client](proposals/redis-support.md)
  - [MQTT client over TCP, TLS, or WebSocket](proposals/mqtt-client.md)

## Separate Design Needed

- [HTTP/2 design spike](proposals/http2-support.md)
- [HTTP/3 and QUIC](proposals/http3-quic-support.md)
- [Database client adapters](proposals/database-adapters.md)
