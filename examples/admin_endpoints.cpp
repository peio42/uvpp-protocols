#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

using namespace std::chrono_literals;

namespace {

struct admin_state {
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  bool maintenance = false;
  std::vector<std::string> audit_log{"server started"};
};

std::string yes_no(bool value) {
  return value ? "true" : "false";
}

std::string uptime_seconds(const admin_state& state) {
  const auto elapsed = std::chrono::steady_clock::now() - state.started_at;
  return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
}

std::string audit_text(const admin_state& state) {
  std::ostringstream out;
  for (const auto& entry : state.audit_log) {
    out << entry << '\n';
  }
  return out.str();
}

} // namespace

int main() {
  uv::loop loop;
  admin_state state;

  uvp::http::server srv(
    loop,
    uvp::http::server_options{}
      .max_header_bytes(32 * 1024)
      .max_body_bytes(64 * 1024)
      .idle_timeout(30s)
      .server_header(false));

  srv.get("/admin/health", [&state](uvp::http::request&, uvp::http::response& res) {
    const auto maintenance = yes_no(state.maintenance);
    const auto uptime = uptime_seconds(state);
    res.json({
      {"status", state.maintenance ? "maintenance" : "ok"},
      {"maintenance", maintenance},
      {"uptime_seconds", uptime},
    });
  });

  srv.get("/admin/metrics", [&state](uvp::http::request&, uvp::http::response& res) {
    std::ostringstream out;
    out << "maintenance " << (state.maintenance ? 1 : 0) << '\n';
    out << "uptime_seconds " << uptime_seconds(state) << '\n';
    out << "audit_entries " << state.audit_log.size() << '\n';
    res.type("text/plain; charset=utf-8").text(out.str());
  });

  srv.get("/admin/audit", [&state](uvp::http::request&, uvp::http::response& res) {
    res.text(audit_text(state));
  });

  srv.post("/admin/maintenance/:state", [&state](uvp::http::request& req, uvp::http::response& res) {
    const auto requested = req.params().get("state");
    if (requested == "on") {
      state.maintenance = true;
    } else if (requested == "off") {
      state.maintenance = false;
    } else {
      res.status(400).json({{"error", "state must be on or off"}});
      return;
    }

    state.audit_log.push_back(std::string("maintenance ") + std::string(requested));
    res.json({{"maintenance", yes_no(state.maintenance)}});
  });

  srv.not_found([](uvp::http::request& req, uvp::http::response& res) {
    res.status(uvp::http::status::not_found)
      .json({{"error", "unknown admin endpoint"}, {"path", req.path()}});
  });

  srv.listen("127.0.0.1", 8081);
  std::cout << "admin endpoints listening on http://127.0.0.1:8081\n";

  loop.run();
}
