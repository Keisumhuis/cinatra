#include <filesystem>
#include <future>
#include <memory>
#include <system_error>

#include "cinatra.hpp"
#include "cinatra/websocket.hpp"
#include "doctest/doctest.h"

using namespace cinatra;

#ifdef CINATRA_ENABLE_SSL
TEST_CASE("test wss client") {
  http_ssl_server server(std::thread::hardware_concurrency());
  server.set_ssl_conf(
      {"../../include/cinatra/server.crt", "../../include/cinatra/server.key"});
  REQUIRE(server.listen("0.0.0.0", "9001"));

  server.set_http_handler<GET>("/", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_message, [](request &req) {
      auto part_data = req.get_part_data();
      req.get_conn<cinatra::SSL>()->send_ws_string(
          std::string(part_data.data(), part_data.length()));
    });
  });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  coro_http_client client;
  bool ok = client.init_ssl("../../include/cinatra", "server.crt");
  REQUIRE_MESSAGE(ok == true, "init ssl fail, please check ssl config");
  REQUIRE(async_simple::coro::syncAwait(
      client.async_ws_connect("wss://localhost:9001")));

  std::promise<void> promise;
  client.on_ws_msg([&promise](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      promise.set_value();
      return;
    }

    CHECK(data.resp_body == "hello");
    promise.set_value();
  });

  auto result = async_simple::coro::syncAwait(client.async_send_ws("hello"));
  std::cout << result.net_err << "\n";

  promise.get_future().wait();

  client.async_close();

  server.stop();
  server_thread.join();
}
#endif

async_simple::coro::Lazy<void> test_websocket(coro_http_client &client,
                                              std::promise<void> &promise) {
  client.on_ws_close([](std::string_view reason) {
    std::cout << "web socket close " << reason << std::endl;
    CHECK(reason == "ws close");
  });
  client.on_ws_msg([&](resp_data data) {
    if (data.net_err) {
      std::cout << data.net_err.message() << "\n";
      promise.set_value();
      return;
    }

    bool r = data.resp_body.find("hello websocket") != std::string::npos ||
             data.resp_body.find("test again") != std::string::npos;
    CHECK(r);

    std::cout << data.resp_body << std::endl;
  });
  bool r = co_await client.async_ws_connect("ws://localhost:8090/ws");
  if (!r) {
    co_return;
  }

  auto result = co_await client.async_send_ws("hello websocket");
  std::cout << result.net_err << "\n";
  result = co_await client.async_send_ws("test again", /*need_mask = */ false);
  std::cout << result.net_err << "\n";
  result = co_await client.async_send_ws_close("ws close");
  std::cout << result.net_err << "\n";
}

TEST_CASE("test websocket") {
  http_server server(std::thread::hardware_concurrency());
  bool r = server.listen("0.0.0.0", "8090");
  if (!r) {
    std::cout << "listen failed."
              << "\n";
  }
  server.enable_timeout(false);
  server.set_http_handler<GET, POST>("/ws", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_open, [](request &req) {
      std::cout << "websocket start" << std::endl;
    });

    req.on(ws_message, [](request &req) {
      auto part_data = req.get_part_data();
      // echo
      std::string str = std::string(part_data.data(), part_data.length());
      req.get_conn<cinatra::NonSSL>()->send_ws_string(str);
      std::cout << part_data.data() << std::endl;
    });

    req.on(ws_error, [](request &req) {
      std::cout << "websocket pack error or network error" << std::endl;
    });
  });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coro_http_client client;
  client.set_ws_sec_key("s//GYHa/XO7Hd2F2eOGfyA==");

  std::promise<void> promise;
  async_simple::coro::syncAwait(test_websocket(client, promise));

  client.async_close();
  promise.get_future().wait();

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  server.stop();
  server_thread.join();
}

void test_websocket_content(size_t len) {
  http_server server(std::thread::hardware_concurrency());
  server.enable_timeout(false);
  REQUIRE(server.listen("0.0.0.0", "8090"));

  server.set_http_handler<GET>("/", [](request &req, response &res) {
    assert(req.get_content_type() == content_type::websocket);

    req.on(ws_message, [](request &req) {
      auto part_data = req.get_part_data();
      req.get_conn<cinatra::NonSSL>()->send_ws_string(
          std::string(part_data.data(), part_data.length()));
    });
  });

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  coro_http_client client;
  REQUIRE(async_simple::coro::syncAwait(
      client.async_ws_connect("ws://localhost:8090")));

  std::pair<std::promise<void>, bool> msg_pair_promise{};

  std::string send_str(len, 'a');

  std::promise<void> quit_promise{};

  client.on_ws_msg([&, send_str](resp_data data) {
    if (data.net_err) {
      std::cout << "ws_msg net error " << data.net_err.message() << "\n";
      quit_promise.set_value();
      if (!msg_pair_promise.second) {
        msg_pair_promise.first.set_value();
      }

      return;
    }

    std::cout << "ws msg len: " << data.resp_body.size() << std::endl;
    REQUIRE(data.resp_body.size() == send_str.size());
    CHECK(data.resp_body == send_str);
    msg_pair_promise.first.set_value();
    msg_pair_promise.second = true;
  });

  async_simple::coro::syncAwait(client.async_send_ws(send_str));
  msg_pair_promise.first.get_future().wait();

  server.stop();
  server_thread.join();

  client.async_close();

  quit_promise.get_future().wait();
}

TEST_CASE("test websocket content lt 126") {
  test_websocket_content(1);
  test_websocket_content(125);
}

TEST_CASE("test websocket content ge 126") {
  test_websocket_content(126);
  test_websocket_content(127);
}

TEST_CASE("test websocket content ge 65535") {
  test_websocket_content(65535);
  test_websocket_content(65536);
}

TEST_CASE("test send after server stop") {
  http_server server(std::thread::hardware_concurrency());
  // server.enable_timeout(false);
  REQUIRE(server.listen("0.0.0.0", "8090"));

  std::promise<void> pr;
  std::future<void> f = pr.get_future();
  std::thread server_thread([&server, &pr]() {
    pr.set_value();
    server.run();
  });
  f.wait();

  auto client = std::make_shared<coro_http_client>();
  REQUIRE(async_simple::coro::syncAwait(
      client->async_ws_connect("ws://localhost:8090")));

  std::promise<void> promise;
  client->on_ws_msg([&client, &promise](resp_data data) {
    if (data.net_err) {
      client->async_close();
    }
    promise.set_value();
  });

  server.stop();

  std::this_thread::sleep_for(std::chrono::milliseconds(600));

  auto result = async_simple::coro::syncAwait(client->async_send_ws(""));
  CHECK(result.net_err);

  server_thread.join();
  promise.get_future().wait();
}
