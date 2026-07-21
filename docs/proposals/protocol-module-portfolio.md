# Protocol Module Portfolio Proposal

Status: Proposed for Milestone 7 planning

## Context

After HTTP, TLS, DNS, URL, and WebSocket server foundations, the project needs a
clear way to evaluate future protocol modules. The candidate space is broad:
databases, caches, messaging, observability, mail, directories, identity,
discovery, SSH, and file transfer.

The project should not start all families at once. It should group them, decide
which ones validate reusable foundations, and identify where external
dependencies are the responsible engineering choice.

## Decision

Track protocol families as a portfolio, then select implementation milestones
from that portfolio.

Milestone 7 should keep the focus on:

- shared protocol foundation;
- WebSocket client;
- planning and ranking protocol families.

HTTP/2 should stay open, but not as the first Milestone 7 implementation target.
It depends on stream multiplexing, flow control, pool capacity, ALPN policy, and
possibly `nghttp2`; those decisions are better made after the shared client
session model is clearer.

## Evaluation Criteria

Each candidate protocol should be judged by:

- how well it composes over `uvp::io::byte_stream`;
- whether it validates shared cancellation, timeout, backpressure, or framing;
- whether a native implementation is reasonable;
- whether a proven external library should be wrapped instead;
- how much public API surface it commits;
- whether client, server, or both sides are in scope;
- whether the protocol belongs in this repository or a companion package.

## Families

### Data, Cache, and Databases

Candidates:

- Redis;
- Memcached;
- PostgreSQL;
- MariaDB/MySQL.

Recommended direction:

- Redis is a good native client candidate: RESP is compact, pipelining is useful,
  TLS composition is straightforward, and it exercises request/result
  multiplexing without HTTP/2 complexity.
- Memcached is also a good native candidate, especially the text protocol first;
  binary protocol can follow if needed.
- PostgreSQL should start as an adapter decision, likely around `libpq`
  nonblocking integration, before considering any native wire implementation.
- MariaDB/MySQL should also start as an adapter decision, probably with
  MariaDB Connector/C or a companion-package boundary.

Existing related proposals:

- [Redis support](redis-support.md)
- [Database client adapters](database-adapters.md)

### Messaging and Pub/Sub

Candidates:

- MQTT;
- NATS;
- RabbitMQ AMQP 0-9-1.

Recommended direction:

- MQTT is a strong native candidate because it is byte-stream friendly and can
  run over TCP, TLS, or WebSocket.
- NATS is a plausible native client after Redis or SMTP because the core
  protocol is relatively approachable and validates reconnect/subscription
  behavior.
- AMQP 0-9-1 is larger. It should probably wait until framing, channel
  ownership, heartbeats, and authentication conventions are established.

Existing related proposal:

- [MQTT client](mqtt-client.md)

### Logs, Metrics, and Telemetry

Candidates:

- syslog;
- statsd / dogstatsd;
- Prometheus exposition or client helpers;
- OpenTelemetry / OTLP.

Recommended direction:

- statsd/dogstatsd is a small first observability module: UDP first, optional
  TCP later, simple serialization, low API risk.
- syslog is a good follow-up because it exercises UDP/TCP/TLS variants and
  structured framing.
- Prometheus exposition mostly belongs on top of the HTTP server and may be a
  helper module rather than a transport protocol.
- OpenTelemetry is larger and dependency-heavy; OTLP over HTTP/gRPC/protobuf
  should be treated as an integration module after HTTP/2/protobuf decisions.

### Mail

Candidates:

- SMTP client;
- LMTP client/server;
- IMAP client;
- SMTP server.

Recommended direction:

- SMTP client is a reasonable early service client: line framing, STARTTLS,
  authentication, timeouts, and streaming message bodies are all useful
  foundation tests.
- LMTP is adjacent to SMTP and can follow once SMTP command/reply handling is
  clean.
- IMAP is substantially more complex because of tagged commands, literals,
  unsolicited responses, and mailbox state.
- SMTP server is possible later, but client-side delivery is a better first
  milestone.

Existing related proposal:

- [SMTP support](smtp-support.md)

### Directories, Identity, and Discovery

Candidates:

- OAuth2 / OIDC helpers;
- LDAP;
- RADIUS;
- TACACS+;
- mDNS;
- DNS-SD.

Recommended direction:

- OAuth2/OIDC should mostly build on HTTP client APIs. It is likely a helper
  layer, not a low-level transport module.
- LDAP needs BER/ASN.1 handling and authentication decisions; it should wait
  until framing/auth foundations are clearer.
- RADIUS and TACACS+ are security-sensitive and should be designed carefully
  before implementation.
- mDNS and DNS-SD are natural extensions of the DNS module, but require UDP
  multicast behavior and service discovery APIs.

### SSH and File Transfer

Candidates:

- SSH client/session;
- SFTP;
- FTP;
- FTPS.

Recommended direction:

- SSH is effectively a protocol suite: key exchange, encryption, authentication,
  channels, port forwarding, exec, shell, and subsystem behavior. It should use
  a proven library or live in a companion package unless there is a very narrow
  first slice.
- SFTP depends on SSH and should follow the SSH dependency decision.
- FTP is implementable but less strategic; FTPS adds TLS and legacy connection
  management complexity.

### HTTP Evolution

Candidates:

- WebSocket client;
- HTTP/2;
- HTTP/3 and QUIC.

Recommended direction:

- WebSocket client belongs in Milestone 7 because it reuses HTTP upgrade and the
  existing WebSocket session model.
- HTTP/2 should remain a design spike until shared stream ownership and flow
  control are settled.
- HTTP/3 and QUIC remain separate design work because QUIC changes the transport
  assumptions.

Existing related proposals:

- [WebSocket client](websocket-client.md)
- [HTTP/2 support](http2-support.md)
- [HTTP/3 and QUIC support](http3-quic-support.md)

## Suggested Ordering

1. Milestone 7: shared protocol foundation, WebSocket client, portfolio
   planning.
2. Milestone 8: outbound connector/proxy routes plus one or two small service
   clients, likely Redis and SMTP.
3. Next native candidates: Memcached, MQTT, syslog, statsd/dogstatsd.
4. Larger adapter candidates: PostgreSQL, MariaDB/MySQL, OpenTelemetry, SSH.
5. Larger native or dependency-backed protocols: LDAP, AMQP 0-9-1, IMAP,
   HTTP/2.

This ordering is deliberately conservative: it uses smaller protocols to harden
the shared substrate before adding protocols with heavy multiplexing, security,
or dependency commitments.

## Open Questions

- Should database and SSH adapters live in this repository or companion
  packages?
- Should observability helpers be protocols, integrations, or examples?
- How much authentication machinery belongs in `uvpp-protocols` versus
  application code?
- Should URL scheme registration be global, per module, or just documented
  mapping functions?
- Which first service client best validates the shared foundation after
  WebSocket client: Redis, SMTP, Memcached, or statsd?

## Source Documents

- [Shared protocol foundation](shared-protocol-foundation.md)
- [Redis support](redis-support.md)
- [SMTP support](smtp-support.md)
- [MQTT client](mqtt-client.md)
- [Database client adapters](database-adapters.md)
- [HTTP/2 support](http2-support.md)
- [HTTP/3 and QUIC support](http3-quic-support.md)
