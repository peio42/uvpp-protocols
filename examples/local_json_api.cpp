#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

namespace {

struct item {
  unsigned int id = 0;
  std::string title;
  bool done = false;
};

std::string escape_json(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += ch;
      break;
    }
  }
  return out;
}

std::string item_json(const item& value) {
  std::ostringstream out;
  out << "{\"id\":\"" << value.id
      << "\",\"title\":\"" << escape_json(value.title)
      << "\",\"done\":\"" << (value.done ? "true" : "false")
      << "\"}";
  return out.str();
}

std::string items_json(const std::vector<item>& values) {
  std::string out = "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += item_json(value);
  }
  out += "]";
  return out;
}

std::optional<std::string> json_string_field(std::string_view body, std::string_view field) {
  const auto key = std::string("\"") + std::string(field) + "\"";
  auto offset = body.find(key);
  if (offset == std::string_view::npos) {
    return {};
  }

  offset = body.find(':', offset + key.size());
  if (offset == std::string_view::npos) {
    return {};
  }

  ++offset;
  while (offset < body.size() && std::isspace(static_cast<unsigned char>(body[offset]))) {
    ++offset;
  }

  if (offset >= body.size() || body[offset] != '"') {
    return {};
  }

  ++offset;
  std::string value;
  while (offset < body.size()) {
    const auto ch = body[offset++];
    if (ch == '"') {
      return value;
    }
    if (ch == '\\' && offset < body.size()) {
      value += body[offset++];
      continue;
    }
    value += ch;
  }
  return {};
}

std::optional<unsigned int> parse_id(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  unsigned int result = 0;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return {};
    }
    result = result * 10 + static_cast<unsigned int>(ch - '0');
  }
  return result;
}

} // namespace

int main() {
  uv::loop loop;
  std::vector<item> items{
    item{1, "read route params", true},
    item{2, "try deferred responses", false},
  };
  unsigned int next_id = 3;

  uvp::http::server srv(loop);

  srv.get("/v1/items", [&items](uvp::http::request&, uvp::http::response& res) {
    res.json(items_json(items));
  });

  srv.get("/v1/items/:id", [&items](uvp::http::request& req, uvp::http::response& res) {
    const auto id = parse_id(req.params().get("id"));
    if (!id) {
      res.status(400).json({{"error", "invalid id"}});
      return;
    }

    const auto found = std::find_if(items.begin(), items.end(), [id](const item& value) {
      return value.id == *id;
    });
    if (found == items.end()) {
      res.status(uvp::http::status::not_found).json({{"error", "item not found"}});
      return;
    }

    res.json(item_json(*found));
  });

  srv.post("/v1/items", uvp::http::body::text{}, [&items, &next_id](
    uvp::http::request&,
    uvp::http::response& res,
    std::string_view body) {
    auto title = json_string_field(body, "title");
    if (!title || title->empty()) {
      res.status(400).json({{"error", "body must contain a string title field"}});
      return;
    }

    auto& created = items.emplace_back(item{next_id++, std::move(*title), false});
    res.status(uvp::http::status::created).json(item_json(created));
  });

  srv.not_found([](uvp::http::request& req, uvp::http::response& res) {
    res.status(uvp::http::status::not_found)
      .json({{"error", "route not found"}, {"path", req.path()}});
  });

  srv.on_error([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
    res.status(uvp::http::status::internal_server_error).json({{"error", "internal server error"}});
  });

  srv.listen("127.0.0.1", 8082);
  std::cout << "local JSON API listening on http://127.0.0.1:8082\n";

  loop.run();
}
