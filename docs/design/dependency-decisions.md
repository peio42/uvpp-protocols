# Dependency Decisions

`uvpp-protocols` depends on lower-level protocol libraries where that reduces
parser risk, security risk, or implementation scope. Those dependencies must
not define the public API shape unless the project explicitly chooses to expose
an adapter module.

## Rules

- External protocol engines stay behind `detail/` or module-private adapters.
- Public APIs use `uv::...` types, not third-party types.
- A dependency may own protocol state, but it must not own the public model.
- For server-side protocol modules, dependencies should not own the uvpp loop,
  accepted sockets, timers, output buffers, or session lifetime.
- If a dependency is not a synchronous state machine, its ownership model must
  be called out explicitly before adoption.
- Do not add a backend selector until there are at least two real backends that
  the project is willing to support and test.

## Current Decisions

| Area | Decision | Status | Notes |
| --- | --- | --- | --- |
| HTTP/1.1 | [`llhttp`](https://github.com/nodejs/llhttp) | Accepted | Parser/state machine under `src/http/detail`; already integrated. |
| HTTP/2 | [`nghttp2`](https://nghttp2.org/) / [`libnghttp2`](https://github.com/nghttp2/nghttp2) | Accepted for future HTTP/2 | Mature C HTTP/2 implementation with HPACK. Do not configure or link in early milestones. |
| HTTP/3 | [`nghttp3`](https://github.com/ngtcp2/nghttp3) plus a QUIC transport such as [`ngtcp2`](https://github.com/ngtcp2/ngtcp2) | Recommended for future HTTP/3 | `nghttp3` implements HTTP/3 mapping over QUIC and QPACK, but does not provide QUIC transport. |
| URL parsing | [`ada-url`](https://github.com/ada-url/ada) | Accepted for shared URL module | Good fit for a general `uvp::url` wrapper. Use it beyond HTTP where WHATWG URL semantics are appropriate. |
| HTTP client | [`libcurl`](https://curl.se/libcurl/) | Candidate with constraints | Useful for broad client protocol coverage. Its multi-socket API can integrate with event loops, but libcurl owns substantial client state and socket orchestration. |
| TLS | OpenSSL-family backend | Deferred | Initial candidate is OpenSSL 3.x for broad platform support. Need a separate TLS design note before adoption. |
| PostgreSQL | [`libpq`](https://www.postgresql.org/docs/current/libpq.html) | Candidate | Official PostgreSQL C client. Nonblocking APIs and socket readiness can integrate with uvpp. |
| MariaDB/MySQL | [MariaDB Connector/C](https://mariadb.com/docs/connectors/mariadb-connector-c) | Candidate | Official C connector for MariaDB/MySQL with nonblocking API support. License and event integration need review. |
| SMTP | No dependency selected | Open | Need to decide whether to implement a small SMTP state machine or adopt a library. |
| WebSocket | No dependency selected | Open | Likely implemented directly on top of HTTP upgrade unless a small framing library is clearly better. |
| MQTT | No dependency selected | Open | Need to evaluate MQTT C libraries versus a small packet/session implementation. |

## HTTP Family

### HTTP/1.1

`llhttp` is the HTTP/1.1 parser. It is not a replaceable backend at this stage.
CMake may fetch it or use a local source checkout, but the library should not
advertise parser choice as a public or build-level product feature.

The integration boundary is:

```text
uvp::http session
  -> src/http/detail/http1_state_machine
    -> llhttp callbacks
```

`llhttp` reports parser events. The uvpp-protocols session owns accepted
connections, read buffers, timers, route dispatch, response writes, and public
request/response objects.

### HTTP/2

`libnghttp2` is the preferred future HTTP/2 engine. It is mature, C-based, and
focused on HTTP/2 framing plus HPACK. This matches the project rule: use a
proven state machine for complex protocol framing, then keep uvpp-protocols in
charge of sockets, timers, and public API.

HTTP/2 should not be introduced during the HTTP/1 milestones, but HTTP/1
abstractions should avoid assumptions that block it:

- do not assume one request at a time per TCP/TLS connection at the public
  connection layer;
- do not make route dispatch depend on HTTP/1 parser callbacks directly;
- keep headers and request metadata version-neutral where practical;
- model response completion independently from connection close.

### HTTP/3

`nghttp3` is appropriate for the HTTP/3 layer, with an important caveat: it is
not a QUIC transport implementation. It implements the HTTP/3 mapping over QUIC
and QPACK. A future HTTP/3 module would need a QUIC layer as well, most likely
`ngtcp2` or another explicitly selected QUIC implementation.

The likely future stack is:

```text
UDP socket
  -> QUIC transport, such as ngtcp2
    -> HTTP/3 mapping and QPACK, such as nghttp3
      -> uvp::http public request/response model
```

Do not add HTTP/3 APIs until the QUIC ownership model is designed. QUIC changes
the transport assumptions more deeply than HTTP/2 does.

## URL Parsing

`ada-url` is the accepted URL parsing dependency for a shared URL module. It is
a modern C++ WHATWG-compatible parser, which makes it suitable for HTTP client
URLs, request target helpers, proxy targets, and configuration strings that use
web-style URL semantics.

The public type should be general:

```cpp
namespace uv {

class url;

}
```

HTTP-specific helpers can then use `uvp::url` rather than defining a separate
`uvp::http::url` type.

Adoption questions:

- Where is WHATWG URL behavior desired, and where do protocols need stricter
  RFC-specific parsing?
- Do we need RFC-oriented parsing for SMTP, database URLs, or lower-level
  socket endpoints?
- Should endpoint convenience parsing accept URL strings, and if so which
  Unix-socket URL convention should be supported?

## HTTP Client

`libcurl` is attractive for HTTP client coverage because it supports many HTTP
features and has an event-oriented multi-socket API. It is not a simple
synchronous parser, however. It owns substantial transfer state and socket
orchestration.

Possible decision:

- use libcurl only for an optional high-level HTTP client adapter;
- keep `uvp::http::server` and shared HTTP vocabulary independent from libcurl;
- never let libcurl types leak into `uvp::http` request/response APIs;
- document that a libcurl adapter composes with uvpp through socket callbacks,
  not through the same state-machine boundary as `llhttp`.

Do not adopt libcurl for the first HTTP server milestones.

## TLS

TLS has a dedicated [design note](tls.md). The initial backend should be
OpenSSL 3.x, kept private to the TLS module behind `uvp::tls` context and stream
types.

TLS must remain a module boundary:

```text
uvp::io::byte_stream
  -> uvp::tls
    -> uvp::io::byte_stream
      -> HTTP, SMTP, MQTT, or another byte-oriented protocol
```

HTTP must not directly depend on a TLS provider, and TLS must not depend on
HTTP. TLS should also support STARTTLS-style upgrades where an application
protocol begins in clear text and later hands its transport to `uvp::tls`.

## Database Protocols

Database adapters are candidates for later milestones. They are not core
protocol modules for the initial HTTP/WebSocket/TLS roadmap, but they fit the
larger project goal of reusable event-based protocol integrations.

### PostgreSQL

`libpq` is the natural candidate for PostgreSQL. It provides the official C API
and supports nonblocking operation. A uvpp adapter would likely monitor the
socket returned by libpq and drive `PQconsumeInput`, `PQflush`, and result
collection from uvpp readiness events.

Adoption questions:

- Is this project meant to include database client modules, or should those live
  in a separate companion package?
- Can the adapter avoid blocking APIs completely?
- How should query/result lifetimes map to uvpp-protocols callback style?

### MariaDB/MySQL

MariaDB Connector/C is the initial candidate for MariaDB/MySQL. It supports the
MariaDB/MySQL C API family and has documented nonblocking operation. Before
adoption, review:

- license constraints;
- compatibility expectations with MySQL;
- nonblocking API ergonomics;
- socket ownership and reconnect behavior;
- prepared statement and result streaming model.

## Open Decisions

- TLS provider and public TLS abstraction.
- Whether URL parsing becomes a shared module.
- Whether HTTP client is implemented with libcurl, direct `llhttp`/`nghttp2`,
  or a smaller custom client stack.
- Whether database protocols belong in `uvpp-protocols` or a separate package.
- SMTP dependency strategy.
- MQTT dependency strategy.
