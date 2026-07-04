# Static File Helper Proposal

Status: Implemented for the initial route helper

## Context

The HTTP server design lists a static file helper as future work. Applications
can already serve files by writing a route handler that resolves a path, reads a
file, sets headers, and calls `response::bytes()` or `response::stream()`, but
the safe version of that handler is easy to get subtly wrong.

The helper should provide a small, conservative way to expose files under an
explicit directory root. It is not a CDN, asset compiler, template renderer, or
general filesystem browser. It is an HTTP route helper that composes with the
existing router, hooks, response ownership model, and HEAD fallback behavior.

## Current State

- Implemented: wildcard routes, decoded path segments, response headers,
  buffered responses, streaming responses, HEAD requests falling back to GET
  routes, route hooks, response hooks, `static_files()`, filesystem path
  confinement, extension content-type mapping, cache validators, and
  static-file integration tests.
- Deferred: range requests, precompressed variants, a public MIME registry,
  a public virtual filesystem interface, and a non-blocking filesystem backend.

## Scope

The first implementation should cover:

- serving regular files from an explicit root directory;
- route-level usage with the existing `server::get()` / `router::get()` API;
- path traversal prevention, including decoded `..` segments and encoded path
  separators;
- optional `index.html` resolution for directory requests;
- hidden-file rejection by default;
- basic content-type detection by extension;
- `Content-Length`, `Last-Modified`, weak `ETag`, and conditional `304 Not
  Modified` responses;
- configurable `Cache-Control`;
- GET and HEAD behavior through the existing server machinery;
- bounded chunked file sending without buffering whole files into memory;
- integration tests for success, rejection, headers, and route interaction.

The helper should not add a new public filesystem abstraction in this milestone.
Any file reader used by the implementation should remain private so it can move
from standard-library or libuv-backed reads to a richer uvpp filesystem wrapper
without changing the HTTP API.

## Public API Shape

The preferred API is a callable route helper:

```cpp
uvp::http::server srv(loop);

srv.get(
  "/assets/*path",
  uvp::http::static_files("public")
    .cache_control("public, max-age=3600")
    .index_file("index.html"));
```

`static_files(root)` returns a moveable/copyable handler object whose call
operator accepts `(request&, response&)`. This keeps static serving visible in
the route table and avoids another server-owned mount subsystem.

Sketch:

```cpp
namespace uvp::http {

enum class hidden_file_policy {
  reject,
  allow,
  allow_well_known,
};

enum class symlink_policy {
  follow_within_root,
  reject,
};

class static_file_options {
public:
  static_file_options& path_param(std::string value) &;
  static_file_options&& path_param(std::string value) &&;

  static_file_options& index_file(std::string value) &;
  static_file_options&& index_file(std::string value) &&;
  static_file_options& no_index_file() & noexcept;
  static_file_options&& no_index_file() && noexcept;

  static_file_options& hidden_files(hidden_file_policy value) & noexcept;
  static_file_options&& hidden_files(hidden_file_policy value) && noexcept;

  static_file_options& symlinks(symlink_policy value) & noexcept;
  static_file_options&& symlinks(symlink_policy value) && noexcept;

  static_file_options& cache_control(std::string value) &;
  static_file_options&& cache_control(std::string value) &&;
  static_file_options& no_cache_control() & noexcept;
  static_file_options&& no_cache_control() && noexcept;

  static_file_options& etag(bool value) & noexcept;
  static_file_options&& etag(bool value) && noexcept;
  static_file_options& last_modified(bool value) & noexcept;
  static_file_options&& last_modified(bool value) && noexcept;
  static_file_options& nosniff(bool value) & noexcept;
  static_file_options&& nosniff(bool value) && noexcept;

  static_file_options& chunk_size(std::size_t value) &;
  static_file_options&& chunk_size(std::size_t value) &&;

  [[nodiscard]] std::string_view path_param() const noexcept;
  [[nodiscard]] std::optional<std::string_view> index_file() const noexcept;
  [[nodiscard]] hidden_file_policy hidden_files() const noexcept;
  [[nodiscard]] symlink_policy symlinks() const noexcept;
  [[nodiscard]] std::optional<std::string_view> cache_control() const noexcept;
  [[nodiscard]] bool etag() const noexcept;
  [[nodiscard]] bool last_modified() const noexcept;
  [[nodiscard]] bool nosniff() const noexcept;
  [[nodiscard]] std::size_t chunk_size() const noexcept;
};

class static_file_handler {
public:
  static_file_handler(std::filesystem::path root, static_file_options options = {});

  static_file_handler& options(static_file_options value) &;
  static_file_handler&& options(static_file_options value) &&;

  static_file_handler& path_param(std::string value) &;
  static_file_handler&& path_param(std::string value) &&;
  static_file_handler& index_file(std::string value) &;
  static_file_handler&& index_file(std::string value) &&;
  static_file_handler& no_index_file() & noexcept;
  static_file_handler&& no_index_file() && noexcept;
  static_file_handler& hidden_files(hidden_file_policy value) & noexcept;
  static_file_handler&& hidden_files(hidden_file_policy value) && noexcept;
  static_file_handler& symlinks(symlink_policy value) & noexcept;
  static_file_handler&& symlinks(symlink_policy value) && noexcept;
  static_file_handler& cache_control(std::string value) &;
  static_file_handler&& cache_control(std::string value) &&;
  static_file_handler& no_cache_control() & noexcept;
  static_file_handler&& no_cache_control() && noexcept;
  static_file_handler& etag(bool value) & noexcept;
  static_file_handler&& etag(bool value) && noexcept;
  static_file_handler& last_modified(bool value) & noexcept;
  static_file_handler&& last_modified(bool value) && noexcept;
  static_file_handler& nosniff(bool value) & noexcept;
  static_file_handler&& nosniff(bool value) && noexcept;
  static_file_handler& chunk_size(std::size_t value) &;
  static_file_handler&& chunk_size(std::size_t value) &&;

  void operator()(request& req, response& res) const;

  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] const static_file_options& options() const noexcept;
};

[[nodiscard]] static_file_handler static_files(
  std::filesystem::path root,
  static_file_options options = {});

} // namespace uvp::http
```

`static_file_handler` mirrors the option setters so the common route-site shape
stays compact. Applications that prefer to share a policy value can configure a
`static_file_options` object and pass it to `static_files(root, options)`.

Default options:

- `path_param = "path"`;
- `index_file = "index.html"`;
- `hidden_files = hidden_file_policy::reject`;
- `symlinks = symlink_policy::follow_within_root`;
- `cache_control = "no-cache"`;
- `etag = true`;
- `last_modified = true`;
- `nosniff = true`;
- `chunk_size = 64 * 1024`.

The route pattern should normally end in a named wildcard with the same name as
`path_param`:

```cpp
srv.get("/static/*path", uvp::http::static_files("./static"));
```

If applications prefer a different wildcard name, configure it explicitly:

```cpp
srv.get(
  "/downloads/*file",
  uvp::http::static_files("./downloads")
    .path_param("file")
    .no_index_file());
```

Construction should reject an empty root path and invalid option values such as
an empty `path_param`, an `index_file` containing a path separator, or a
`chunk_size` of zero. A missing or non-directory root is a setup failure and may
throw `std::filesystem_error` or `std::invalid_argument`.

## Route Integration

The helper should be ordinary route code. It must not bypass request hooks,
pre-handler hooks, response hooks, route groups, mounted routers, server
exception handling, keep-alive policy, or pending-write limits.

`HEAD` is already handled by the server by falling back to a matching GET route
and suppressing the serialized response body. The static helper should not need
to register a separate HEAD route. It should still compute the same headers as
GET, including `Content-Length`, validators, content type, and cache headers.

The server's automatic `405 Method Not Allowed` response remains responsible
for unsupported methods. If only the static GET route matches, the existing
`Allow` header should include `GET` and `HEAD`.

## Path Resolution

Path resolution is the security-critical part of this helper. The implementation
must not concatenate `req.params().get("path")` directly onto the root. Wildcard
parameters join decoded segments with `/`, which loses the distinction between
a real path separator and an encoded slash inside one original URL segment.

Instead, the helper should derive the matched wildcard tail from
`req.decoded_path_segments()` and `req.matched_pattern()`:

1. Parse the matched route pattern into route segments.
2. Find the final `*<path_param>` segment.
3. Count the route segments before that wildcard.
4. Use the request's decoded path segments after that count as filesystem path
   components.

Each request component is accepted only if:

- it is not empty;
- it is not `.` or `..`;
- it contains no NUL byte;
- it contains no `/` or `\` after percent-decoding;
- on Windows, it is not a drive prefix, UNC prefix, alternate data stream name,
  or other absolute path escape.

This means `/assets/a%2Fb` must not serve `root/a/b`. Encoded separators should
be rejected or treated as not found.

After component validation, build the candidate path by appending components to
the configured root. Resolve the root to a canonical absolute path. Resolve the
candidate, or its nearest existing parent, and verify that the final target is
still under the canonical root before opening it.

Symlink handling follows `symlink_policy`:

- `follow_within_root`: symlinks may be followed only when the resolved target
  stays under the canonical root;
- `reject`: any symlink encountered in the selected path is rejected.

This is path confinement, not a process sandbox. The proposal does not claim to
defend against every time-of-check/time-of-use race on a hostile filesystem. It
is intended for serving an application-owned directory tree.

## Directory And Hidden File Behavior

Directory listing is never enabled by default and is outside the initial scope.

When the resolved target is a directory:

- if `index_file()` has a value, try that file inside the directory;
- if the index file does not exist or is not a regular file, return the normal
  not-found response;
- if `index_file()` is disabled, return the normal not-found response.

Hidden file policy is checked for every path component, including index files:

- `reject`: reject any component whose first character is `.`;
- `allow`: do not apply hidden-file rejection;
- `allow_well_known`: allow a leading `.well-known` component at the static
  root, but reject other hidden components.

The default should reject hidden files. Serving `.well-known` paths is useful
for ACME challenges and similar protocols, but it should be an explicit choice.

## Response Headers

Successful file responses should set:

```http
Content-Type: <detected type>
Content-Length: <file size>
Last-Modified: <http-date>
ETag: W/"<implementation-defined validator>"
Cache-Control: no-cache
X-Content-Type-Options: nosniff
```

`Last-Modified` and `ETag` are controlled by options. `Cache-Control` is set
only when the configured value is present. `X-Content-Type-Options: nosniff` is
enabled by default because static serving often exposes browser-executed
content.

The weak ETag can be derived from stable file metadata such as size and modified
time. It does not need to hash the entire file in the first implementation.

If an application sets a header before the static helper runs, the helper should
not silently overwrite it unless the header is essential to correctness. A
reasonable rule is:

- preserve application-provided `Cache-Control`;
- preserve application-provided `Content-Type`;
- overwrite `Content-Length`, `ETag`, and `Last-Modified` only when the helper
  owns the successful file response.

## Content Types

The first implementation should ship a small built-in extension map:

| Extension | Content-Type |
| --- | --- |
| `.html`, `.htm` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js`, `.mjs` | `text/javascript; charset=utf-8` |
| `.json`, `.map` | `application/json; charset=utf-8` |
| `.txt` | `text/plain; charset=utf-8` |
| `.svg` | `image/svg+xml` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.webp` | `image/webp` |
| `.ico` | `image/x-icon` |
| `.wasm` | `application/wasm` |
| `.pdf` | `application/pdf` |
| `.xml` | `application/xml; charset=utf-8` |

Unknown extensions should use `application/octet-stream`. Do not sniff file
contents.

A custom MIME registry can be added later if a concrete use case appears. The
initial implementation can keep the map private.

## Conditional Requests

When validators are enabled, the helper should handle conditional GET and HEAD
before opening and streaming the file body:

- `If-None-Match` takes precedence over `If-Modified-Since`;
- a matching ETag returns `304 Not Modified`;
- when no ETag condition is present, an `If-Modified-Since` value at or after
  the file modification time returns `304 Not Modified`;
- malformed conditional headers are ignored rather than producing `400`.

`304` responses should include the same cache validators that would have been
sent for `200`, but no `Content-Length` for the file body.

Range requests are outside the initial scope. The first implementation should
ignore `Range` and return `200 OK` with the full file. If range support is added
later, it should support at least single byte ranges, `206 Partial Content`, and
`416 Range Not Satisfiable` before advertising `Accept-Ranges`.

## Status Mapping

The default mapping should avoid revealing details about the filesystem:

| Condition | Status |
| --- | --- |
| file served | `200 OK` |
| conditional validator matches | `304 Not Modified` |
| target missing | `404 Not Found` |
| target is not a regular file and no index applies | `404 Not Found` |
| rejected traversal, hidden path, or outside-root symlink | `404 Not Found` |
| root cannot be read because of permissions | `500 Internal Server Error` |
| selected file disappears during send | close or error the response stream |
| read failure after headers are committed | report stream error and close |

Using `404` for rejected paths keeps traversal and hidden-file probes from
learning which directories exist. A future diagnostic option may expose `403`
for applications that prefer explicit forbidden responses.

## Sending Model

The helper should avoid `response::bytes()` for successful file bodies unless
the file is known to be tiny. The default path should use `response::stream()`
and send chunks of at most `chunk_size`.

The sender owns:

- the opened file;
- a `streaming_response` handle;
- the current offset;
- the configured chunk buffer;
- completion, drain, cancel, and error callbacks.

The sender writes until EOF or until `stream_write_result` reports accepted
backpressure. On backpressure, it waits for `on_drain()` before reading and
writing the next chunk. On normal EOF, it ends the stream. On cancellation, it
closes the file and drops pending work.

If the implementation initially uses blocking file reads, those reads must be
bounded to one chunk at a time and the documentation should say that this helper
is suitable for application assets, not high-throughput file distribution. If a
private libuv-backed file reader is available, prefer it. In either case, the
public API should not expose the backend choice.

## Error And Exception Model

Setup errors follow the project API principles: invalid configuration throws
during construction or route registration.

Per-request filesystem failures should normally be converted to HTTP responses
while headers are still uncommitted. Once streaming headers have been committed,
the helper cannot synthesize a new HTTP error response. It should notify the
underlying stream error path and close the response, matching the existing
streaming response model.

The helper should not call the server exception handler for ordinary not-found
or rejected-path cases. Unexpected filesystem exceptions before headers commit
may be translated to `500 Internal Server Error`; truly unexpected programming
errors can continue through the normal route exception policy.

## Out Of Scope

- Directory listings.
- Template rendering.
- Asset fingerprints, bundling, transpilation, minification, or compression.
- Precompressed variant negotiation such as `.br` or `.gz`.
- Range requests in the first implementation.
- Uploads, writes, deletes, or WebDAV-style filesystem operations.
- A public virtual filesystem interface.
- Content sniffing.
- Authorization policy beyond normal route hooks.

## Implementation Plan

1. Add `include/uvpp/protocols/http/static_files.hpp` and include it from the
   HTTP umbrella header.
2. Add a private implementation file for path resolution, content types,
   validators, and file sending.
3. Implement `static_file_options`, `static_file_handler`, and
   `static_files(root, options)`.
4. Resolve wildcard tails from `decoded_path_segments()` and
   `matched_pattern()`, not from a joined wildcard parameter string.
5. Add canonical-root confinement, hidden-file checks, index handling, and
   symlink policy.
6. Add response header generation and conditional request handling.
7. Add chunked streaming file sends that respect `stream_write_result` and
   `on_drain()`.
8. Add unit tests for path resolution and content-type detection.
9. Add integration tests for GET, HEAD, 304, missing files, index files, hidden
   files, traversal attempts, encoded separators, and symlink behavior where the
   platform supports it.
10. Document the helper in `docs/user/http-server.md` after implementation.

## Test Matrix

Path resolution:

- `/assets/app.js` serves `root/app.js`;
- `/assets` serves `root/index.html` when index is enabled;
- `/assets/` follows the same path as `/assets`;
- `/assets/../secret.txt` does not escape root;
- `/assets/%2e%2e/secret.txt` does not escape root;
- `/assets/a%2Fb.txt` does not serve `root/a/b.txt`;
- hidden files are rejected by default;
- `.well-known` is served only with `allow_well_known`;
- symlinks outside the root are rejected.

HTTP behavior:

- successful GET includes content type, content length, validators, cache
  header, and `nosniff`;
- HEAD includes the same metadata headers and no body;
- unknown extensions use `application/octet-stream`;
- `If-None-Match` returns `304` when the weak ETag matches;
- `If-Modified-Since` returns `304` when appropriate;
- malformed conditional headers are ignored;
- unsupported methods use the existing `405` machinery.

Streaming behavior:

- files are sent in bounded chunks;
- backpressure pauses further file reads until drain;
- cancel closes the file sender state;
- read failure after headers commit does not attempt a second HTTP response.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [API principles](../design/api-principles.md)
- [Route path decoding archive](../archive/route-path-decoding.md)
