#include "app.hpp"
#include "matchmaker/client.hpp"
#include "store.hpp"
#include "util.hpp"

#include "cert.hpp"
#include "fixtures.hpp"

#include <doctest/doctest.h>

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>

using namespace matchmaker;
using namespace matchmaker::test;
using json = nlohmann::json;

namespace
{

const char* kGame = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
const char* kOther = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

struct Server
{
  std::filesystem::path dir;
  std::unique_ptr<App> app;

  explicit Server(const std::map<std::string, std::string>& extra = {})
  {
    dir = make_temp_dir();
    const auto db = (dir / "mm.sqlite").string();
    write_test_certificate(dir / "cert.pem", dir / "key.pem");
    {
      auto store = open_sqlite_store(db);
      store->migrate();
      store->set_config("listen", "127.0.0.1:0");
      store->set_config("stats_listen", "127.0.0.1:0");
      store->set_config("tls_cert", (dir / "cert.pem").string());
      store->set_config("tls_key", (dir / "key.pem").string());
      store->set_config("jwt_secret", "SUPERSECRETVALUE");
      store->set_config("public_host", "127.0.0.1");
      store->set_config("public_url", "https://127.0.0.1");
      store->set_config("relay_bind", "127.0.0.1");
      store->set_config("relay_port_start", "54100");
      store->set_config("relay_port_end", "54200");
      store->set_config("relay_tcp_enabled", "1");
      store->set_config("max_threads", "1");
      store->set_config("http_gzip_min_bytes", "1");
      store->set_config("host_heartbeat_interval_sec", "60");
      store->set_config("host_heartbeat_misses", "10");
      store->set_config("max_idle_sec", "60");
      store->set_config("max_channel_lifetime_sec", "120");
      for( const auto& [key, value] : extra )
      {
        store->set_config(key, value);
      }
      auto game = sample_game(kGame, "secret");
      game.allow_join_in_progress = true;
      game.promote_host_on_leave = true;
      game.limits.allow_tcp = true;
      store->upsert_game(game);
      store->upsert_game(sample_game(kOther, "other-token"));
    }
    app = std::make_unique<App>(db);
    app->start();
  }

  ~Server()
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

struct Session
{
  std::string token;
  std::string user_id;
};

Session auth(Client& client, const char* game, const char* token, const char* alias)
{
  auto res = client.request("POST", "/api/v1/auth",
                            {{"game_id", game}, {"token", token}, {"alias", alias}});
  REQUIRE(res.error.empty());
  REQUIRE(res.status == 200);
  return {res.body.at("access_token").get<std::string>(),
          res.body.at("user_id").get<std::string>()};
}

json read_where(Client& client, auto&& pred)
{
  for( int i = 0; i < 12; ++i )
  {
    auto msg = client.ws_read(std::chrono::milliseconds(1500));
    if( pred(msg) )
    {
      return msg;
    }
  }
  REQUIRE(false);
  return {};
}

bool udp_roundtrip(const json& left_event, const json& right_event)
{
  const auto token_a = hex_decode(left_event.at("token").get<std::string>());
  const auto token_b = hex_decode(right_event.at("token").get<std::string>());
  if( !token_a || !token_b )
  {
    return false;
  }
  boost::asio::io_context ioc;
  boost::asio::ip::udp::socket left(ioc);
  boost::asio::ip::udp::socket right(ioc);
  left.open(boost::asio::ip::udp::v4());
  right.open(boost::asio::ip::udp::v4());
  const auto dest_a =
      boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                     static_cast<unsigned short>(left_event.at("port").get<int>()));
  const auto dest_b = boost::asio::ip::udp::endpoint(
      boost::asio::ip::make_address("127.0.0.1"),
      static_cast<unsigned short>(right_event.at("port").get<int>()));
  left.send_to(boost::asio::buffer(*token_a), dest_a);
  right.send_to(boost::asio::buffer(*token_b), dest_b);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const std::string payload = "lobby-bytes";
  left.send_to(boost::asio::buffer(payload), dest_a);
  std::array<char, 64> buf{};
  boost::asio::ip::udp::endpoint from;
  boost::system::error_code ec;
  right.async_receive_from(boost::asio::buffer(buf), from,
                           [&](boost::system::error_code read_ec, std::size_t n)
                           {
                             ec = read_ec;
                             if( !read_ec )
                             {
                               buf[n] = '\0';
                             }
                           });
  boost::asio::steady_timer timer(ioc);
  timer.expires_after(std::chrono::seconds(2));
  timer.async_wait(
      [&](boost::system::error_code)
      {
        right.cancel();
      });
  ioc.run();
  return !ec && std::string(buf.data()) == payload;
}

} // namespace

TEST_CASE("host control rtt is measured while the host socket is reading")
{
  Server server;
  std::string lobby_id;
  std::atomic<bool> reading{false};
  std::exception_ptr host_error;
  std::thread host_thread(
      [&]
      {
        try
        {
          Client host = server.client();
          const auto session = auth(host, kGame, "secret", "probe-host");
          auto created = host.request("POST", "/api/v1/lobbies", json::object(), session.token);
          REQUIRE(created.status == 200);
          lobby_id = created.body.at("lobby_id").get<std::string>();
          host.ws_connect(lobby_id, session.token);
          auto snapshot = host.ws_read(std::chrono::seconds(2));
          REQUIRE(snapshot["op"] == "lobby_snapshot");
          reading.store(true, std::memory_order_release);
          try
          {
            host.ws_read(std::chrono::seconds(3));
          }
          catch( ... )
          {
          }
        }
        catch( ... )
        {
          host_error = std::current_exception();
          reading.store(true, std::memory_order_release);
        }
      });
  while( !reading.load(std::memory_order_acquire) )
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Client guest = server.client();
  const auto guest_session = auth(guest, kGame, "secret", "probe-guest");
  auto ping = guest.request("GET", "/api/v1/lobbies/" + lobby_id + "?client_ts=7", {},
                            guest_session.token, false);
  host_thread.join();
  if( host_error )
  {
    std::rethrow_exception(host_error);
  }
  REQUIRE(ping.status == 200);
  CHECK(ping.body["client_ts"] == 7);
  CHECK(ping.body["host_control_rtt_ms"].is_number());
}

TEST_CASE("https then websocket then https")
{
  Server server;
  Client guest_http = server.client();
  auto health = guest_http.request("GET", "/api/v1/health");
  REQUIRE(health.status == 200);
  CHECK(health.body["status"] == "ok");

  auto config = guest_http.request("GET", "/api/v1/config");
  REQUIRE(config.status == 200);
  CHECK(config.content_encoding.find("gzip") != std::string::npos);
  CHECK(config.body["compression"]["http"].size() >= 1);
  CHECK(config.body.dump().find("SUPERSECRETVALUE") == std::string::npos);
  CHECK_FALSE(config.body.contains("jwt_secret"));

  Client host = server.client();
  Client guest = server.client();
  const auto host_session = auth(host, kGame, "secret", "host");
  const auto guest_session = auth(guest, kGame, "secret", "guest");

  auto created = host.request("POST", "/api/v1/lobbies", json::object(), host_session.token);
  REQUIRE(created.status == 200);
  const auto lobby_id = created.body.at("lobby_id").get<std::string>();
  auto duplicate = host.request("POST", "/api/v1/lobbies", json::object(), host_session.token);
  CHECK(duplicate.status == 409);
  CHECK(duplicate.body["error"] == "already_in_lobby");

  host.ws_connect(lobby_id, host_session.token);
  CHECK(host.ws_offered_extensions().find("permessage-deflate") != std::string::npos);
  auto snapshot = host.ws_read(std::chrono::seconds(2));
  CHECK(snapshot["op"] == "lobby_snapshot");
  CHECK(snapshot["phase"] == "closed");
  CHECK_FALSE(snapshot.contains("password"));

  host.ws_send({{"op", "set_phase"}, {"phase", "open"}, {"id", "phase"}});
  auto phase_ok = read_where(host,
                             [](const json& msg)
                             {
                               return msg.value("op", "") == "ok";
                             });
  CHECK(phase_ok["id"] == "phase");
  host.ws_send({{"op", "set_slots"}, {"slots", 3}});
  read_where(host,
             [](const json& msg)
             {
               return msg.value("op", "") == "ok";
             });
  host.ws_send({{"op", "set_short_code"}, {"code", "Play1"}});
  read_where(host,
             [](const json& msg)
             {
               return msg.value("op", "") == "ok";
             });

  auto listed = guest.request("GET", "/api/v1/lobbies", {}, guest_session.token, false);
  REQUIRE(listed.status == 200);
  REQUIRE(listed.body["lobbies"].size() == 1);
  CHECK(listed.body["lobbies"][0]["locked"] == false);
  CHECK(listed.body["lobbies"][0]["phase"] == "open");
  CHECK(listed.body["lobbies"][0].contains("metadata"));

  auto ping = guest.request("GET", "/api/v1/lobbies/" + lobby_id + "?client_ts=42", {},
                            guest_session.token, false);
  REQUIRE(ping.status == 200);
  CHECK(ping.body["client_ts"] == 42);
  CHECK(ping.body["lobby"]["lobby_id"] == lobby_id);
  CHECK(ping.body.contains("host_control_rtt_ms"));

  auto by_code =
      guest.request("GET", "/api/v1/lobbies/by-code/play1", {}, guest_session.token, false);
  REQUIRE(by_code.status == 200);
  CHECK(by_code.body["lobby_id"] == lobby_id);

  Client other = server.client();
  const auto other_session = auth(other, kOther, "other-token", "outsider");
  auto hidden = other.request("GET", "/api/v1/lobbies/" + lobby_id, {}, other_session.token, false);
  CHECK(hidden.status == 404);
  auto hidden_list = other.request("GET", "/api/v1/lobbies", {}, other_session.token, false);
  REQUIRE(hidden_list.status == 200);
  CHECK(hidden_list.body["lobbies"].empty());

  auto joined = guest.request("POST", "/api/v1/lobbies/" + lobby_id + "/join", json::object(),
                              guest_session.token);
  REQUIRE(joined.status == 200);
  guest.ws_connect(lobby_id, guest_session.token);
  auto guest_snap = guest.ws_read(std::chrono::seconds(2));
  CHECK(guest_snap["op"] == "lobby_snapshot");
  auto entered = read_where(host,
                            [](const json& msg)
                            {
                              return msg.value("event", "") == "user_entered";
                            });
  CHECK(entered["user_id"] == guest_session.user_id);

  Client peer = server.client();
  const auto peer_session = auth(peer, kGame, "secret", "peer");
  auto peer_joined = peer.request("POST", "/api/v1/lobbies/" + lobby_id + "/join", json::object(),
                                 peer_session.token);
  REQUIRE(peer_joined.status == 200);
  peer.ws_connect(lobby_id, peer_session.token);
  CHECK(peer.ws_read(std::chrono::seconds(2))["op"] == "lobby_snapshot");
  read_where(host,
             [](const json& msg)
             {
               return msg.value("event", "") == "user_entered";
             });
  host.ws_send({{"op", "allocate_relays"},
                {"channels", json::array({{{"a", guest_session.user_id},
                                           {"b", peer_session.user_id},
                                           {"protocol", "udp"},
                                           {"lifetime_sec", 30}}})}});
  auto rejected = read_where(host,
                             [](const json& msg)
                             {
                               return msg.value("op", "") == "error";
                             });
  CHECK(rejected["error"] == "bad_request");

  host.ws_send({{"op", "allocate_relays"},
                {"channels", json::array({{{"a", host_session.user_id},
                                           {"b", guest_session.user_id},
                                           {"protocol", "udp"},
                                           {"lifetime_sec", 30}}})}});
  json host_alloc;
  json host_ok;
  for( int i = 0; i < 8 && (host_alloc.is_null() || host_ok.is_null()); ++i )
  {
    auto msg = host.ws_read(std::chrono::seconds(2));
    if( msg.value("event", "") == "relay_allocated" )
    {
      host_alloc = msg;
    }
    if( msg.value("op", "") == "ok" )
    {
      host_ok = msg;
    }
    if( msg.value("op", "") == "error" )
    {
      FAIL(msg.dump());
    }
  }
  REQUIRE(host_alloc.is_object());
  REQUIRE(host_ok.is_object());
  CHECK_FALSE(host_ok.dump().find(host_alloc["token"].get<std::string>()) != std::string::npos);
  auto guest_alloc = read_where(guest,
                                [](const json& msg)
                                {
                                  return msg.value("event", "") == "relay_allocated";
                                });
  CHECK(guest_alloc["token"] != host_alloc["token"]);
  CHECK(udp_roundtrip(host_alloc, guest_alloc));

  host.ws_send({{"op", "release_relays"}});
  read_where(host,
             [](const json& msg)
             {
               return msg.value("event", "") == "relay_released" || msg.value("op", "") == "ok";
             });
  host.ws_send({{"op", "chat"}, {"text", "still-here"}});
  auto chat = read_where(guest,
                         [](const json& msg)
                         {
                           return msg.value("event", "") == "chat";
                         });
  CHECK(chat["text"] == "still-here");
  CHECK(host.ws_open());
  CHECK(guest.ws_open());

  host.ws_send({{"op", "close_lobby"}});
  auto host_closed = read_where(host,
                                [](const json& msg)
                                {
                                  return msg.value("event", "") == "lobby_closed";
                                });
  auto guest_closed = read_where(guest,
                                 [](const json& msg)
                                 {
                                   return msg.value("event", "") == "lobby_closed";
                                 });
  CHECK(host_closed.value("lobby_id", "") == lobby_id);
  CHECK(guest_closed.value("lobby_id", "") == lobby_id);
  bool host_socket_closed = false;
  bool guest_socket_closed = false;
  try
  {
    host.ws_read(std::chrono::milliseconds(800));
  }
  catch( ... )
  {
    host_socket_closed = true;
  }
  try
  {
    guest.ws_read(std::chrono::milliseconds(800));
  }
  catch( ... )
  {
    guest_socket_closed = true;
  }
  CHECK(host_socket_closed);
  CHECK(guest_socket_closed);

  auto after = guest.request("GET", "/api/v1/lobbies", {}, guest_session.token, false);
  CHECK(after.status == 200);
  auto recreated = host.request("POST", "/api/v1/lobbies", json::object(), host_session.token);
  CHECK(recreated.status == 200);

  boost::asio::io_context stats_ioc;
  boost::asio::ip::tcp::socket stats(stats_ioc);
  stats.connect({boost::asio::ip::make_address("127.0.0.1"), server.app->stats_port()});
  std::string stats_body;
  std::array<char, 2048> chunk{};
  boost::system::error_code ec;
  for( ;; )
  {
    const auto n = stats.read_some(boost::asio::buffer(chunk), ec);
    stats_body.append(chunk.data(), n);
    if( ec )
    {
      break;
    }
  }
  auto stats_json = json::parse(stats_body);
  CHECK(stats_json.contains("connections"));
  CHECK(stats_json.contains("lobbies"));
  CHECK(stats_json.contains("channels"));
  CHECK(stats_json.contains("games"));
}

namespace
{

const char* kAdmin = "ADMIN-TOKEN-FOR-TESTS";

HttpResult admin(Client& client, const std::string& method, const std::string& path,
                 const json& body = json::object())
{
  return client.request(method, "/admin/api/v1" + path, body, kAdmin, false);
}

json config_key(const json& config, const std::string& key)
{
  for( const auto& entry : config.at("keys") )
  {
    if( entry.at("key") == key )
    {
      return entry;
    }
  }
  FAIL("missing config key " << key);
  return {};
}

json row_with(const json& rows, const char* field, const std::string& value)
{
  for( const auto& row : rows )
  {
    if( row.value(field, "") == value )
    {
      return row;
    }
  }
  FAIL("missing row " << value);
  return {};
}

bool socket_closes(Client& client)
{
  try
  {
    for( int i = 0; i < 12; ++i )
    {
      client.ws_read(std::chrono::milliseconds(800));
    }
  }
  catch( ... )
  {
    return true;
  }
  return false;
}

} // namespace

TEST_CASE("admin is off while admin_token is empty")
{
  Server server;
  Client client = server.client();
  CHECK(client.request("GET", "/admin", {}, {}, false).status == 404);
  CHECK(client.request("GET", "/admin/api/v1/config", {}, "anything", false).status == 404);
}

TEST_CASE("admin config writes stay pending until reload")
{
  Server server(std::map<std::string, std::string>{{"admin_token", kAdmin}});
  Client client = server.client();

  auto page = client.request("GET", "/admin", {}, {}, false);
  CHECK(page.status == 200);
  CHECK(page.raw_body.find("Matchmaker admin") != std::string::npos);

  CHECK(client.request("GET", "/admin/api/v1/config", {}, {}, false).status == 401);
  CHECK(client.request("GET", "/admin/api/v1/config", {}, "wrong-token", false).status == 401);
  const auto player = auth(client, kGame, "secret", "player");
  CHECK(client.request("GET", "/admin/api/v1/config", {}, player.token, false).status == 401);

  auto config = admin(client, "GET", "/config");
  REQUIRE(config.status == 200);
  CHECK(config_key(config.body, "admit_sec")["live"] == "60");
  CHECK(config_key(config.body, "admit_sec")["apply"] == "reload");
  CHECK(config_key(config.body, "admit_sec")["pending"] == false);
  CHECK(config_key(config.body, "admin_token")["kind"] == "secret");

  CHECK(admin(client, "POST", "/config", {{"values", {{"no_such_key", "1"}}}}).status == 400);
  CHECK(admin(client, "POST", "/config", {{"values", {{"admit_sec", "soon"}}}}).status == 400);
  CHECK(admin(client, "POST", "/config", {{"values", {{"http_gzip", "maybe"}}}}).status == 400);
  CHECK(admin(client, "POST", "/config", {{"values", {{"admin_token", ""}}}}).status == 400);
  CHECK(admin(client, "POST", "/config", {{"values", {{"listen", "nowhere"}}}}).status == 400);

  auto saved = admin(client, "POST", "/config",
                     {{"values", {{"admit_sec", "30"}, {"listen", "127.0.0.1:1"}}}});
  REQUIRE(saved.status == 200);
  CHECK(config_key(saved.body, "admit_sec")["stored"] == "30");
  CHECK(config_key(saved.body, "admit_sec")["live"] == "60");
  CHECK(config_key(saved.body, "admit_sec")["pending"] == true);

  REQUIRE(admin(client, "POST", "/reload").status == 200);
  config = admin(client, "GET", "/config");
  REQUIRE(config.status == 200);
  CHECK(config_key(config.body, "admit_sec")["live"] == "30");
  CHECK(config_key(config.body, "admit_sec")["pending"] == false);
  const auto listen = config_key(config.body, "listen");
  CHECK(listen["apply"] == "restart");
  CHECK(listen["live"] == "127.0.0.1:0");
  CHECK(listen["pending"] == true);

  auto published = client.request("GET", "/api/v1/config", {}, {}, false);
  REQUIRE(published.status == 200);
  CHECK(published.body.dump().find(kAdmin) == std::string::npos);
}

TEST_CASE("admin auth failures are rate limited")
{
  Server server(std::map<std::string, std::string>{{"admin_token", kAdmin}, {"auth_fail_limit", "2"}});
  Client client = server.client();
  CHECK(client.request("GET", "/admin/api/v1/config", {}, "wrong-1", false).status == 401);
  CHECK(client.request("GET", "/admin/api/v1/config", {}, "wrong-2", false).status == 401);
  CHECK(admin(client, "GET", "/config").status == 429);
}

TEST_CASE("admin games and regions publish on reload")
{
  Server server(std::map<std::string, std::string>{{"admin_token", kAdmin}});
  Client client = server.client();

  CHECK(admin(client, "POST", "/games", {{"name", "No token"}}).status == 400);
  CHECK(admin(client, "POST", "/games", {{"name", "Bad"}, {"token", "t"}, {"limits", {{"bogus", 1}}}})
            .status == 400);
  auto created = admin(client, "POST", "/games",
                       {{"name", "Fresh"},
                        {"token", "fresh-token"},
                        {"limits", {{"max_players_per_lobby", 2}, {"allow_wt", true}}}});
  REQUIRE(created.status == 200);
  CHECK(created.body["created"] == true);
  const auto id = created.body["game"]["id"].get<std::string>();

  auto games = admin(client, "GET", "/games");
  REQUIRE(games.status == 200);
  CHECK(games.body.dump().find("token_hash") == std::string::npos);
  CHECK(games.body.dump().find(sha256_hex("fresh-token")) == std::string::npos);
  auto fresh = row_with(games.body["games"], "id", id);
  CHECK(fresh["pending"] == true);
  CHECK(fresh["live"].is_null());

  const json fresh_auth = {{"game_id", id}, {"token", "fresh-token"}};
  CHECK(client.request("POST", "/api/v1/auth", fresh_auth).status == 401);
  REQUIRE(admin(client, "POST", "/reload").status == 200);
  CHECK(client.request("POST", "/api/v1/auth", fresh_auth).status == 200);

  games = admin(client, "GET", "/games");
  fresh = row_with(games.body["games"], "id", id);
  CHECK(fresh["pending"] == false);
  CHECK(fresh["limits"]["allow_wt"] == true);
  CHECK(fresh["live"]["limits"]["allow_wt"] == false);

  REQUIRE(admin(client, "POST", "/games", {{"id", id}, {"name", "Renamed"}}).status == 200);
  REQUIRE(admin(client, "POST", "/reload").status == 200);
  CHECK(client.request("POST", "/api/v1/auth", fresh_auth).status == 200);

  const auto host = auth(client, kGame, "secret", "host");
  REQUIRE(client.request("POST", "/api/v1/lobbies", json::object(), host.token).status == 200);
  auto blocked = admin(client, "POST", std::string("/games/") + kGame + "/delete");
  CHECK(blocked.status == 409);
  CHECK(blocked.body["error"] == "conflict");
  CHECK(admin(client, "POST", "/games/" + id + "/delete").status == 200);
  CHECK(admin(client, "POST", "/games/" + id + "/delete").status == 404);

  REQUIRE(admin(client, "POST", "/regions",
                {{"id", "eu"}, {"public_url", "https://eu.example"}, {"sort_order", 5}})
              .status == 200);
  CHECK(admin(client, "POST", "/regions/local/delete").status == 409);
  REQUIRE(admin(client, "POST", "/reload").status == 200);
  auto published = client.request("GET", "/api/v1/config", {}, {}, false);
  REQUIRE(published.status == 200);
  CHECK(row_with(published.body["regions"], "id", "eu")["public_url"] == "https://eu.example");
}

TEST_CASE("admin lobby controls reach members and hide secrets")
{
  Server server(std::map<std::string, std::string>{{"admin_token", kAdmin}});
  Client ops = server.client();
  Client host = server.client();
  Client guest = server.client();
  const auto host_session = auth(host, kGame, "secret", "host");
  const auto guest_session = auth(guest, kGame, "secret", "guest");

  auto created = host.request("POST", "/api/v1/lobbies", json::object(), host_session.token);
  REQUIRE(created.status == 200);
  const auto lobby_id = created.body.at("lobby_id").get<std::string>();
  host.ws_connect(lobby_id, host_session.token);
  REQUIRE(host.ws_read(std::chrono::seconds(2))["op"] == "lobby_snapshot");

  auto listed = admin(ops, "GET", "/lobbies");
  REQUIRE(listed.status == 200);
  CHECK(row_with(listed.body["lobbies"], "lobby_id", lobby_id)["phase"] == "closed");

  REQUIRE(admin(ops, "POST", "/lobbies/" + lobby_id, {{"op", "set_phase"}, {"phase", "open"}})
              .status == 200);
  auto phase = read_where(host,
                          [](const json& msg)
                          {
                            return msg.value("event", "") == "lobby_phase_changed";
                          });
  CHECK(phase["phase"] == "open");

  REQUIRE(guest.request("POST", "/api/v1/lobbies/" + lobby_id + "/join", json::object(),
                        guest_session.token)
              .status == 200);
  guest.ws_connect(lobby_id, guest_session.token);
  REQUIRE(guest.ws_read(std::chrono::seconds(2))["op"] == "lobby_snapshot");
  read_where(host,
             [](const json& msg)
             {
               return msg.value("event", "") == "user_entered";
             });

  host.ws_send({{"op", "allocate_relays"},
                {"channels", json::array({{{"a", host_session.user_id},
                                           {"b", guest_session.user_id},
                                           {"protocol", "udp"},
                                           {"lifetime_sec", 30}}})}});
  auto host_alloc = read_where(host,
                               [](const json& msg)
                               {
                                 return msg.value("event", "") == "relay_allocated";
                               });
  auto guest_alloc = read_where(guest,
                                [](const json& msg)
                                {
                                  return msg.value("event", "") == "relay_allocated";
                                });

  REQUIRE(admin(ops, "POST", "/lobbies/" + lobby_id,
                {{"op", "set_password"}, {"password", "room-secret-pw"}})
              .status == 200);
  auto locked = read_where(host,
                           [](const json& msg)
                           {
                             return msg.value("event", "") == "lock_changed";
                           });
  CHECK(locked["locked"] == true);

  auto view = admin(ops, "GET", "/lobbies/" + lobby_id);
  REQUIRE(view.status == 200);
  CHECK(view.body["locked"] == true);
  CHECK_FALSE(view.body.contains("password"));
  REQUIRE(view.body["channels"].size() == 1);
  CHECK(view.body["members"].size() == 2);
  listed = admin(ops, "GET", "/lobbies");
  CHECK(row_with(listed.body["lobbies"], "lobby_id", lobby_id)["channel_count"] == 1);
  for( const auto& dump : {view.body.dump(), listed.body.dump()} )
  {
    CHECK(dump.find("room-secret-pw") == std::string::npos);
    CHECK(dump.find(host_alloc["token"].get<std::string>()) == std::string::npos);
    CHECK(dump.find(guest_alloc["token"].get<std::string>()) == std::string::npos);
  }

  CHECK(admin(ops, "POST", "/lobbies/" + lobby_id,
              {{"op", "release_channel"}, {"channel_id", "not-a-channel"}})
            .status == 404);
  REQUIRE(admin(ops, "POST", "/lobbies/" + lobby_id,
                {{"op", "release_channel"},
                 {"channel_id", view.body["channels"][0]["id"].get<std::string>()}})
              .status == 200);
  read_where(host,
             [](const json& msg)
             {
               return msg.value("event", "") == "relay_released";
             });

  REQUIRE(admin(ops, "POST", "/lobbies/" + lobby_id,
                {{"op", "kick"}, {"user_id", guest_session.user_id}})
              .status == 200);
  auto kicked = read_where(guest,
                           [](const json& msg)
                           {
                             return msg.value("event", "") == "kicked";
                           });
  CHECK(kicked["lobby_id"] == lobby_id);
  CHECK(socket_closes(guest));
  auto left = read_where(host,
                         [](const json& msg)
                         {
                           return msg.value("event", "") == "user_left";
                         });
  CHECK(left["user_id"] == guest_session.user_id);

  auto closed = admin(ops, "POST", "/lobbies/" + lobby_id, {{"op", "close"}});
  REQUIRE(closed.status == 200);
  CHECK(closed.body["lobby"].is_null());
  auto ended = read_where(host,
                          [](const json& msg)
                          {
                            return msg.value("event", "") == "lobby_closed";
                          });
  CHECK(ended["reason"] == "admin");
  CHECK(socket_closes(host));
  CHECK(admin(ops, "GET", "/lobbies/" + lobby_id).status == 404);
}
