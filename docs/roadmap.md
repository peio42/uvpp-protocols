# Roadmap

## Target

- Composable protocol foundations for uvpp applications: HTTP/WebSocket
  support, TLS transport composition, uvpp-native client-side building blocks,
  and a reusable substrate for service protocols.

## Current focus

- Milestone 7: Protocol expansion foundations
  - [ ] [Shared protocol foundation](proposals/shared-protocol-foundation.md)
  - [ ] [WebSocket client](proposals/websocket-client.md)
  - [ ] [Protocol module portfolio](proposals/protocol-module-portfolio.md)

Keep completed Current focus proposals listed until the milestone is closed.
When a Current focus proposal is implemented, mark its line with `[x]` instead
of removing it. Replace the whole Current focus block only when starting the
next milestone or step.

## Next

- Milestone 8: First service client modules
  - [Outbound connector and proxy routes](proposals/outbound-connector-and-proxy-routes.md)
  - [Redis client](proposals/redis-support.md)
  - [SMTP client](proposals/smtp-support.md)

## Later

- HTTP evolution
  - [HTTP client flow control and deadlines](proposals/http-client-flow-control-and-deadlines.md)
  - [HTTP redirect policy extensions](proposals/http-redirect-policy-extensions.md)
  - [HTTP/2 design spike](proposals/http2-support.md)

- TLS hardening
  - [TLS policy and identity](proposals/tls-policy-and-identity.md)
  - [TLS graceful shutdown](proposals/tls-graceful-shutdown.md)

- HTTP API polish
  - [Fluent temporary route builder overloads](proposals/fluent-temporary-route-builders.md)
  - [Route-level hooks](proposals/route-level-hooks.md)
  - [Route options body mode](proposals/route-options-body-mode.md)
  - [Route options parameter constraints](proposals/route-options-parameter-constraints.md)
  - [Stream body error ownership](proposals/stream-body-error-ownership.md)

- Protocol client families
  - [MQTT client over TCP, TLS, or WebSocket](proposals/mqtt-client.md)
  - [Database client adapters](proposals/database-adapters.md)

## Separate Design Needed

- [HTTP/3 and QUIC](proposals/http3-quic-support.md)
