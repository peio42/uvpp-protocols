#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>

using namespace std::chrono_literals;

namespace {

struct stream_client {
  uvp::http::streaming_response stream;
  std::deque<std::string> pending;
  bool closed = false;
};

struct log_state {
  unsigned int next_sequence = 1;
  std::vector<std::string> recent;
  std::vector<std::shared_ptr<stream_client>> clients;
  std::size_t cancelled_clients = 0;
  std::size_t errored_clients = 0;
};

std::span<const std::byte> as_bytes(const std::string& value) noexcept {
  return std::as_bytes(std::span{value.data(), value.size()});
}

std::string ndjson_event(unsigned int sequence, std::string_view message) {
  return uvp::json{{"sequence", sequence}, {"message", std::string(message)}}.dump() + "\n";
}

std::string recent_text(const log_state& state) {
  std::string out;
  for (const auto& line : state.recent) {
    out += line;
  }
  return out;
}

void flush_client(stream_client& client) {
  while (!client.pending.empty() && client.stream.active()) {
    auto line = std::move(client.pending.front());
    client.pending.pop_front();

    const auto result = client.stream.write(std::move(line));
    if (!result.accepted()) {
      client.closed = true;
      return;
    }
    if (!result) {
      return;
    }
  }
}

void publish_log(log_state& state, std::string_view message) {
  auto line = ndjson_event(state.next_sequence++, message);
  state.recent.push_back(line);
  if (state.recent.size() > 20) {
    state.recent.erase(state.recent.begin());
  }

  for (auto& client : state.clients) {
    if (client->stream.active() && !client->closed) {
      client->pending.push_back(line);
      flush_client(*client);
    }
  }
  std::erase_if(state.clients, [](const std::shared_ptr<stream_client>& client) {
    return client->closed || !client->stream.active();
  });
}

} // namespace

int main() {
  uv::loop loop;
  log_state state;

  uvp::http::server srv(
    loop,
    uvp::http::server_options{}
      .max_pending_responses_per_connection(32)
      .idle_timeout(2min));

  srv.get("/logs/recent", [&state](uvp::http::request&, uvp::http::response& res) {
    res.type("application/x-ndjson").bytes(as_bytes(recent_text(state)));
  });

  srv.get("/logs/stream", [&state](uvp::http::request&, uvp::http::response& res) {
    if (state.clients.size() >= 32) {
      res.status(uvp::http::status::service_unavailable)
        .json(uvp::json{{"error", "too many pending log stream clients"}});
      return;
    }

    auto client = std::make_shared<stream_client>();
    client->stream = res.stream();
    client->stream.type("application/x-ndjson");
    client->stream.on_drain([client] {
      flush_client(*client);
    });
    client->stream.on_cancel([&state, client] {
      client->closed = true;
      ++state.cancelled_clients;
    });
    client->stream.on_error([&state, client](std::error_code) {
      client->closed = true;
      ++state.errored_clients;
    });

    for (const auto& line : state.recent) {
      client->pending.push_back(line);
    }
    flush_client(*client);
    state.clients.push_back(std::move(client));
  });

  srv.get("/logs/stats", [&state](uvp::http::request&, uvp::http::response& res) {
    res.json(uvp::json{
      {"recent", state.recent.size()},
      {"stream_clients", state.clients.size()},
      {"cancelled_stream_clients", state.cancelled_clients},
      {"errored_stream_clients", state.errored_clients},
    });
  });

  srv.not_found([](uvp::http::request&, uvp::http::response& res) {
    res.status(uvp::http::status::not_found)
      .json(uvp::json{{"error", "try /logs/recent, /logs/stream, or /logs/stats"}});
  });

  uv::timer ticker(loop);
  ticker.start(250ms, 1s, [&state](uv::timer&) {
    publish_log(state, "background task heartbeat");
  });

  srv.listen("127.0.0.1", 8083);
  std::cout << "log streaming example listening on http://127.0.0.1:8083\n";
  std::cout << "try: curl -N http://127.0.0.1:8083/logs/stream\n";

  loop.run();
}
