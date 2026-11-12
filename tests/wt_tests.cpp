#include "app.hpp"
#include "crypto.hpp"
#include "matchmaker/client.hpp"
#include "store.hpp"

#include "cert.hpp"
#include "fixtures.hpp"
#include "wt_peer.hpp"

#include <doctest/doctest.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <chrono>
#include <string>

using namespace matchmaker;
using namespace matchmaker::test;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{

namespace beast = boost::beast;
namespace http = beast::http;

const char* kWtGame = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
const char* kGoodOrigin = "https://game.example";

struct WtServer
{
  std::filesystem::path dir;
  std::unique_ptr<App> app;

  WtServer()
  {
    dir = make_temp_dir();
    const auto db = (dir / "mm.sqlite").string();
    write_test_certificate(dir / "cert.pem", dir / "key.pem");
    {
      auto store = open_sqlite_store(db);
      store->migrate();
      store->set_config("listen", "127.0.0.1:0");
      store->set_config("stats_listen", "127.0.0.1:0");
      store->set_config("wt_listen", "127.0.0.1:0");
      store->set_config("relay_wt_enabled", "1");
      store->set_config("cors_origins", kGoodOrigin);
      store->set_config("tls_cert", (dir / "cert.pem").string());
      store->set_config("tls_key", (dir / "key.pem").string());
      store->set_config("jwt_secret", "SUPERSECRETVALUE");
      store->set_config("public_host", "127.0.0.1");
      store->set_config("public_url", "https://127.0.0.1");
      store->set_config("relay_bind", "127.0.0.1");
      store->set_config("relay_port_start", "54300");
      store->set_config("relay_port_end", "54400");
      store->set_config("max_threads", "1");
      store->set_config("host_heartbeat_interval_sec", "60");
      store->set_config("host_heartbeat_misses", "10");
      store->set_config("max_idle_sec", "60");
      store->set_config("max_channel_lifetime_sec", "120");
      auto game = sample_game(kWtGame, "secret");
      game.allow_join_in_progress = true;
      game.limits.allow_wt = true;
      store->upsert_game(game);
    }
    app = std::make_unique<App>(db);
    app->start();
  }

  ~WtServer()
  {
    if( app )
    {
      app->stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  Client client() const
  {
    ClientOptions options;
    options.host = "127.0.0.1";
    options.port = app->https_port();
    options.insecure = true;
    options.timeout = std::chrono::seconds(8);
    return Client(options);
  }
};

struct Login
{
  std::string token;
  std::string user_id;
};

Login login(Client& client, const char* alias)
{
  auto res = client.request("POST", "/api/v1/auth",
                            {{"game_id", kWtGame}, {"token", "secret"}, {"alias", alias}});
  REQUIRE(res.status == 200);
  return {res.body.at("access_token").get<std::string>(),
          res.body.at("user_id").get<std::string>()};
}

std::string ticket_for(Client& client, const Login& who)
{
  auto res = client.request("POST", "/api/v1/wt-ticket", json::object(), who.token);
  REQUIRE(res.status == 200);
  return res.body.at("ticket").get<std::string>();
}

struct RawResponse
{
  int status = 0;
  std::string allow_origin;
  std::string allow_methods;
  std::string vary;
  std::string body;
};

RawResponse raw_request(std::uint16_t port, http::verb method, const std::string& target,
                        const std::string& origin)
{
  boost::asio::io_context ioc;
  boost::asio::ssl::context ctx(boost::asio::ssl::context::tls_client);
  ctx.set_verify_mode(boost::asio::ssl::verify_none);
  beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
  beast::get_lowest_layer(stream).connect({boost::asio::ip::make_address("127.0.0.1"), port});
  stream.handshake(boost::asio::ssl::stream_base::client);
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if( !origin.empty() )
  {
    req.set(http::field::origin, origin);
  }
  if( method == http::verb::options )
  {
    req.set(http::field::access_control_request_method, "POST");
  }
  http::write(stream, req);
  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);
  RawResponse out;
  out.status = static_cast<int>(res.result_int());
  out.allow_origin = std::string(res[http::field::access_control_allow_origin]);
  out.allow_methods = std::string(res[http::field::access_control_allow_methods]);
  out.vary = std::string(res[http::field::vary]);
  out.body = res.body();
  beast::error_code ec;
  stream.shutdown(ec);
  return out;
}

bool is_op(const json& message, const char* op)
{
  return message.value("op", "") == op;
}

bool is_event(const json& message, const char* event)
{
  return message.value("event", "") == event;
}

std::vector<std::uint8_t> datagram_for(const std::string& channel_id, std::string_view payload)
{
  const auto id = uuid_bytes(channel_id);
  REQUIRE(id.has_value());
  std::vector<std::uint8_t> out(id->begin(), id->end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

json read_ws_where(Client& client, auto&& pred)
{
  for( int i = 0; i < 12; ++i )
  {
    auto message = client.ws_read(std::chrono::milliseconds(1500));
    if( pred(message) )
    {
      return message;
    }
  }
  REQUIRE(false);
  return {};
}

} // namespace

TEST_CASE("https cors allows listed origins and refuses others")
{
  WtServer server;
  const auto port = server.app->https_port();

  const auto allowed = raw_request(port, http::verb::get, "/api/v1/health", kGoodOrigin);
  CHECK(allowed.status == 200);
  CHECK(allowed.allow_origin == kGoodOrigin);
  CHECK(allowed.vary.find("Origin") != std::string::npos);

  const auto refused = raw_request(port, http::verb::get, "/api/v1/health", "https://evil.example");
  CHECK(refused.status == 403);
  CHECK(json::parse(refused.body)["error"] == "forbidden");
  CHECK(refused.allow_origin.empty());

  const auto preflight = raw_request(port, http::verb::options, "/api/v1/auth", kGoodOrigin);
  CHECK(preflight.status == 204);
  CHECK(preflight.allow_origin == kGoodOrigin);
  CHECK(preflight.allow_methods.find("POST") != std::string::npos);

  const auto native = raw_request(port, http::verb::get, "/api/v1/health", "");
  CHECK(native.status == 200);
  CHECK(native.allow_origin.empty());

  Client probe = server.client();
  auto config = probe.request("GET", "/api/v1/config");
  REQUIRE(config.status == 200);
  CHECK(config.body["webtransport"]["enabled"] == true);
}

TEST_CASE("webtransport lobby session: ticket, chat, datagrams, and release")
{
  WtServer server;
  REQUIRE(server.app->wt_port() != 0);
  Client host_http = server.client();
  Client guest_http = server.client();
  const auto host = login(host_http, "wt-host");
  const auto guest = login(guest_http, "wt-guest");

  auto no_lobby = guest_http.request("POST", "/api/v1/wt-ticket", json::object(), guest.token);
  CHECK(no_lobby.status != 200);
  CHECK(no_lobby.body["error"] == "not_member");

  auto created = host_http.request("POST", "/api/v1/lobbies", json::object(), host.token);
  REQUIRE(created.status == 200);
  const auto lobby_id = created.body.at("lobby_id").get<std::string>();
  const auto path = "/wt/v1/lobbies/" + lobby_id;

  auto ticket_res = host_http.request("POST", "/api/v1/wt-ticket", json::object(), host.token);
  REQUIRE(ticket_res.status == 200);
  CHECK(ticket_res.body["expires_in"] == 30);
  CHECK(ticket_res.body["url"].get<std::string>().find(path + "?ticket=") != std::string::npos);
  const auto host_ticket = ticket_res.body.at("ticket").get<std::string>();

  {
    WtPeer jwt_peer(server.app->wt_port());
    CHECK(jwt_peer.connect(path + "?access_token=" + host.token) == 401);
  }
  {
    WtPeer bad_origin(server.app->wt_port());
    CHECK(bad_origin.connect(path + "?ticket=" + host_ticket, "https://evil.example") == 403);
  }

  WtPeer host_wt(server.app->wt_port());
  REQUIRE(host_wt.connect(path + "?ticket=" + host_ticket, kGoodOrigin) == 200);
  auto snapshot = host_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "lobby_snapshot");
      },
      3s);
  REQUIRE(snapshot.has_value());
  bool host_listed = false;
  for( const auto& member : (*snapshot)["members"] )
  {
    if( member["user_id"] == host.user_id )
    {
      host_listed = true;
      CHECK(member["transport"] == "webtransport");
    }
  }
  CHECK(host_listed);

  {
    WtPeer reuse(server.app->wt_port());
    CHECK(reuse.connect(path + "?ticket=" + host_ticket) == 401);
  }

  host_wt.send({{"op", "set_phase"}, {"phase", "open"}, {"id", "open"}});
  auto opened = host_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "ok") || is_op(m, "error");
      },
      3s);
  REQUIRE(opened.has_value());
  CHECK(is_op(*opened, "ok"));
  CHECK((*opened)["id"] == "open");

  auto joined = guest_http.request("POST", "/api/v1/lobbies/" + lobby_id + "/join",
                                   json::object(), guest.token);
  REQUIRE(joined.status == 200);
  WtPeer guest_wt(server.app->wt_port());
  REQUIRE(guest_wt.connect(path + "?ticket=" + ticket_for(guest_http, guest)) == 200);
  REQUIRE(guest_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "lobby_snapshot");
      },
      3s));
  auto entered = host_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "user_entered");
      },
      3s);
  REQUIRE(entered.has_value());
  CHECK((*entered)["user_id"] == guest.user_id);
  CHECK((*entered)["transport"] == "webtransport");

  host_wt.send({{"op", "chat"}, {"text", "over-quic"}, {"id", "c1"}});
  auto chat = guest_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "chat");
      },
      3s);
  REQUIRE(chat.has_value());
  CHECK((*chat)["text"] == "over-quic");

  host_wt.send({{"op", "allocate_relays"},
                {"id", "alloc"},
                {"channels", json::array({{{"a", host.user_id},
                                           {"b", guest.user_id},
                                           {"protocol", "wt"},
                                           {"lifetime_sec", 30}}})}});
  auto host_alloc = host_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "relay_allocated") || is_op(m, "error");
      },
      3s);
  REQUIRE(host_alloc.has_value());
  REQUIRE_MESSAGE(is_event(*host_alloc, "relay_allocated"), host_alloc->dump());
  CHECK((*host_alloc)["protocol"] == "wt");
  CHECK((*host_alloc)["peer_user_id"] == guest.user_id);
  CHECK_FALSE(host_alloc->contains("port"));
  CHECK_FALSE(host_alloc->contains("token"));
  CHECK_FALSE(host_alloc->contains("host"));
  auto guest_alloc = guest_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "relay_allocated");
      },
      3s);
  REQUIRE(guest_alloc.has_value());
  const auto channel_id = (*host_alloc)["channel_id"].get<std::string>();
  CHECK((*guest_alloc)["channel_id"] == channel_id);

  const auto hello = datagram_for(channel_id, "game-bytes");
  host_wt.send_datagram(hello);
  auto forwarded = guest_wt.read_datagram(3s);
  REQUIRE(forwarded.has_value());
  CHECK(*forwarded == hello);

  const auto back = datagram_for(channel_id, "reply");
  guest_wt.send_datagram(back);
  auto returned = host_wt.read_datagram(3s);
  REQUIRE(returned.has_value());
  CHECK(*returned == back);

  host_wt.send_datagram(datagram_for(uuid_v4(), "lost"));
  CHECK_FALSE(guest_wt.read_datagram(400ms).has_value());

  host_wt.send({{"op", "release_relays"}, {"id", "release"}});
  auto released = guest_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "relay_released");
      },
      3s);
  REQUIRE(released.has_value());
  CHECK((*released)["channel_id"] == channel_id);

  host_wt.send_datagram(datagram_for(channel_id, "after-release"));
  CHECK_FALSE(guest_wt.read_datagram(400ms).has_value());

  host_wt.send({{"op", "chat"}, {"text", "still-open"}});
  auto still = guest_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "chat") && m.value("text", "") == "still-open";
      },
      3s);
  CHECK(still.has_value());
  CHECK_FALSE(host_wt.session_closed(100ms));
  CHECK_FALSE(guest_wt.session_closed(100ms));

  guest_wt.close_session();
  auto left = host_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "user_left");
      },
      3s);
  REQUIRE(left.has_value());
  CHECK((*left)["user_id"] == guest.user_id);
}

TEST_CASE("wt allocation needs both members on webtransport; udp still binds ports")
{
  WtServer server;
  Client host_http = server.client();
  Client guest = server.client();
  const auto host = login(host_http, "mixed-host");
  const auto guest_login = login(guest, "mixed-guest");

  auto created = host_http.request("POST", "/api/v1/lobbies", json::object(), host.token);
  REQUIRE(created.status == 200);
  const auto lobby_id = created.body.at("lobby_id").get<std::string>();
  WtPeer host_wt(server.app->wt_port());
  REQUIRE(host_wt.connect("/wt/v1/lobbies/" + lobby_id + "?ticket=" + ticket_for(host_http, host)) ==
          200);
  REQUIRE(host_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "lobby_snapshot");
      },
      3s));
  host_wt.send({{"op", "set_phase"}, {"phase", "open"}});
  REQUIRE(host_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "ok");
      },
      3s));

  auto joined = guest.request("POST", "/api/v1/lobbies/" + lobby_id + "/join", json::object(),
                              guest_login.token);
  REQUIRE(joined.status == 200);
  guest.ws_connect(lobby_id, guest_login.token);
  auto snapshot = guest.ws_read(2s);
  REQUIRE(is_op(snapshot, "lobby_snapshot"));
  for( const auto& member : snapshot["members"] )
  {
    CHECK(member["transport"] == (member["user_id"] == host.user_id ? "webtransport" : "websocket"));
  }
  auto entered = host_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "user_entered");
      },
      3s);
  REQUIRE(entered.has_value());
  CHECK((*entered)["transport"] == "websocket");

  {
    WtPeer second(server.app->wt_port());
    auto ticket = guest.request("POST", "/api/v1/wt-ticket", json::object(), guest_login.token);
    REQUIRE(ticket.status == 200);
    CHECK(second.connect("/wt/v1/lobbies/" + lobby_id + "?ticket=" +
                         ticket.body.at("ticket").get<std::string>()) == 409);
  }

  host_wt.send({{"op", "allocate_relays"},
                {"id", "wt"},
                {"channels", json::array({{{"a", host.user_id},
                                           {"b", guest_login.user_id},
                                           {"protocol", "wt"},
                                           {"lifetime_sec", 30}}})}});
  auto refused = host_wt.read_until(
      [](const json& m)
      {
        return is_op(m, "error") || is_event(m, "relay_allocated");
      },
      3s);
  REQUIRE(refused.has_value());
  CHECK(is_op(*refused, "error"));
  CHECK((*refused)["error"] == "bad_request");
  CHECK((*refused)["id"] == "wt");

  host_wt.send({{"op", "allocate_relays"},
                {"id", "udp"},
                {"channels", json::array({{{"a", host.user_id},
                                           {"b", guest_login.user_id},
                                           {"protocol", "udp"},
                                           {"lifetime_sec", 30}}})}});
  auto host_alloc = host_wt.read_until(
      [](const json& m)
      {
        return is_event(m, "relay_allocated") || is_op(m, "error");
      },
      3s);
  REQUIRE(host_alloc.has_value());
  REQUIRE_MESSAGE(is_event(*host_alloc, "relay_allocated"), host_alloc->dump());
  CHECK((*host_alloc)["protocol"] == "udp");
  CHECK((*host_alloc)["port"].get<int>() >= 54300);
  CHECK((*host_alloc)["token"].get<std::string>().size() == 32);
  auto guest_alloc = read_ws_where(guest,
                                   [](const json& m)
                                   {
                                     return is_event(m, "relay_allocated");
                                   });
  CHECK(guest_alloc["port"].get<int>() >= 54300);
  CHECK(guest_alloc["port"] != (*host_alloc)["port"]);
}
