# WebSocket Client Proposal

Status: Implemented direct client slice for Milestone 7 protocol expansion foundations

## Decision

Add a native, callback-based WebSocket client in 'uvp::websocket'. The first
slice accepts direct 'ws://' and 'wss://' URLs, establishes the transport,
performs the RFC 6455 client handshake, and returns the existing move-only
'uvp::websocket::session' on success.

The connection operation owns the transport only until the handshake succeeds.
It then transfers the 'uvp::io::byte_stream' to the WebSocket session. The
session owns framing, reads, writes, close handling, and the eventual transport
shutdown. A failed, timed-out, or cancelled attempt closes every partially
established lower layer and completes exactly once.

The first implementation must not build the upgrade on 'uvp::http::client'.
That API currently rejects a 101 response and has no transport-handoff contract.
The WebSocket client may share private HTTP/1.1 serialization and header-parser
utilities once suitable, but it owns a small, non-pooled upgrade exchange.

## Context

The server-side implementation already provides RFC 6455 handshakes, framing,
message callbacks, close handling, bounded outbound writes, automatic pong, and
an explicit binary byte-stream adapter. The client is the important Milestone 7
consumer of the shared protocol foundation:

~~~
ws URL
  -> DNS -> TCP -> HTTP/1.1 upgrade -> WebSocket session

wss URL
  -> DNS -> TCP -> TLS (SNI + verification + ALPN http/1.1)
  -> HTTP/1.1 upgrade -> WebSocket session
~~~

It exercises URL expansion, operation lifetime, deadlines, TLS composition,
HTTP upgrade parsing, transport handoff, client-side masking, and write
backpressure without introducing an unrelated high-level service API.

## Current State

- Implemented: server-side handshake, framing, sessions, 'accept_detached',
  message and outbound-write limits, automatic pong, and binary byte-stream
  adaptation.
- Implemented: URL parsing for 'ws'/'wss', DNS, TCP connection, TLS
  'byte_stream' adaptation, exactly-once operation lifetime, and deadlines.
- Implemented: direct client handshake, options, URL connection helper,
  client-side masked writes, cancellation, deadlines, and client connection
  lifecycle.
- Explicitly unsupported by 'uvp::http::client': a 101 response and an HTTP
  transport handoff.

## Goals

- Provide direct 'ws://' and 'wss://' connection from a URL.
- Return the same 'uvp::websocket::session' vocabulary used by server endpoints;
  application message and close semantics must not diverge by endpoint role.
- Keep transport ownership explicit at the upgrade boundary.
- Apply TLS SNI and peer verification to the logical URL host, never to a
  future proxy host.
- Enforce RFC 6455 client handshake and framing requirements before exposing a
  usable session.
- Bound message, handshake, and queued-write resources; make cancellation and
  timeouts exactly-once operations.
- Leave a narrow insertion point for a later outbound connector and proxy
  route implementation.

## Non-Goals

- Browser API compatibility, cookies, Origin policy, CORS, or a JavaScript event
  model.
- Automatic redirects, authentication challenges, connection pooling, or
  automatic reconnect.
- Per-message compression or any 'Sec-WebSocket-Extensions' negotiation.
- HTTP/2 extended CONNECT, HTTP/3, or WebTransport.
- HTTP forward proxy, HTTP CONNECT, SOCKS, PAC, and environment proxy
  discovery. These belong to the later
  [Outbound connector and proxy routes](outbound-connector-and-proxy-routes.md)
  proposal.
- A subscription, request/response, or RPC layer above WebSocket.

## Proposed Public API

This is the intended public shape; names may receive small adjustments before
stabilization. The options that describe an established session should be shared
with server acceptance, rather than copied into unrelated option classes. A
common 'session_options' may be extracted from the current 'accept_options',
while retaining a source-compatible server-facing wrapper.

~~~cpp
namespace uvp::websocket {

enum class client_errc {
  invalid_url = 1,
  unsupported_scheme,
  invalid_option,
  dns_failed,
  connect_failed,
  tls_failed,
  handshake_failed,
  unexpected_response,
  invalid_accept,
  invalid_subprotocol,
  extensions_unsupported,
  cancelled,
  timeout,
};

struct client_options {
  // Session policy; final storage is shared with server acceptance.
  std::size_t max_message_bytes = 1024 * 1024;
  std::size_t max_pending_write_bytes = 1024 * 1024;
  std::chrono::milliseconds close_timeout = std::chrono::seconds{5};
  bool auto_pong = true;

  std::vector<std::string> subprotocols;
  std::optional<std::string> origin;
  uvp::http::headers headers;

  std::chrono::milliseconds overall_timeout{0};
  std::chrono::milliseconds dns_timeout{0};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds tls_handshake_timeout{0};
  std::chrono::milliseconds websocket_handshake_timeout{0};

  bool tls_default_verify_paths = true;
  std::string tls_ca_file;
  std::string tls_ca_path;
};

using connect_callback = std::function<void(uvp::result<session>)>;

class connect_operation {
public:
  void cancel() noexcept;
  [[nodiscard]] bool active() const noexcept;
};

class client {
public:
  explicit client(uv::loop& loop);
  client(uv::loop& loop, client_options options);

  [[nodiscard]] connect_operation connect(
    std::string_view url, connect_callback callback);
  [[nodiscard]] connect_operation connect(
    std::string_view url, client_options options, connect_callback callback);
};

} // namespace uvp::websocket
~~~

The callback receives a lifetime-owning 'session', equivalent to
'websocket::accept()', rather than a detached handle. Users retain it, move it
to a protocol above WebSocket, or explicitly convert it with
'std::move(session).into_byte_stream()'. Destruction of the owning handle closes
the completed connection.

~~~cpp
uvp::websocket::client client(loop);

auto operation = client.connect("wss://echo.example.test/events",
  uvp::websocket::client_options{
    .subprotocols = {"events.v1"},
    .websocket_handshake_timeout = std::chrono::seconds{10},
  },
  [](uvp::result<uvp::websocket::session> result) {
    if (!result) {
      log_error(result.error().code, result.error().detail);
      return;
    }

    auto ws = std::move(result).value();
    ws.on_text([](uvp::websocket::session&, std::string_view message) {
      consume(message);
    });
    ws.text("ready");
    retain(std::move(ws));
  });
~~~

'headers' is an escape hatch for ordinary request fields such as tracing
headers. It must reject invalid HTTP field names and values as the HTTP client
does, and it must reject protocol-owned fields: 'Host', 'Connection', 'Upgrade',
'Sec-WebSocket-Key', 'Sec-WebSocket-Version', 'Sec-WebSocket-Protocol', and
'Sec-WebSocket-Extensions'. 'Origin', if configured, is written once by the
client rather than through 'headers'.

The initial API rejects URL userinfo: it must not silently translate a password
into an Authorization header. The URL fragment is not sent.

## Connection and Handshake Flow

The connect state is loop-affine and uses 'uvp::detail::operation_lifetime' as
the sole terminal-completion gate:

~~~
parse URL
  -> resolve logical host
  -> TCP connect
  -> [wss only: TLS handshake]
  -> write HTTP upgrade request
  -> read and validate HTTP 101 response
  -> transfer stream to client-role WebSocket session
  -> complete successfully
~~~

For 'ws', the default port is 80; for 'wss', it is 443 unless explicitly
provided. The request target is the URL origin-form path and query, defaulting
to '/'. DNS, TCP, and TLS reuse the existing layers.

For 'wss', the client builds a 'tls::client_context' with the logical hostname
as 'server_name', default ALPN '{"http/1.1"}', and the configured trust
locations. A TLS result selecting a non-empty protocol other than 'http/1.1'
fails before HTTP bytes are written.

When the future outbound connector exists, only the DNS/TCP portion changes. It
must return a stream logically connected to the URL authority; TLS, HTTP
upgrade, and WebSocket framing remain above that boundary. This keeps proxy
identity out of SNI and hostname verification.

### HTTP Upgrade Request

Every attempt obtains a cryptographically secure, fresh 16-byte nonce, encodes
it as canonical Base64, and remembers it only until response validation. It
writes one HTTP/1.1 request:

~~~http
GET /events HTTP/1.1
Host: echo.example.test
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: <fresh canonical Base64 nonce>
Sec-WebSocket-Protocol: events.v1, other.v1
Origin: https://application.example.test

~~~

'Host' includes an explicit non-default port and formats IP literals correctly.
'Sec-WebSocket-Protocol' is omitted if no protocols are offered. Each offer
must be a distinct, non-empty HTTP token; an invalid or duplicate option fails
before the connection starts.

The initial implementation sends no 'Sec-WebSocket-Extensions' request. It must
use a deliberate 'Connection: close' request policy, or otherwise prove that a
failed upgrade cannot enter HTTP reuse. A successful WebSocket is never put in
an HTTP connection pool.

### HTTP Upgrade Response

The response reader parses only a bounded HTTP/1.1 status line and header block;
it does not accept arbitrary HTTP response semantics. Before changing transport
owner, it requires:

- status exactly 101;
- 'Upgrade' equal to the case-insensitive token 'websocket';
- 'Connection' containing the case-insensitive token 'Upgrade';
- exactly one valid 'Sec-WebSocket-Accept' value equal to Base64(SHA-1(nonce +
  the RFC 6455 GUID));
- when 'Sec-WebSocket-Protocol' is present, exactly one HTTP token that
  case-sensitively equals an offered protocol;
- no 'Sec-WebSocket-Extensions' field.

The parser enforces the HTTP client's safe header byte/count limits, rejects
malformed or ambiguous duplicate fields, and retains any bytes after the empty
line. Those bytes are given to the session before it starts a subsequent read,
so a server frame coalesced with the 101 response is not lost.

Any other response, including redirects and 401/407, is a terminal failed
upgrade. Error detail may include the status, but never credentials or
unbounded response data.

## Shared Session and Client Framing Role

The result reuses 'uvp::websocket::session'; it must not introduce a parallel
'client_session'. The private frame state gains a role selected only by a
session factory:

~~~
server role: incoming frames must be masked; outgoing frames are unmasked
client role: incoming frames must be unmasked; outgoing frames are masked
~~~

All other behavior stays shared: RSV and opcode validation, control-frame
rules, fragmentation, message limit, UTF-8 and close-code validation, ping/pong
policy, close timer, callbacks, and bounded writes.

For every client-role outbound frame, the implementation obtains a new
unpredictable four-byte masking key from the platform cryptographic random
source. It writes the key in the frame header and XORs the payload before that
encoded frame enters the queue. The encoded size, including header and mask,
counts toward the pending-write limit. A predictable PRNG, a fixed key, or a
reused key is unacceptable. Client-role input rejects a masked server frame
with a protocol-error close; server-role input keeps rejecting unmasked client
frames.

The SHA-1/Base64 helpers should be consolidated in private WebSocket handshake
code. SHA-1 remains solely an RFC 6455 compatibility algorithm, not a general
application hashing API.

## Timeouts, Cancellation, and Ownership

A zero timeout means disabled, consistent with current client options.
'overall_timeout' covers the whole attempt. The active phase timeout changes as
the state enters DNS, TCP, TLS, and WebSocket handshake work. The WebSocket
handshake timeout covers both request write and receipt of a complete validated
response header.

Cancellation is valid in every phase. It cancels active DNS/TCP/TLS children,
stops deadlines, closes any stream still owned by the connect operation, and
reports 'client_errc::cancelled'. If success, cancellation, and a deadline race,
only the first terminal event invokes the callback. Once the successful callback
begins, ownership has moved to the returned session; cancelling its completed
operation cannot close the established WebSocket.

The public category must distinguish invalid URL/configuration, DNS/connect/TLS
failure, invalid handshake response, cancellation, and timeout. The associated
detail preserves the causal phase or lower-layer error without exposing private
parser or OpenSSL types as API requirements.

## Backpressure and Session Lifetime

The upgrade request is fixed-size and response headers are bounded, so this
operation does not need a general HTTP body writer. It still waits for the
lower-stream write completion before leaving the write phase.

After handoff, the session's existing queue is authoritative: it retains
encoded frames until lower-stream write completion and bounds pending encoded
bytes. Extract shared accounting only if the client role proves a concrete
common boundary; do not introduce a generic framing framework merely for this
proposal.

The returned session follows 'accept()' ownership rules. Callback-only detached
client connections are intentionally not part of the first API; users retain
the session in application state. That avoids a second lifetime convention
without a demonstrated need.

## Implementation Plan

1. Extract private RFC 6455 helpers shared by server validation and client
   request/response validation: token lists, canonical Base64, accept digest,
   and HTTP-token checks.
2. Make frame state role-aware and add client masking from a cryptographic
   random source. Preserve every existing server framing test.
3. Add client state, options, error category, URL expansion, deadlines,
   cancellation, direct TCP composition, and 'wss' TLS composition.
4. Add bounded HTTP/1.1 upgrade serialization and response-head parsing; only
   after complete validation transfer stream and unread bytes to a client-role
   session.
5. Add user documentation and an echo-client example that retains the session.
6. When the outbound connector lands, add routes without changing WebSocket
   handshake or session semantics.

## Tests

Use a local controllable TCP/TLS peer as well as an in-process WebSocket server
so malformed upgrade and framing cases are deterministic.

- URL validation: only ws/wss with authority; default/explicit ports;
  origin-form target; rejected userinfo and unsupported schemes.
- Direct ws and wss success, including SNI, verification, and HTTP/1.1 ALPN.
- Exact request serialization: Host, fresh canonical key, upgrade tokens,
  optional Origin/subprotocols, no fragment, and no caller override of
  reserved fields.
- Each 101 validation rule: status, token lists, accept digest, duplicate or
  malformed fields, selected subprotocol, and unsupported extensions.
- A frame coalesced after 101 reaches the session; responses split at every
  header boundary still succeed.
- Client output is masked with a different valid key per frame; client input
  rejects masked server frames; server-role masking tests still pass.
- Text/binary, fragmentation, ping/pong, UTF-8, close handshake, close
  timeout, and byte-stream adaptation behave identically after client handoff.
- Limits include client-frame encoding and fail without unbounded growth.
- Cancellation and all timeout phases complete exactly once, close partial
  transports, and never issue a late success callback.
- Destruction of an owning returned session closes its transport; cancelling a
  completed operation does not.

## Acceptance Criteria

Milestone 7 WebSocket-client work is complete when a direct ws:// or wss:// URL
produces a retained 'uvp::websocket::session' only after a fully validated RFC
6455 handshake; that session applies client masking; and malformed input,
resource limits, cancellation, and deadlines have deterministic exactly-once
tests.

## Source Documents

- [WebSocket design](../design/websocket.md)
- [Protocol composition](../design/protocol-composition.md)
- [Shared protocol foundation](shared-protocol-foundation.md)
- [Outbound connector and proxy routes](outbound-connector-and-proxy-routes.md)
- [Operation lifetime](../design/operation-lifetime.md)
- [Operation deadlines](../design/operation-deadline.md)
- [User WebSocket documentation](../user/websocket.md)
