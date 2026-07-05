# Roadmap

## Target

- Composable protocol foundations for uvpp applications: HTTP/WebSocket server
  support, TLS transport composition, and uvpp-native client-side building
  blocks for URL parsing, DNS, outbound connections, and HTTP.

## Current focus

- Milestone 6: Client-side foundations
  - [ ] [Shared URL module](proposals/shared-url-module.md)
  - [ ] [DNS resolution](proposals/dns-resolution.md)
  - [ ] [HTTP client](proposals/http-client.md)

Keep completed Current focus proposals listed until the milestone is closed.
When a Current focus proposal is implemented, mark its line with `[x]` instead
of removing it. Replace the whole Current focus block only when starting the
next milestone or step.

## Next

- Milestone 7: Client protocol expansion
  - [WebSocket client](proposals/websocket-client.md)
  - [HTTP/2 design spike](proposals/http2-support.md)

## Later

- TLS hardening
  - [TLS policy and identity](proposals/tls-policy-and-identity.md)
  - [TLS graceful shutdown](proposals/tls-graceful-shutdown.md)

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

- [HTTP/3 and QUIC](proposals/http3-quic-support.md)
- [Database client adapters](proposals/database-adapters.md)
