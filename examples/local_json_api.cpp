#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
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

uvp::json item_json(const item& value) {
  return uvp::json{
    {"id", value.id},
    {"title", value.title},
    {"done", value.done},
  };
}

uvp::json items_json(const std::vector<item>& values) {
  auto out = uvp::json::array();
  for (const auto& value : values) {
    out.push_back(item_json(value));
  }
  return out;
}

std::optional<std::string> json_string_field(std::string_view body, std::string_view field) {
  try {
    const auto parsed = uvp::json::parse(body.begin(), body.end());
    const auto it = parsed.find(field);
    if (it == parsed.end() || !it->is_string()) {
      return {};
    }
    return it->get<std::string>();
  } catch (const uvp::json::exception&) {
    return {};
  }
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
      res.status(400).json(uvp::json{{"error", "invalid id"}});
      return;
    }

    const auto found = std::find_if(items.begin(), items.end(), [id](const item& value) {
      return value.id == *id;
    });
    if (found == items.end()) {
      res.status(uvp::http::status::not_found).json(uvp::json{{"error", "item not found"}});
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
      res.status(400).json(uvp::json{{"error", "body must contain a string title field"}});
      return;
    }

    auto& created = items.emplace_back(item{next_id++, std::move(*title), false});
    res.status(uvp::http::status::created).json(item_json(created));
  });

  srv.not_found([](uvp::http::request& req, uvp::http::response& res) {
    res.status(uvp::http::status::not_found)
      .json(uvp::json{{"error", "route not found"}, {"path", std::string(req.path())}});
  });

  srv.on_exception([](uvp::http::request&, uvp::http::response& res, std::exception_ptr) {
    res.status(uvp::http::status::internal_server_error).json(uvp::json{{"error", "internal server error"}});
  });

  srv.listen("127.0.0.1", 8082);
  std::cout << "local JSON API listening on http://127.0.0.1:8082\n";

  loop.run();
}
