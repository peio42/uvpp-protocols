# Typed JSON Body Policy Proposal

Status: Implemented

## Context

HTTP body policies leave room for an explicit `body::json<T>` request policy.
Responses already expose `response::json()` and `uvp::json` is available as the
project JSON type.

## Current State

- Implemented: `body::bytes`, `body::text`, `body::stream`, `body::json<T>`,
  `response::json`, and the `uvp::json` alias.

## Implemented Scope

- Parse JSON request bodies through `uvp::json`.
- Convert typed bodies through `from_json` customization.
- Define parse error and validation error response behavior.
- Keep typed JSON policies explicit at route declaration time.

## API

The implementation adds an explicit buffered body policy:

```cpp
namespace uvp::http::body {

template<class T = uvp::json>
struct json {};

} // namespace uvp::http::body
```

Routes opt in at declaration time:

```cpp
struct create_user {
  std::string name;
};

void from_json(const uvp::json& value, create_user& out) {
  out.name = value.at("name").get<std::string>();
}

srv.post(
  "/users",
  uvp::http::route_options{}.max_body_bytes(64 * 1024),
  uvp::http::body::json<create_user>{},
  [](uvp::http::request& req, uvp::http::response& res, const create_user& body) {
    (void)req;
    res.status(uvp::http::status::created).json({{"name", body.name}});
  });
```

`body::json<>` is shorthand for `body::json<uvp::json>`.

JSON remains an expensive/typed policy and should not be inferred from handler
signatures. A route that wants JSON must declare `body::json<T>{}` explicitly.

## Handler Contract

For `body::json<T>`, the handler receives:

```cpp
void handler(request& req, response& res, const T& body);
```

The decoded value is owned by the route wrapper and is valid for the duration of
the handler call. If an application needs to keep it after returning, it copies
or moves into its own state. The implementation requires a synchronous handler,
matching the existing buffered `bytes` and `text` policies.

The implementation may call the stored handler with a local decoded value. That
keeps the public contract simple while still allowing handlers that accept `T`
by value.

## Dispatch Model

JSON reuses the buffered request path:

1. `body_mode::json` sits beside `none`, `bytes`, `text`, and `stream`.
2. Router overloads for `body::json<T>` store `body_mode::json` and a
   `wrap_json_handler<T>(...)` route handler.
3. Keep body limit and timeout handling identical to `body::bytes` and
   `body::text`.
4. After the complete body is buffered, pass the raw bytes to the JSON wrapper.
5. The wrapper validates content type, parses through `uvp::json`, converts to
   `T`, and calls the application handler only on success.

This keeps routing, buffering, rejection, and conversion attached to the route
body policy instead of adding a late `request::json<T>()` API.

## Content Type Policy

The default policy requires a JSON media type before parsing:

- accept `application/json`;
- accept structured JSON suffixes such as `application/problem+json`;
- accept optional parameters such as `application/json; charset=utf-8`;
- compare media types case-insensitively;
- reject missing or non-JSON `Content-Type` with `415 Unsupported Media Type`.

This adds `status::unsupported_media_type = 415` and a reason phrase.

## Error Behavior

The framework generates the response before the handler runs:

| Failure | Status | Body |
| --- | --- | --- |
| Missing or non-JSON `Content-Type` | `415 Unsupported Media Type` | `unsupported media type\n` |
| Malformed JSON, including an empty body | `400 Bad Request` | `invalid json\n` |
| `from_json` / `get<T>()` conversion failure | `422 Unprocessable Content` | `invalid json body\n` |

The handler should not be invoked for these failures. Response hooks should
still observe the generated response because the route matched and a response
slot exists.

The implementation catches `uvp::json::parse_error` for syntax errors and
`uvp::json::exception` during typed conversion. Exceptions thrown by the
handler continue to flow through the existing route exception handler.

## Implementation Notes

The policy marker belongs with the existing body policy markers in
`include/uvpp/protocols/http/request.hpp`; the typed route wrapper belongs in
`include/uvpp/protocols/http/router.hpp`, where the current policy wrappers are
already templated:

- `uvpp/protocols/json.hpp` is included where `body::json<T>` is declared;
- `template<class T = uvp::json> struct json {};` lives under
  `namespace body`;
- `detail::body_mode` includes `json`;
- `wrap_json_handler<T>(Handler&&)` performs content type validation, parsing,
  typed conversion, and handler dispatch;
- `router::route(...)` has overloads for `body::json<T>`;
- matching `router`, `route_group`, `route_resource`, and `server`
  convenience overload coverage through the existing generic `BodyPolicy`
  paths;
- `infer_body_policy` remains unchanged, so JSON
  stays explicit.

The server already treats every non-stream body mode as buffered, so
`body_mode::json` follows that path without special `src/http/server.cpp`
handling.

Parsing from bytes avoids requiring a null-terminated body:

```cpp
auto view = std::string_view{
  reinterpret_cast<const char*>(body.data()),
  body.size(),
};
auto parsed = uvp::json::parse(view.begin(), view.end());
auto decoded = parsed.get<T>();
```

For `T = uvp::json`, the wrapper passes the parsed `uvp::json` directly
instead of calling `get<uvp::json>()`.

## Tests

Focused router and integration coverage includes:

- router stores `body_mode::json` for explicit `body::json<T>{}`;
- valid `application/json` invokes the handler with `uvp::json`;
- valid typed JSON invokes `from_json` and the typed handler;
- `application/problem+json` and parameterized `application/json` are accepted;
- missing or non-JSON `Content-Type` returns `415`;
- malformed JSON returns `400`;
- typed conversion failures return `422`;
- route body limits and body timeouts still apply before JSON parsing;
- pre-handler hooks run before JSON conversion, matching the existing buffered
  request flow.

## Out Of Scope

- General schema validation framework.
- Content negotiation.
- Non-JSON body binding.

## Source Documents

- [HTTP server design](../design/http-server.md)
- [ADR 0005](../adr/0005-explicit-http-body-policies.md)
