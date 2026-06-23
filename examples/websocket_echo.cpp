#include <iostream>
#include <span>
#include <string_view>

#include <uvpp/uv.hpp>
#include <uvpp/protocols/http.hpp>
#include <uvpp/protocols/websocket.hpp>

int main() {
  uv::loop loop;
  uvp::http::server srv(loop);

  srv.get("/health", uvp::http::body::none{}, [](uvp::http::request&, uvp::http::response& res) {
    res.text("ok\n");
  });

  srv.upgrade("/echo", [](uvp::http::upgrade_request& req) {
    uvp::websocket::accept_detached(req, uvp::websocket::accept_options{}
      .on_text([](uvp::websocket::session& ws, std::string_view message) {
        ws.text(message);
      })
      .on_binary([](uvp::websocket::session& ws, std::span<const std::byte> message) {
        ws.binary(message);
      })
      .on_error([](uvp::websocket::session&, std::error_code error) {
        std::cerr << "websocket error: " << error.message() << '\n';
      }));
  });

  srv.listen("127.0.0.1", 8084);
  std::cout << "websocket echo listening on ws://127.0.0.1:8084/echo\n";
  loop.run();
}
