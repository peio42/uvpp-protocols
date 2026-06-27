# Multipart Handling Proposal

Status: Draft, not implemented

## Current State

- Implemented: request body streaming and bounded buffered body policies.
- Not implemented: multipart parser, `body::multipart_stream`,
  `body::multipart_form`, part APIs, and multipart limits.

## Scope

Multipart support is an HTTP body decoder for `multipart/form-data` uploads. It
should cover two application shapes:

- streaming uploads, where each part is delivered as it arrives and file bytes
  are not buffered by default;
- bounded form parsing, where small fields and explicitly allowed small files
  are collected into a structured form object before the route handler runs.

The first implementation should focus on server-side request parsing. Multipart
response generation and generic non-form multipart variants can be added later
if a concrete use case appears.

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

Typed multipart policies should not be inferred from handler signatures. Like
future `body::json<T>` policies, multipart decoding is specific enough that the
route should declare it explicitly.

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
        part.text(64 * 1024, [](result<std::string> value) {
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
  void text(std::size_t max_bytes, std::function<void(result<std::string>)> callback);
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

`result<std::string>` is a design placeholder for the eventual asynchronous
success-or-error value type. The important part is that bounded text reads
report parser errors, size-limit failures, and disconnects without throwing from
the request body callback.

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

The default policy should accept text fields up to safe small limits and reject
file buffering unless a file field is explicitly allowed:

```cpp
auto policy = uvp::http::body::multipart_form{}
  .max_total_bytes(8 * 1024 * 1024)
  .max_field_bytes(64 * 1024)
  .max_file_bytes(0)
  .max_parts(64)
  .repeated_fields(uvp::http::repeated_field_policy::collect);

srv.post("/profile", policy, [](uvp::http::request&, uvp::http::response& res,
                                const uvp::http::multipart_form& form) {
  auto display_name = form.field("display_name");
  auto tags = form.fields("tag");
  auto avatar = form.file("avatar");

  res.status(204).end();
});
```

The structured form should preserve repeated fields:

```cpp
class multipart_form {
public:
  std::optional<std::string_view> field(std::string_view name) const;
  std::span<const multipart_field> fields(std::string_view name) const;

  std::optional<multipart_file_view> file(std::string_view name) const;
  std::span<const multipart_file_view> files(std::string_view name) const;
};
```

`field(name)` should follow the configured repeated-field policy. The
multi-value accessors should always be available so applications can implement
their own validation when the policy is permissive.

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

## Security And Limits

Multipart defaults should be safe for small services and explicit for expensive
behavior. The option set should cover:

- total multipart body byte limit;
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

Multipart parsing is security-sensitive. Prefer a mature streaming parser if a
small C or C++ dependency fits the project dependency policy. If the project
implements the parser itself, keep it private to the multipart or HTTP detail
layer and test it as a state machine with fragmented boundaries, partial
headers, malformed inputs, and backpressure pauses at every transition.

The parser must not own the socket, event loop, timers, or HTTP session. It
should consume byte spans, emit typed events, and let the HTTP session own
transport reads, timeout handles, response errors, and keep-alive sequencing.

## Relationship To WebSocket

Multipart was not required for server-side WebSocket support, but deciding the
body policy shape prevents the HTTP API from drifting. WebSocket uses HTTP
upgrade hooks, while multipart needs HTTP body streaming and buffering hooks.
Both features benefit from keeping protocol-specific state machines behind
explicit route policies and avoiding hidden request APIs that start asynchronous
work late.
