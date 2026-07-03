# Multipart Handling Proposal

Status: Implemented for the Milestone 4 route-policy surface

## Current State

- Implemented: request body streaming, bounded buffered body policies, internal
  multipart streaming parser, `body::multipart_stream`, streaming part APIs,
  `body::multipart_form`, owning collected form model, non-owning form views,
  `multipart_stream_options`, `multipart_form_options`, `multipart_limits`,
  multipart error reporting, size-limit status mapping, stream backpressure,
  and multipart unit/integration tests.
- Deferred: schema-level per-field/per-file rules, server-wide multipart
  default inheritance, timeout-between-boundaries policy, and late
  `req.form()` / `req.multipart_stream()` helper entrypoints.

## Scope

Multipart support is an HTTP body decoder for `multipart/form-data` uploads. It
should cover two application shapes:

- streaming uploads, where each part is delivered as it arrives and file bytes
  are not buffered by default;
- bounded form parsing, where small fields and explicitly allowed small files
  are collected into a structured form object before the route handler runs.

Implementation should land in two steps:

1. `multipart_stream`: internal streaming parser, part callbacks, backpressure,
   multipart error reporting, and parser state-machine tests.
2. `multipart_form`: bounded in-memory collection using the same internal
   multipart syntax rules, immutable form model, memory-safe defaults, and
   integration tests.

Multipart response generation and generic non-form multipart variants can be
added later if a concrete use case appears.

## Body Policies

Multipart should follow the existing HTTP route body policy model. Routes
declare multipart handling explicitly:

```cpp
srv.post("/upload",
  uvp::http::body::multipart_stream{},
  [](uvp::http::request& req, uvp::http::response& res, uvp::http::multipart_stream& mp) {
    // Register part callbacks.
  });

srv.post("/profile",
  uvp::http::body::multipart_form{},
  [](uvp::http::request& req, uvp::http::response& res, const uvp::http::multipart_form& form) {
    // Read the completed bounded form.
  });
```

This keeps body ownership visible at route declaration time. The server can
reject unsupported content types, enforce multipart limits, decide when to
dispatch the handler, and preserve the existing distinction between buffered
and streaming request bodies.

Avoid a late `request::multipart()` API for primary routing. It would make the
server read policy harder to see and would push content-type validation and
buffering decisions into application code after the HTTP body has already
started.

`multipart_form` may be inferred from a handler that accepts
`const multipart_form&`, because it follows the buffered body path and can
answer parse errors before handler entry. `multipart_stream` remains explicit:
its response ownership and `on_error()` requirements are specific enough that
the route should declare it.

Route policies remain the primary API. Helper shapes such as `req.form()` or
`req.multipart_stream()` may be used only as explicit multipart entrypoints
that follow the same ownership and error-response rules; they must not hide a
late asynchronous body read behind a generic request API.

## Result And Error Model

Multipart introduces a small protocol-neutral result shape that can later be
reused by TLS, WebSocket, or other modules:

```cpp
namespace uvp {

struct error {
  std::error_code code;
  std::string detail;
};

template<class T>
class result; // expected-compatible success-or-error value

} // namespace uvp
```

The project currently targets C++20, while `std::expected` is C++23. Keep the
project on C++20 for this milestone and provide a small project-local
expected-compatible `uvp::result<T>` with the same success/error shape and no
third-party dependency. A future C++23 migration can turn it into an alias for
`std::expected`.

HTTP-specific parser errors use a standard error category:

```cpp
namespace uvp::http {

enum class errc {
  malformed_content_type,
  unsupported_media_type,

  multipart_missing_boundary,
  multipart_invalid_boundary,
  multipart_malformed_body,
  multipart_malformed_part_header,
  multipart_unexpected_end,
  multipart_limit_exceeded,
};

std::error_code make_error_code(errc);

} // namespace uvp::http
```

`uvp::error::detail` carries diagnostic text for logs and application responses.
The public HTTP status mapping remains separate from the low-level error code.

## Response Ownership

Automatic HTTP error responses are allowed only while the framework still owns
response responsibility:

1. Parsing before the handler, or a non-streaming helper such as `req.form()`,
   may produce a default HTTP error response.
2. In multipart streaming, once the handler has received
   `multipart_stream&`, parser errors are delivered to the application.
3. After `res.defer()`, the application owns completion and must respond.
4. After response writing starts, no automatic HTTP error response is possible.
5. In streaming mode, missing `mp.on_error()` is an application programming
   error. The implementation should log explicit detail and close the
   connection in a controlled way.

For `multipart_form`, the framework can answer by default unless a form
`on_error()` hook is configured. For `multipart_stream`, `on_error()` is
mandatory because parsing continues after control has entered the application
handler.

## Streaming API

`body::multipart_stream{}` dispatches after request headers are complete and
after the server has validated that the request is a `multipart/form-data`
request with a usable boundary.

The intended application shape is:

```cpp
srv.post("/upload",
  uvp::http::body::multipart_stream{},
  [](uvp::http::request&, uvp::http::response& res, uvp::http::multipart_stream& mp) {
    auto reply = std::make_shared<uvp::http::deferred_response>(res.defer());

    mp.on_part([reply](uvp::http::multipart_part& part) {
      if (part.filename()) {
        auto file = open_upload_sink(part.name(), part.safe_filename());

        part.stream()
          .on_data([file](std::span<const std::byte> chunk) mutable {
            file.write(chunk);
          })
          .on_end([file]() mutable {
            file.close();
          })
          .on_error([file](std::error_code) mutable {
            file.abort();
          });
      } else if (part.name() == "title") {
        part.text(64 * 1024, [](uvp::result<std::string> value) {
          // Field complete.
        });
      } else {
        part.discard();
      }
    });

    mp.on_end([reply] {
      if (reply->active()) {
        reply->status(uvp::http::status::created).text("uploaded");
      }
    });

    mp.on_error([reply](std::error_code) {
      if (reply->active()) {
        reply->status(400).json(uvp::json{{"error", "upload failed"}});
      }
    });
  });
```

A multipart part exposes metadata and exactly one consumption path:

```cpp
class multipart_part {
public:
  std::string_view name() const noexcept;
  std::optional<std::string_view> filename() const noexcept;
  std::string safe_filename() const;
  const http::headers& headers() const noexcept;

  multipart_part_stream& stream();
  void text(std::size_t max_bytes, std::function<void(uvp::result<std::string>)> callback);
  void discard();

  void pause();
  void resume();
};
```

The exact return types can evolve, but the contract should stay strict:

- file parts are not buffered by default;
- text buffering is opt-in and must provide a per-field byte limit;
- `stream()`, `text()`, and `discard()` are mutually exclusive;
- a part must be consumed, discarded, or rejected before the parser advances to
  the next part.

Bounded text reads report parser errors, size-limit failures, and disconnects
with `uvp::result<std::string>` instead of throwing from the request body
callback.

## Unhandled Parts

The default unhandled-part policy should be conservative:

```cpp
enum class unhandled_part_policy {
  reject,
  discard,
};
```

`reject` should be the default. It prevents an endpoint from accidentally
accepting and draining a large unexpected upload. Applications that intentionally
ignore unknown fields can opt into `discard`, or call `part.discard()` for each
unknown field they choose to tolerate.

If a callback returns without selecting a consumption path and the policy is
`reject`, the stream should fail with a multipart error. If the policy is
`discard`, the parser may drain the part while still enforcing total upload,
part, timeout, and file limits.

## Backpressure

Multipart streaming must preserve request-body backpressure. Calling
`part.pause()` or `part.stream().pause()` stops consuming additional body bytes
after the current callback. Calling `resume()` restarts parsing.

This is stronger than merely suppressing callbacks. The parser must be able to:

- stop in the middle of an already received body chunk;
- keep any unconsumed bytes for later parsing;
- avoid emitting the next part while the current part is paused;
- propagate pause and resume to the underlying `request_body_stream`, so the
  HTTP session stops reading from the transport when application sinks are full.

Part-level pause and stream-level pause may be aliases, but the semantics should
be documented in one place.

## Buffered Form API

`body::multipart_form{}` buffers a bounded form and dispatches after the
multipart message is complete. It is for small HTML-style forms and tightly
bounded metadata uploads, not arbitrary file ingestion.

The default policy accepts text fields up to safe small limits and rejects file
buffering unless a file field is explicitly allowed:

```cpp
auto policy = uvp::http::body::multipart_form{}
  .max_total_bytes(16 * 1024 * 1024)
  .max_memory_bytes(8 * 1024 * 1024)
  .max_field_bytes(1024 * 1024)
  .max_file_bytes(0);

srv.post("/profile", policy, [](uvp::http::request&, uvp::http::response& res,
                                const uvp::http::multipart_form& form) {
  auto display_name = form.first_field("display_name");
  auto tags = form.fields("tag");

  res.status(204).end();
});
```

`multipart_form` is an owning immutable representation of a collected
`multipart/form-data` body. It preserves original part order and repeated field
names. Fields and files are exposed through non-owning view types that remain
valid only while the owning `multipart_form` remains alive and unmoved.

The API never collapses repeated fields into a single map value. Callers choose
explicitly between first, single, and multi-value access:

```cpp
class multipart_form {
public:
  std::optional<multipart_field_view> first_field(std::string_view name) const;
  uvp::result<multipart_field_view> single_field(std::string_view name) const;
  std::span<const multipart_field_view> fields(std::string_view name) const;

  std::optional<multipart_file_view> first_file(std::string_view name) const;
  uvp::result<multipart_file_view> single_file(std::string_view name) const;
  std::span<const multipart_file_view> files(std::string_view name) const;

  std::span<const multipart_part_view> parts() const noexcept;
};
```

A part is considered a file when its `Content-Disposition` contains a
`filename` parameter. Otherwise it is considered a field. Field values are
exposed as raw bytes, with `std::string_view` convenience access that does not
validate, transcode, or decode charset metadata.

File buffering in `multipart_form` should require an explicit field rule once
the API grows enough to support schemas:

```cpp
body::multipart_form{}
  .field("display_name", field_rule{}.required().max_bytes(64 * 1024))
  .file("avatar", file_rule{}.optional().max_bytes(2 * 1024 * 1024));
```

Until that schema surface exists, a global non-zero `max_file_bytes` can allow
small files, but the documentation should warn that streaming is preferred for
untrusted or large files.

## Validation Layers

Multipart parsing and application validation are separate concerns.

The parser should validate:

- content type and boundary presence;
- multipart syntax;
- per-part header syntax and header limits;
- total body, part, field, file, and count limits;
- upload and inter-boundary timeouts;
- client disconnects and parser errors.

The application should validate:

- required business fields;
- repeated-field meaning;
- accepted field names;
- accepted content types for file fields;
- final storage path, virus scanning, checksums, or other domain rules.

The library may provide optional form schema helpers later, but the initial
multipart parser should not become a full validation framework.

## Multipart Syntax

Multipart parsing follows RFC 7578 for `multipart/form-data` and RFC 2046 for
boundary handling. The implementation accepts quoted and unquoted `boundary`
parameters, validates the decoded boundary value, and recognizes delimiters
only at the beginning of the body or immediately after CRLF.

Each part must contain exactly one `Content-Disposition` header with
disposition type `form-data` and exactly one `name` parameter. `filename` is
optional. Duplicate `Content-Disposition`, duplicate `Content-Type`, and
duplicate disposition parameters are malformed input. Multiple parts with the
same name are allowed.

Part headers are parsed as a bounded MIME-like header block. Header names are
case-insensitive. Obsolete folded headers are not supported.
`Content-Transfer-Encoding` is not decoded; non-trivial encodings are rejected.

Quoted-string parameter values are supported, including backslash-escaped
characters. CR, LF, NUL, and unterminated quoted strings are rejected.

## Security And Limits

Multipart defaults should be safe for small services and explicit for expensive
behavior. The option set should cover:

- total multipart body byte limit;
- total collected memory byte limit;
- per-file byte limit;
- per-field buffered byte limit;
- per-part header byte limit;
- per-part header count limit;
- field count and file count limits;
- total part count limit;
- maximum field name length;
- maximum filename length;
- upload timeout;
- timeout between two boundaries;
- unhandled-part policy;
- repeated-field policy.

Multipart limits are part of the HTTP API contract. They are configured through
immutable option structs on the route body policy and passed to the parser as a
snapshot. The low-level parser receives effective limits and does not own
defaults. Limits cannot be changed after multipart parsing has started.

`multipart_stream` defaults are streaming-oriented: structural limits are
enforced, text fields are limited to 1 MiB, and file and total body size limits
are application-configurable. Parser errors are delivered to the application by
default.

`multipart_form` defaults are memory-safe: files are rejected by default, fields
are collected in memory with a 1 MiB per-field limit, an 8 MiB memory limit, and
a 16 MiB total body limit. When a route does not set `route_options`
`max_body_bytes`, `body::multipart_form{}` uses its total multipart limit as
the route buffering limit.

The server should reject:

- a `multipart/form-data` request without a boundary;
- an unsupported or malformed boundary;
- malformed multipart framing;
- oversized part headers;
- too many parts, fields, or files;
- a file or field that exceeds its configured limit;
- a route declared as `multipart_form` when the request would require
  unbounded buffering.

Recommended HTTP status mapping:

- `400 Bad Request` for malformed multipart data;
- `408 Request Timeout` for upload or boundary timeouts;
- `413 Payload Too Large` for size and count limits;
- `415 Unsupported Media Type` for a route that requires multipart but receives
  another content type;
- `422 Unprocessable Entity` may be used by application code for missing or
  invalid business fields after parsing succeeds.

Filenames are metadata, not paths. The public API should expose the raw
filename, when present, but applications must not use it directly as a storage
path. Provide a `safe_filename()` helper that strips path separators, control
characters, and other unsafe bytes, while documenting that applications may
still need stronger domain-specific normalization.

Support for `filename*` and charset-aware field decoding can be added later.
The initial design may treat field and filename values as byte strings exposed
as UTF-8-like `std::string_view` only when the parser can do so without
transcoding. Any lossy decoding should be opt-in.

## Parser Boundary

Multipart parsing is security-sensitive, but no adequate low-risk dependency is
selected for this milestone. Implement a private internal parser rather than
adding a fragile dependency or buffering complete uploads. Keep the parser
private to the multipart or HTTP detail layer and test it as a state machine
with fragmented boundaries, partial headers, malformed inputs, and backpressure
pauses at every transition.

The parser must not own the socket, event loop, timers, or HTTP session. It
should consume byte spans, emit typed events, and let the HTTP session own
transport reads, timeout handles, response errors, and keep-alive sequencing.

## Tests

Parser state-machine tests should cover:

- quoted and unquoted boundary parameters;
- invalid, missing, and unterminated boundaries;
- delimiter recognition at body start and after CRLF only;
- fragmented delimiters, headers, and body bytes across arbitrary chunks;
- malformed framing, unexpected EOF, and final boundary handling;
- duplicate `Content-Disposition`, duplicate `Content-Type`, and duplicate
  disposition parameters;
- missing disposition, non-`form-data` disposition, and missing `name`;
- quoted-string escapes and rejection of CR, LF, NUL, or unterminated quotes;
- rejected obsolete folded headers;
- rejected non-trivial `Content-Transfer-Encoding`;
- field, file, part-header, part-count, memory, and total-size limits;
- pause/resume at every parser transition.

HTTP integration tests should cover:

- `multipart_stream` dispatch after header validation;
- mandatory streaming `on_error()` behavior;
- stream part `on_data`, `on_end`, `on_error`, `discard`, and text helper paths;
- backpressure propagation to request-body reads;
- automatic error responses before handler entry;
- no automatic error response after handler entry, `res.defer()`, or response
  writes;
- `multipart_form` collection of repeated fields and original part order;
- files rejected by default in `multipart_form`;
- explicitly allowed small files in `multipart_form`;
- route and default limit inheritance snapshots.

## Relationship To WebSocket

Multipart was not required for server-side WebSocket support, but deciding the
body policy shape prevents the HTTP API from drifting. WebSocket uses HTTP
upgrade hooks, while multipart needs HTTP body streaming and buffering hooks.
Both features benefit from keeping protocol-specific state machines behind
explicit route policies and avoiding hidden request APIs that start asynchronous
work late.
