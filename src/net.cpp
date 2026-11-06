#include "app.hpp"

#include "catalog.hpp"
#include "crypto.hpp"
#include "jwt.hpp"
#include "lobby.hpp"
#include "log.hpp"
#include "queue.hpp"
#include "relay.hpp"
#include "store.hpp"
#include "util.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <thread>

namespace matchmaker
{
namespace
{

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

json error_body(const Error& error)
{
  json body = {{"error", error_code_string(error.code)}, {"message", error.message}};
  if( error.max_lifetime_sec )
  {
    body["max_lifetime_sec"] = *error.max_lifetime_sec;
  }
  return body;
}

json frame_ok(const json& request)
{
  json body = {{"op", "ok"}};
  if( request.contains("id") )
  {
    body["id"] = request["id"];
  }
  return body;
}

json frame_error(const json& request, const Error& error)
{
  json body = error_body(error);
  body["op"] = "error";
  if( request.contains("id") )
  {
    body["id"] = request["id"];
  }
  return body;
}

std::string query_value(const std::string& query, const std::string& key)
{
  std::size_t start = 0;
  while( start < query.size() )
  {
    auto end = query.find('&', start);
    if( end == std::string::npos )
    {
      end = query.size();
    }
    const auto part = query.substr(start, end - start);
    const auto eq = part.find('=');
    const auto name = url_decode(eq == std::string::npos ? part : part.substr(0, eq));
    if( name == key )
    {
      return eq == std::string::npos ? "" : url_decode(part.substr(eq + 1));
    }
    start = end + 1;
  }
  return {};
}

bool has_query(const std::string& query, const std::string& key)
{
  std::size_t start = 0;
  while( start < query.size() )
  {
    auto end = query.find('&', start);
    if( end == std::string::npos )
    {
      end = query.size();
    }
    const auto part = query.substr(start, end - start);
    const auto eq = part.find('=');
    if( url_decode(eq == std::string::npos ? part : part.substr(0, eq)) == key )
    {
      return true;
    }
    start = end + 1;
  }
  return false;
}

struct ParsedTarget
{
  std::string path;
  std::string query;
};

ParsedTarget parse_target(std::string_view target)
{
  const auto pos = target.find('?');
  if( pos == std::string_view::npos )
  {
    return {std::string(target), {}};
  }
  return {std::string(target.substr(0, pos)), std::string(target.substr(pos + 1))};
}

bool accepts_gzip(const http::request<http::string_body>& req)
{
  const auto value = req[http::field::accept_encoding];
  std::size_t start = 0;
  while( start < value.size() )
  {
    auto end = value.find(',', start);
    if( end == std::string::npos )
    {
      end = value.size();
    }
    auto token = value.substr(start, end - start);
    const auto semi = token.find(';');
    if( semi != std::string::npos )
    {
      token = token.substr(0, semi);
    }
    while( !token.empty() && token.front() == ' ' )
    {
      token.remove_prefix(1);
    }
    while( !token.empty() && token.back() == ' ' )
    {
      token.remove_suffix(1);
    }
    if( token == "gzip" )
    {
      return true;
    }
    start = end + 1;
  }
  return false;
}

} // namespace

struct App::State
{
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
  using Clock = std::chrono::steady_clock;

  struct User
  {
    std::string user_id;
    std::string game_id;
    std::string jti;
    std::string alias;
    std::string external_id;
  };

  struct AllocMeta
  {
    std::string lobby_id;
    std::string game_id;
  };

  struct FailWindow
  {
    int count = 0;
    Clock::time_point start = Clock::now();
  };

  class WsSession;

  std::string database_path;
  std::unique_ptr<Store> store;
  Catalog catalog;
  std::string boot_id = uuid_v4();
  boost::asio::io_context ioc{1};
  Strand strand;
  std::unique_ptr<RelayService> relay;
  LobbyService lobbies;
  QueueService queue;
  std::unordered_map<std::string, User> users;
  std::unordered_map<std::string, FailWindow> auth_fails;
  std::unordered_map<std::string, std::weak_ptr<WsSession>> sockets;
  std::unordered_map<std::string, AllocMeta> allocs;
  std::unordered_map<std::string, int> game_channels;
  std::unordered_map<std::string, int> lobby_channels;
  tcp::acceptor https_acceptor;
  tcp::acceptor stats_acceptor;
  ssl::context ssl_ctx;
  boost::asio::steady_timer ticker;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  std::vector<std::thread> threads;
  std::unique_ptr<boost::asio::signal_set> signals;
  std::atomic<int> connections{0};
  bool stopping = false;
  bool joined = false;
  std::uint16_t https_port_ = 0;
  std::uint16_t stats_port_ = 0;
  Clock::time_point last_prune = Clock::now();

  explicit State(std::string path)
      : database_path(std::move(path)), strand(boost::asio::make_strand(ioc)), https_acceptor(ioc),
        stats_acceptor(ioc), ssl_ctx(ssl::context::tls_server), ticker(ioc),
        work(ioc.get_executor())
  {
    relay = std::make_unique<RelayService>(ioc, strand);
  }

  void load_catalog()
  {
    catalog.replace(SystemConfig::from_map(store->load_config()), store->load_regions(),
                    store->load_games());
  }

  void apply_limits()
  {
    const auto& cfg = catalog.system();
    Timing timing;
    timing.admit = std::chrono::seconds(std::max(1, cfg.admit_sec));
    timing.join_hold = std::chrono::seconds(std::max(1, cfg.join_hold_sec));
    timing.heartbeat_interval = std::chrono::seconds(std::max(1, cfg.host_heartbeat_interval_sec));
    timing.heartbeat_misses = std::max(1, cfg.host_heartbeat_misses);
    lobbies.set_timing(timing);
    lobbies.set_system_player_cap(cfg.max_players_per_lobby);
    lobbies.set_system_password_cap(cfg.max_password_length);
    RelayConfig relay_cfg;
    relay_cfg.bind_host = cfg.relay_bind;
    relay_cfg.port_start = cfg.relay_port_start;
    relay_cfg.port_end = cfg.relay_port_end;
    relay_cfg.max_concurrent = cfg.max_concurrent_channels;
    relay_cfg.max_packet = cfg.max_udp_packet_bytes;
    relay_cfg.tcp_enabled = cfg.relay_tcp_enabled;
    relay->configure(relay_cfg);
  }

  void wire()
  {
    lobbies.set_rules(
        [this](const std::string& id)
        {
          return catalog.game(id);
        });
    lobbies.set_emit(
        [this](const json& event, const std::vector<std::string>& recipients)
        {
          fanout(event, recipients);
        });
    lobbies.set_on_destroyed(
        [this](const std::string& lobby_id)
        {
          relay->release_lobby(lobby_id, Clock::now());
          queue.on_lobby_destroyed(lobby_id);
        });
    queue.set_match_fn(
        [this](const std::string& game_id, const std::vector<Actor>& actors, LobbyService::Time now)
        {
          return lobbies.create_match(game_id, actors, now);
        });
    relay->set_on_closed(
        [this](const RelayClosed& closed)
        {
          on_relay_closed(closed);
        });
  }

  Actor actor_of(const User& user) const
  {
    return Actor{user.user_id, user.game_id, user.alias, user.external_id};
  }

  const User* user_from_bearer(const std::string& header, Error& error) const
  {
    constexpr std::string_view prefix = "Bearer ";
    if( !header.starts_with(prefix) )
    {
      error = Error{ErrorCode::Unauthorized, "bearer token required", std::nullopt};
      return nullptr;
    }
    const auto token = header.substr(prefix.size());
    const auto now_sec = wall_ms() / 1000;
    auto claims = jwt_verify(token, catalog.system().jwt_secret, now_sec);
    if( !claims )
    {
      error = claims.error();
      return nullptr;
    }
    if( claims->boot_id != boot_id )
    {
      error = Error{ErrorCode::Unauthorized, "session ended", std::nullopt};
      return nullptr;
    }
    const auto it = users.find(claims->sub);
    if( it == users.end() || it->second.jti != claims->jti )
    {
      error = Error{ErrorCode::Unauthorized, "unknown session", std::nullopt};
      return nullptr;
    }
    return &it->second;
  }

  json public_config() const
  {
    const auto& cfg = catalog.system();
    json regions = json::array();
    json self = nullptr;
    for( const auto& region : catalog.regions() )
    {
      json item = {{"id", region.id}, {"public_url", region.public_url}};
      regions.push_back(item);
      if( region.id == cfg.region_id )
      {
        self = item;
      }
    }
    if( self.is_null() )
    {
      self = {{"id", cfg.region_id}, {"public_url", cfg.public_url}};
    }
    json compression = {{"http", json::array()}, {"websocket", json::array()}};
    if( cfg.http_gzip )
    {
      compression["http"].push_back("gzip");
    }
    if( cfg.websocket_permessage_deflate )
    {
      compression["websocket"].push_back("permessage-deflate");
    }
    return {{"health", {{"status", "ok"}}},
            {"region", self},
            {"regions", regions},
            {"compression", compression},
            {"limits", {{"max_message_bytes", cfg.max_message_bytes}}}};
  }

  json stats_json() const
  {
    int lobby_total = 0;
    int queued = 0;
    json games = json::array();
    for( const auto& game : catalog.games() )
    {
      const int count = lobbies.lobby_count(game.id);
      lobby_total += count;
      queued += queue.queued_count(game.id);
      games.push_back({{"id", game.id}, {"lobbies", count}});
    }
    return {{"connections", connections.load()},
            {"lobbies", lobby_total},
            {"queue_depth", queued},
            {"channels", relay->channels()},
            {"forwarded_bytes", relay->forwarded_bytes()},
            {"drops", relay->drops()},
            {"games", games}};
  }

  void fanout(const json& event, const std::vector<std::string>& recipients);
  void on_relay_closed(const RelayClosed& closed);
  void send_to(const std::string& user_id, const json& message, bool then_close);

  struct HttpOut
  {
    int status = 200;
    json body = json::object();
    bool gzip = true;
  };
  using Respond = std::function<void(HttpOut)>;

  void handle_http(const http::request<http::string_body>& req, const std::string& remote,
                   Respond respond);
  void handle_ws_message(const std::string& user_id, const std::string& lobby_id,
                         const json& message,
                         const std::function<void(const json&, bool close_after)>& send);

  void schedule_tick()
  {
    ticker.expires_after(std::chrono::milliseconds(250));
    ticker.async_wait(boost::asio::bind_executor(
        strand,
        [this](boost::system::error_code ec)
        {
          if( ec || stopping )
          {
            return;
          }
          const auto now = Clock::now();
          lobbies.tick(now);
          relay->tick(now);
          if( now - last_prune > std::chrono::hours(1) )
          {
            last_prune = now;
            const auto cutoff =
                wall_ms() -
                static_cast<std::int64_t>(catalog.system().retention_days) * 24 * 60 * 60 * 1000;
            try
            {
              store->prune_logs(cutoff);
            }
            catch( const std::exception& ex )
            {
              std::cerr << "prune failed: " << ex.what() << '\n';
            }
          }
          schedule_tick();
        }));
  }

  void arm_signals()
  {
    signals->async_wait(
        [this](boost::system::error_code ec, int sig)
        {
          if( ec || stopping )
          {
            return;
          }
          bool reload = false;
#ifdef SIGHUP
          if( sig == SIGHUP )
          {
            reload = true;
          }
#endif
#ifdef SIGBREAK
          if( sig == SIGBREAK )
          {
            reload = true;
          }
#endif
          if( reload )
          {
            boost::asio::dispatch(strand,
                                  [this]
                                  {
                                    reload_catalog();
                                  });
            arm_signals();
            return;
          }
          stopping = true;
          work.reset();
          ioc.stop();
        });
  }

  void reload_catalog()
  {
    try
    {
      load_catalog();
      apply_limits();
      write_log(store.get(), LogLevel::Info, "config", "reloaded configuration");
    }
    catch( const std::exception& ex )
    {
      write_log(store.get(), LogLevel::Error, "config", ex.what());
    }
  }

  void do_https_accept();
  void do_stats_accept();

  bool too_many_failures(const std::string& remote)
  {
    const auto& cfg = catalog.system();
    auto& window = auth_fails[remote];
    const auto now = Clock::now();
    if( now - window.start > std::chrono::seconds(std::max(1, cfg.auth_fail_window_sec)) )
    {
      window.start = now;
      window.count = 0;
    }
    return window.count >= cfg.auth_fail_limit;
  }

  void note_failure(const std::string& remote)
  {
    auto& window = auth_fails[remote];
    const auto now = Clock::now();
    if( now - window.start >
        std::chrono::seconds(std::max(1, catalog.system().auth_fail_window_sec)) )
    {
      window.start = now;
      window.count = 0;
    }
    ++window.count;
  }
};

class App::State::WsSession : public std::enable_shared_from_this<WsSession>
{
public:
  WsSession(State& state, beast::ssl_stream<beast::tcp_stream> stream,
            http::request<http::string_body> req, std::string user_id, std::string lobby_id)
      : state_(state), ws_(std::move(stream)), req_(std::move(req)), user_id_(std::move(user_id)),
        lobby_id_(std::move(lobby_id))
  {
  }

  void run()
  {
    const auto& cfg = state_.catalog.system();
    websocket::stream_base::timeout timeout;
    timeout.handshake_timeout = std::chrono::seconds(10);
    timeout.idle_timeout = std::chrono::seconds(
        std::max(1, cfg.host_heartbeat_interval_sec * cfg.host_heartbeat_misses));
    timeout.keep_alive_pings = true;
    ws_.set_option(timeout);
    ws_.read_message_max(static_cast<std::size_t>(std::max(1024, cfg.max_frame_bytes)));
    if( cfg.websocket_permessage_deflate )
    {
      websocket::permessage_deflate deflate;
      deflate.server_enable = true;
      deflate.client_enable = false;
      deflate.server_no_context_takeover = true;
      deflate.client_no_context_takeover = true;
      deflate.server_max_window_bits = std::clamp(cfg.websocket_max_window_bits, 9, 15);
      deflate.compLevel = std::clamp(cfg.websocket_deflate_level, 0, 9);
      ws_.set_option(deflate);
    }
    ws_.control_callback(
        [self = shared_from_this()](websocket::frame_type kind, beast::string_view)
        {
          if( kind == websocket::frame_type::pong )
          {
            self->on_pong();
          }
        });
    ws_.async_accept(req_,
                     boost::asio::bind_executor(state_.strand,
                                                [self = shared_from_this()](beast::error_code ec)
                                                {
                                                  if( ec )
                                                  {
                                                    self->state_.connections.fetch_sub(1);
                                                    return;
                                                  }
                                                  self->accepted_ = true;
                                                  self->on_open();
                                                }));
  }

  void send(const json& message, bool then_close)
  {
    auto text = std::make_shared<std::string>(message.dump());
    const auto cap =
        static_cast<std::size_t>(std::max(1024, state_.catalog.system().max_outbound_queue_bytes));
    if( queued_bytes_ + text->size() > cap )
    {
      depart();
      return;
    }
    if( then_close )
    {
      close_after_ = true;
    }
    outbound_.push_back(std::move(text));
    queued_bytes_ += outbound_.back()->size();
    pump();
  }

  void probe(std::chrono::milliseconds timeout, std::function<void(std::optional<int>)> cb)
  {
    if( probing_ )
    {
      cb(last_rtt_);
      return;
    }
    probing_ = true;
    probe_started_ = Clock::now();
    probe_cb_ = std::move(cb);
    probe_timer_.emplace(state_.strand);
    probe_timer_->expires_after(timeout);
    probe_timer_->async_wait(
        [self = shared_from_this()](boost::system::error_code ec)
        {
          if( ec || !self->probing_ )
          {
            return;
          }
          self->finish_probe(std::nullopt);
        });
    ws_.async_ping({}, boost::asio::bind_executor(state_.strand,
                                                  [self = shared_from_this()](beast::error_code ec)
                                                  {
                                                    if( ec )
                                                    {
                                                      self->finish_probe(std::nullopt);
                                                    }
                                                  }));
  }

  ~WsSession()
  {
    if( accepted_ )
    {
      state_.connections.fetch_sub(1);
    }
  }

private:
  using Clock = std::chrono::steady_clock;

  void on_open()
  {
    state_.sockets[user_id_] = weak_from_this();
    auto snapshot = state_.lobbies.complete_connect(user_id_, lobby_id_, Clock::now());
    if( !snapshot )
    {
      state_.sockets.erase(user_id_);
      send(frame_error(json::object(), snapshot.error()), true);
      return;
    }
    state_.queue.cancel_queued_user(user_id_);
    send(*snapshot, false);
    do_read();
  }

  void do_read()
  {
    ws_.async_read(
        buffer_,
        boost::asio::bind_executor(
            state_.strand,
            [self = shared_from_this()](beast::error_code ec, std::size_t)
            {
              if( ec )
              {
                self->depart();
                return;
              }
              const auto text = beast::buffers_to_string(self->buffer_.data());
              self->buffer_.consume(self->buffer_.size());
              auto message = json::parse(text, nullptr, false);
              if( !message.is_object() || !message.contains("op") )
              {
                self->send(
                    frame_error(json::object(), Error{ErrorCode::BadRequest,
                                                      "expected an op object", std::nullopt}),
                    false);
              }
              else
              {
                self->state_.handle_ws_message(self->user_id_, self->lobby_id_, message,
                                               [self](const json& outbound, bool close_after)
                                               {
                                                 self->send(outbound, close_after);
                                               });
              }
              if( !self->departed_ )
              {
                self->do_read();
              }
            }));
  }

  void pump()
  {
    if( writing_ || outbound_.empty() )
    {
      if( !writing_ && outbound_.empty() && close_after_ )
      {
        start_close();
      }
      return;
    }
    writing_ = true;
    ws_.text(true);
    ws_.async_write(
        boost::asio::buffer(*outbound_.front()),
        boost::asio::bind_executor(state_.strand,
                                   [self = shared_from_this()](beast::error_code ec, std::size_t)
                                   {
                                     self->writing_ = false;
                                     if( !self->outbound_.empty() )
                                     {
                                       self->queued_bytes_ -= self->outbound_.front()->size();
                                       self->outbound_.pop_front();
                                     }
                                     if( ec )
                                     {
                                       self->depart();
                                       return;
                                     }
                                     self->pump();
                                   }));
  }

  void start_close()
  {
    if( closing_ )
    {
      return;
    }
    closing_ = true;
    ws_.async_close(websocket::close_code::normal,
                    boost::asio::bind_executor(state_.strand,
                                               [self = shared_from_this()](beast::error_code)
                                               {
                                                 self->departed_ = true;
                                                 self->state_.sockets.erase(self->user_id_);
                                               }));
  }

  void depart()
  {
    if( departed_ )
    {
      return;
    }
    departed_ = true;
    state_.lobbies.leave(user_id_, Clock::now());
    state_.sockets.erase(user_id_);
    close_after_ = true;
    if( !writing_ )
    {
      start_close();
    }
  }

  void on_pong()
  {
    if( !probing_ )
    {
      return;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - probe_started_)
            .count();
    finish_probe(static_cast<int>(ms));
  }

  void finish_probe(std::optional<int> rtt)
  {
    if( !probing_ )
    {
      return;
    }
    probing_ = false;
    if( rtt )
    {
      last_rtt_ = rtt;
    }
    if( probe_timer_ )
    {
      probe_timer_->cancel();
    }
    if( probe_cb_ )
    {
      auto cb = std::move(probe_cb_);
      probe_cb_ = nullptr;
      cb(rtt ? rtt : last_rtt_);
    }
  }

  State& state_;
  websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws_;
  http::request<http::string_body> req_;
  beast::flat_buffer buffer_;
  std::string user_id_;
  std::string lobby_id_;
  std::deque<std::shared_ptr<std::string>> outbound_;
  std::size_t queued_bytes_ = 0;
  bool writing_ = false;
  bool close_after_ = false;
  bool closing_ = false;
  bool departed_ = false;
  bool accepted_ = false;
  bool probing_ = false;
  std::optional<int> last_rtt_;
  Clock::time_point probe_started_{};
  std::function<void(std::optional<int>)> probe_cb_;
  std::optional<boost::asio::steady_timer> probe_timer_;
};

void App::State::fanout(const json& event, const std::vector<std::string>& recipients)
{
  const bool closing = event.value("event", "") == "lobby_closed";
  for( const auto& user_id : recipients )
  {
    const auto it = sockets.find(user_id);
    if( it == sockets.end() )
    {
      continue;
    }
    if( auto session = it->second.lock() )
    {
      session->send(event, closing);
    }
  }
}

void App::State::send_to(const std::string& user_id, const json& message, bool then_close)
{
  const auto it = sockets.find(user_id);
  if( it == sockets.end() )
  {
    return;
  }
  if( auto session = it->second.lock() )
  {
    session->send(message, then_close);
  }
}

void App::State::on_relay_closed(const RelayClosed& closed)
{
  const auto meta = allocs.find(closed.id);
  if( meta != allocs.end() )
  {
    if( auto game = game_channels.find(meta->second.game_id);
        game != game_channels.end() && game->second > 0 )
    {
      --game->second;
    }
    if( auto lobby = lobby_channels.find(meta->second.lobby_id);
        lobby != lobby_channels.end() && lobby->second > 0 )
    {
      --lobby->second;
    }
    allocs.erase(meta);
  }
  const char* name = closed.reason == "released" ? "relay_released" : "relay_expired";
  json event = {{"op", "event"},
                {"event", name},
                {"lobby_id", closed.lobby_id},
                {"channel_id", closed.id},
                {"reason", closed.reason}};
  send_to(closed.user_a, event, false);
  send_to(closed.user_b, event, false);
}

class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
  HttpSession(App::State& state, tcp::socket socket)
      : state_(state), stream_(std::move(socket), state.ssl_ctx)
  {
  }

  void run()
  {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(15));
    stream_.async_handshake(
        ssl::stream_base::server,
        boost::asio::bind_executor(state_.strand,
                                   [self = shared_from_this()](beast::error_code ec)
                                   {
                                     if( ec )
                                     {
                                       self->release_count();
                                       return;
                                     }
                                     self->do_read();
                                   }));
  }

  ~HttpSession()
  {
    if( !handed_off_ )
    {
      release_count();
    }
  }

private:
  void do_read()
  {
    parser_.emplace();
    parser_->body_limit(
        static_cast<std::uint64_t>(std::max(1024, state_.catalog.system().max_message_bytes)));
    req_ = {};
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
    http::async_read(
        stream_, buffer_, *parser_,
        boost::asio::bind_executor(
            state_.strand,
            [self = shared_from_this()](beast::error_code ec, std::size_t)
            {
              if( ec == http::error::body_limit )
              {
                self->write_out(App::State::HttpOut{
                    413, error_body(Error{ErrorCode::TooLarge, "body too large", std::nullopt}),
                    true});
                return;
              }
              if( ec )
              {
                return;
              }
              self->req_ = self->parser_->release();
              if( websocket::is_upgrade(self->req_) )
              {
                self->upgrade();
                return;
              }
              std::string remote = "unknown";
              beast::error_code ep_ec;
              auto endpoint =
                  beast::get_lowest_layer(self->stream_).socket().remote_endpoint(ep_ec);
              if( !ep_ec )
              {
                remote = endpoint.address().to_string();
              }
              self->state_.handle_http(self->req_, remote,
                                       [self](App::State::HttpOut out)
                                       {
                                         self->write_out(std::move(out));
                                       });
            }));
  }

  void write_out(App::State::HttpOut out)
  {
    std::string body = out.body.dump();
    res_.emplace();
    res_->result(out.status);
    res_->version(req_.version());
    res_->set(http::field::content_type, "application/json; charset=utf-8");
    res_->keep_alive(req_.keep_alive());
    const auto& cfg = state_.catalog.system();
    if( out.gzip && cfg.http_gzip && accepts_gzip(req_) &&
        static_cast<int>(body.size()) >= cfg.http_gzip_min_bytes )
    {
      auto compressed = gzip_compress(body);
      if( !compressed.empty() )
      {
        body = std::move(compressed);
        res_->set(http::field::content_encoding, "gzip");
        res_->set(http::field::vary, "Accept-Encoding");
      }
    }
    res_->body() = std::move(body);
    res_->prepare_payload();
    http::async_write(
        stream_, *res_,
        boost::asio::bind_executor(state_.strand,
                                   [self = shared_from_this()](beast::error_code ec, std::size_t)
                                   {
                                     if( ec )
                                     {
                                       return;
                                     }
                                     if( self->res_->need_eof() )
                                     {
                                       beast::error_code shutdown;
                                       self->stream_.shutdown(shutdown);
                                       return;
                                     }
                                     self->do_read();
                                   }));
  }

  void release_count()
  {
    if( holds_count_ )
    {
      holds_count_ = false;
      state_.connections.fetch_sub(1);
    }
  }

  void upgrade()
  {
    const auto target = parse_target(std::string(req_.target()));
    constexpr std::string_view prefix = "/ws/v1/lobbies/";
    if( !target.path.starts_with(prefix) || target.path.size() == prefix.size() )
    {
      write_out(App::State::HttpOut{
          404, error_body(Error{ErrorCode::NotFound, "not found", std::nullopt}), true});
      return;
    }
    Error error;
    const auto* user =
        state_.user_from_bearer(std::string(req_[http::field::authorization]), error);
    if( !user )
    {
      write_out(App::State::HttpOut{http_status(error.code), error_body(error), true});
      return;
    }
    const auto lobby_id = url_decode(target.path.substr(prefix.size()));
    const auto owned = state_.lobbies.lobby_of(user->user_id);
    if( !owned || *owned != lobby_id )
    {
      const auto code = owned ? ErrorCode::AlreadyInLobby : ErrorCode::NotMember;
      const char* message = owned ? "already in a lobby" : "no join reservation";
      write_out(App::State::HttpOut{http_status(code),
                                    error_body(Error{code, message, std::nullopt}), true});
      return;
    }
    handed_off_ = true;
    std::make_shared<App::State::WsSession>(state_, std::move(stream_), std::move(req_),
                                            user->user_id, lobby_id)
        ->run();
  }

  App::State& state_;
  beast::ssl_stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  http::request<http::string_body> req_;
  std::optional<http::response<http::string_body>> res_;
  bool handed_off_ = false;
  bool holds_count_ = true;
};

class StatsSession : public std::enable_shared_from_this<StatsSession>
{
public:
  StatsSession(App::State& state, tcp::socket socket) : state_(state), socket_(std::move(socket))
  {
  }

  void run()
  {
    boost::asio::dispatch(state_.strand,
                          [self = shared_from_this()]
                          {
                            self->body_ = self->state_.stats_json().dump();
                            boost::asio::async_write(
                                self->socket_, boost::asio::buffer(self->body_),
                                [self](boost::system::error_code, std::size_t)
                                {
                                  boost::system::error_code ec;
                                  self->socket_.shutdown(tcp::socket::shutdown_send, ec);
                                });
                          });
  }

private:
  App::State& state_;
  tcp::socket socket_;
  std::string body_;
};

void App::State::do_https_accept()
{
  https_acceptor.async_accept(boost::asio::bind_executor(
      strand,
      [this](boost::system::error_code ec, tcp::socket socket)
      {
        if( stopping )
        {
          return;
        }
        if( !ec )
        {
          const int limit = std::max(1, catalog.system().max_connections);
          if( connections.load() >= limit )
          {
            socket.close();
          }
          else
          {
            connections.fetch_add(1);
            std::make_shared<HttpSession>(*this, std::move(socket))->run();
          }
        }
        if( !stopping )
        {
          do_https_accept();
        }
      }));
}

void App::State::do_stats_accept()
{
  stats_acceptor.async_accept(
      [this](boost::system::error_code ec, tcp::socket socket)
      {
        if( stopping )
        {
          return;
        }
        if( !ec )
        {
          std::make_shared<StatsSession>(*this, std::move(socket))->run();
        }
        if( !stopping )
        {
          do_stats_accept();
        }
      });
}

void App::State::handle_http(const http::request<http::string_body>& req, const std::string& remote,
                             Respond respond)
{
  const auto target = parse_target(std::string(req.target()));
  const auto method = req.method();
  auto finish_error = [&](const Error& error, int status = 0)
  {
    respond(HttpOut{status == 0 ? http_status(error.code) : status, error_body(error), true});
  };
  if( method == http::verb::get && target.path == "/api/v1/health" )
  {
    respond(HttpOut{200, {{"status", "ok"}, {"region", catalog.system().region_id}}, true});
    return;
  }
  if( method == http::verb::get && target.path == "/api/v1/config" )
  {
    respond(HttpOut{200, public_config(), true});
    return;
  }
  json body = json::object();
  if( !req.body().empty() )
  {
    body = json::parse(req.body(), nullptr, false);
    if( body.is_discarded() )
    {
      finish_error(Error{ErrorCode::BadRequest, "invalid json", std::nullopt});
      return;
    }
  }
  if( method == http::verb::post && target.path == "/api/v1/auth" )
  {
    if( too_many_failures(remote) )
    {
      finish_error(Error{ErrorCode::Limit, "too many auth failures", std::nullopt}, 429);
      return;
    }
    const auto game_id = body.value("game_id", "");
    const auto token = body.value("token", "");
    const auto* game = catalog.game(game_id);
    const auto hashed = sha256_hex(token);
    if( !game || token.empty() || !ct_equal(hashed, game->token_hash) )
    {
      note_failure(remote);
      write_log(store.get(), LogLevel::Warn, "auth", "authentication failed", game_id);
      finish_error(game ? Error{ErrorCode::Unauthorized, "bad token", std::nullopt}
                        : Error{ErrorCode::UnknownGame, "unknown game", std::nullopt});
      return;
    }
    auto alias = body.value("alias", "");
    auto external = body.value("external_id", "");
    if( static_cast<int>(alias.size()) > game->limits.max_alias_bytes || external.size() > 128 )
    {
      finish_error(Error{ErrorCode::TooLarge, "alias or external_id is too long", std::nullopt});
      return;
    }
    User user;
    user.user_id = uuid_v4();
    user.game_id = game_id;
    user.jti = uuid_v4();
    user.alias = std::move(alias);
    user.external_id = std::move(external);
    const auto now_sec = wall_ms() / 1000;
    JwtClaims claims;
    claims.sub = user.user_id;
    claims.game_id = game_id;
    claims.jti = user.jti;
    claims.boot_id = boot_id;
    claims.iat = now_sec;
    claims.exp = now_sec + catalog.system().jwt_ttl_sec;
    auto access = jwt_sign(claims, catalog.system().jwt_secret);
    users.emplace(user.user_id, user);
    respond(HttpOut{200,
                    {{"token_type", "Bearer"},
                     {"access_token", access},
                     {"expires_in", catalog.system().jwt_ttl_sec},
                     {"user_id", user.user_id},
                     {"game_id", game_id}},
                    true});
    return;
  }

  Error auth_error;
  const auto* user = user_from_bearer(std::string(req[http::field::authorization]), auth_error);
  if( !user )
  {
    finish_error(auth_error);
    return;
  }
  const auto actor = actor_of(*user);
  const auto now = Clock::now();

  if( method == http::verb::get && target.path == "/api/v1/lobbies" )
  {
    int limit = 50;
    if( has_query(target.query, "limit") )
    {
      try
      {
        limit = std::stoi(query_value(target.query, "limit"));
      }
      catch( ... )
      {
        finish_error(Error{ErrorCode::BadRequest, "bad limit", std::nullopt});
        return;
      }
    }
    auto listed = lobbies.list(actor.game_id, query_value(target.query, "cursor"), limit);
    if( !listed )
    {
      finish_error(listed.error());
      return;
    }
    respond(HttpOut{200, *listed, true});
    return;
  }
  constexpr std::string_view by_code = "/api/v1/lobbies/by-code/";
  if( method == http::verb::get && target.path.starts_with(by_code) )
  {
    auto found = lobbies.get_by_code(actor.game_id, url_decode(target.path.substr(by_code.size())));
    if( !found )
    {
      finish_error(found.error());
      return;
    }
    respond(HttpOut{200, *found, true});
    return;
  }
  constexpr std::string_view lobby_prefix = "/api/v1/lobbies/";
  if( target.path.starts_with(lobby_prefix) )
  {
    auto rest = target.path.substr(lobby_prefix.size());
    const auto slash = rest.find('/');
    const auto lobby_id = url_decode(slash == std::string::npos ? rest : rest.substr(0, slash));
    const auto action = slash == std::string::npos ? std::string{} : rest.substr(slash + 1);
    if( method == http::verb::get && action.empty() )
    {
      auto found = lobbies.get(actor.game_id, lobby_id);
      if( !found )
      {
        finish_error(found.error());
        return;
      }
      json payload = {
          {"lobby", *found}, {"server_ts", wall_ms()}, {"host_control_rtt_ms", nullptr}};
      if( has_query(target.query, "client_ts") )
      {
        const auto text = query_value(target.query, "client_ts");
        try
        {
          payload["client_ts"] = std::stoll(text);
        }
        catch( ... )
        {
          payload["client_ts"] = text;
        }
      }
      const auto host = lobbies.connected_host(lobby_id);
      if( !host )
      {
        respond(HttpOut{200, payload, true});
        return;
      }
      auto socket = sockets.find(*host);
      std::shared_ptr<WsSession> session;
      if( socket != sockets.end() )
      {
        session = socket->second.lock();
      }
      if( !session )
      {
        respond(HttpOut{200, payload, true});
        return;
      }
      session->probe(std::chrono::milliseconds(std::max(50, catalog.system().probe_timeout_ms)),
                     [respond, payload](std::optional<int> rtt) mutable
                     {
                       if( rtt )
                       {
                         payload["host_control_rtt_ms"] = *rtt;
                       }
                       respond(HttpOut{200, payload, true});
                     });
      return;
    }
    if( method == http::verb::post && action == "join" )
    {
      if( queue.has_open_ticket(actor.user_id) )
      {
        finish_error(Error{ErrorCode::AlreadyQueued, "already queued", std::nullopt});
        return;
      }
      std::optional<std::string> password;
      if( body.contains("password") && !body["password"].is_null() )
      {
        if( !body["password"].is_string() )
        {
          finish_error(Error{ErrorCode::BadRequest, "password must be a string", std::nullopt});
          return;
        }
        password = body["password"].get<std::string>();
      }
      auto joined = lobbies.begin_join(actor, lobby_id, password, now);
      if( !joined )
      {
        finish_error(joined.error());
        return;
      }
      respond(HttpOut{200, *joined, true});
      return;
    }
  }
  if( method == http::verb::post && target.path == "/api/v1/lobbies" )
  {
    if( queue.has_open_ticket(actor.user_id) )
    {
      finish_error(Error{ErrorCode::AlreadyQueued, "already queued", std::nullopt});
      return;
    }
    auto created = lobbies.create(actor, now);
    if( !created )
    {
      finish_error(created.error());
      return;
    }
    write_log(store.get(), LogLevel::Info, "lobby", "lobby created", actor.game_id);
    respond(HttpOut{200, *created, true});
    return;
  }
  constexpr std::string_view tickets = "/api/v1/matchmaking/tickets";
  if( target.path == tickets && method == http::verb::post )
  {
    if( lobbies.user_in_lobby(actor.user_id) )
    {
      finish_error(Error{ErrorCode::AlreadyInLobby, "already in a lobby", std::nullopt});
      return;
    }
    const auto* game = catalog.game(actor.game_id);
    if( !game )
    {
      finish_error(Error{ErrorCode::UnknownGame, "unknown game", std::nullopt});
      return;
    }
    auto ticket = queue.enqueue(actor, *game, body.value("mode", ""), now);
    if( !ticket )
    {
      finish_error(ticket.error());
      return;
    }
    if( ticket->value("status", "") == "match_ready" )
    {
      write_log(store.get(), LogLevel::Info, "queue", "match formed", actor.game_id);
    }
    respond(HttpOut{200, *ticket, true});
    return;
  }
  if( target.path.starts_with(std::string(tickets) + "/") &&
      target.path.size() > tickets.size() + 1 )
  {
    const auto ticket_id = url_decode(target.path.substr(tickets.size() + 1));
    if( method == http::verb::delete_ )
    {
      auto cancelled = queue.cancel(actor, ticket_id, lobbies, now);
      if( !cancelled )
      {
        finish_error(cancelled.error());
        return;
      }
      respond(HttpOut{200, {{"ticket_id", ticket_id}, {"status", "cancelled"}}, true});
      return;
    }
    if( method == http::verb::get )
    {
      auto current = queue.status(actor, ticket_id);
      if( !current )
      {
        finish_error(current.error());
        return;
      }
      int wait_ms = 0;
      if( has_query(target.query, "wait_ms") )
      {
        try
        {
          wait_ms = std::stoi(query_value(target.query, "wait_ms"));
        }
        catch( ... )
        {
          finish_error(Error{ErrorCode::BadRequest, "bad wait_ms", std::nullopt});
          return;
        }
      }
      wait_ms = std::clamp(wait_ms, 0, catalog.system().ticket_wait_cap_ms);
      if( wait_ms == 0 || current->value("status", "") != "queued" )
      {
        respond(HttpOut{200, *current, true});
        return;
      }
      auto done = std::make_shared<std::atomic<bool>>(false);
      auto timer = std::make_shared<boost::asio::steady_timer>(strand);
      auto complete = [this, respond, done, timer, actor, ticket_id]
      {
        if( done->exchange(true) )
        {
          return;
        }
        timer->cancel();
        auto latest = queue.status(actor, ticket_id);
        if( !latest )
        {
          respond(HttpOut{http_status(latest.error().code), error_body(latest.error()), true});
          return;
        }
        respond(HttpOut{200, *latest, true});
      };
      timer->expires_after(std::chrono::milliseconds(wait_ms));
      timer->async_wait(boost::asio::bind_executor(strand,
                                                   [complete](boost::system::error_code ec)
                                                   {
                                                     if( ec )
                                                     {
                                                       return;
                                                     }
                                                     complete();
                                                   }));
      queue.add_waiter(actor, ticket_id, complete);
      return;
    }
  }
  finish_error(Error{ErrorCode::NotFound, "not found", std::nullopt});
}

void App::State::handle_ws_message(const std::string& user_id, const std::string& lobby_id,
                                   const json& message,
                                   const std::function<void(const json&, bool)>& send)
{
  const auto it = users.find(user_id);
  if( it == users.end() )
  {
    send(frame_error(message, Error{ErrorCode::Unauthorized, "unknown session", std::nullopt}),
         true);
    return;
  }
  auto actor = actor_of(it->second);
  const auto op = message.value("op", "");
  const auto now = Clock::now();
  auto ok = [&]
  {
    send(frame_ok(message), false);
  };
  auto fail = [&](const Error& error)
  {
    send(frame_error(message, error), false);
  };

  if( op == "heartbeat" )
  {
    auto beat = lobbies.heartbeat(actor, lobby_id, now);
    if( !beat )
    {
      fail(beat.error());
      return;
    }
    json ack = {{"op", "heartbeat_ack"}, {"server_ts", wall_ms()}};
    if( message.contains("id") )
    {
      ack["id"] = message["id"];
    }
    send(ack, false);
    return;
  }
  if( op == "leave" )
  {
    lobbies.leave(user_id, now);
    send(frame_ok(message), true);
    return;
  }
  if( op == "close_lobby" )
  {
    if( lobbies.connected_host(lobby_id) != actor.user_id )
    {
      fail(Error{ErrorCode::NotHost, "host only", std::nullopt});
      return;
    }
    send(frame_ok(message), false);
    auto closed = lobbies.close_lobby(actor, now);
    if( closed )
    {
      write_log(store.get(), LogLevel::Info, "lobby", "lobby closed", actor.game_id);
    }
    return;
  }
  if( op == "set_phase" )
  {
    auto status = lobbies.set_phase(actor, lobby_id, message.value("phase", ""));
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_slots" )
  {
    if( !message.contains("slots") || !message["slots"].is_number_integer() )
    {
      fail(Error{ErrorCode::BadRequest, "slots must be an integer", std::nullopt});
      return;
    }
    auto status = lobbies.set_slots(actor, lobby_id, message["slots"].get<int>());
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_late_join" )
  {
    auto status = lobbies.set_late_join(actor, lobby_id, message.value("allowed", false));
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_password" )
  {
    std::optional<std::string> password;
    if( message.contains("password") && !message["password"].is_null() )
    {
      if( !message["password"].is_string() )
      {
        fail(Error{ErrorCode::BadRequest, "password must be a string", std::nullopt});
        return;
      }
      password = message["password"].get<std::string>();
    }
    auto status = lobbies.set_password(actor, lobby_id, password);
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_short_code" )
  {
    std::optional<std::string> code;
    if( message.contains("code") && !message["code"].is_null() )
    {
      if( !message["code"].is_string() )
      {
        fail(Error{ErrorCode::BadRequest, "code must be a string", std::nullopt});
        return;
      }
      code = message["code"].get<std::string>();
    }
    auto status = lobbies.set_short_code(actor, lobby_id, code);
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_metadata" )
  {
    auto status = lobbies.set_metadata(actor, lobby_id, message.value("metadata", json::object()));
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "set_alias" )
  {
    const auto alias = message.value("alias", "");
    auto status = lobbies.set_alias(actor, lobby_id, alias);
    if( !status )
    {
      fail(status.error());
      return;
    }
    it->second.alias = alias;
    ok();
    return;
  }
  if( op == "set_ready" )
  {
    auto status = lobbies.set_ready(actor, lobby_id, message.value("ready", false));
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "chat" )
  {
    auto status = lobbies.chat(actor, lobby_id, message.value("text", ""));
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "custom" )
  {
    std::optional<std::string> target;
    if( message.contains("target_user_id") && !message["target_user_id"].is_null() )
    {
      if( !message["target_user_id"].is_string() )
      {
        fail(Error{ErrorCode::BadRequest, "target_user_id must be a string", std::nullopt});
        return;
      }
      target = message["target_user_id"].get<std::string>();
    }
    auto status = lobbies.custom(actor, lobby_id, message.value("name", ""),
                                 message.value("data", json::object()), target);
    status ? ok() : fail(status.error());
    return;
  }
  if( op == "release_relays" )
  {
    if( lobbies.connected_host(lobby_id) != actor.user_id )
    {
      fail(Error{ErrorCode::NotHost, "host only", std::nullopt});
      return;
    }
    std::vector<std::string> ids;
    if( message.contains("ids") )
    {
      if( !message["ids"].is_array() )
      {
        fail(Error{ErrorCode::BadRequest, "ids must be an array", std::nullopt});
        return;
      }
      for( const auto& id : message["ids"] )
      {
        if( id.is_string() )
        {
          ids.push_back(id.get<std::string>());
        }
      }
    }
    else
    {
      for( const auto& [id, meta] : allocs )
      {
        if( meta.lobby_id == lobby_id )
        {
          ids.push_back(id);
        }
      }
    }
    for( const auto& id : ids )
    {
      const auto meta = allocs.find(id);
      if( meta == allocs.end() || meta->second.lobby_id != lobby_id )
      {
        continue;
      }
      relay->release(id, now);
    }
    write_log(store.get(), LogLevel::Info, "relay", "relays released", actor.game_id);
    ok();
    return;
  }
  if( op == "allocate_relays" )
  {
    if( lobbies.connected_host(lobby_id) != actor.user_id )
    {
      fail(Error{ErrorCode::NotHost, "host only", std::nullopt});
      return;
    }
    const auto* game = catalog.game(actor.game_id);
    if( !game )
    {
      fail(Error{ErrorCode::UnknownGame, "unknown game", std::nullopt});
      return;
    }
    if( !message.contains("channels") || !message["channels"].is_array() )
    {
      fail(Error{ErrorCode::BadRequest, "channels must be an array", std::nullopt});
      return;
    }
    const auto& requested = message["channels"];
    const int game_count = game_channels[game->id];
    const int lobby_count = lobby_channels[lobby_id];
    if( game_count + static_cast<int>(requested.size()) > game->limits.max_concurrent_channels ||
        lobby_count + static_cast<int>(requested.size()) > game->limits.max_channels_per_lobby )
    {
      fail(Error{ErrorCode::Limit, "channel limit", std::nullopt});
      return;
    }
    json allocations = json::array();
    std::vector<std::string> created;
    for( const auto& channel : requested )
    {
      if( !channel.is_object() )
      {
        fail(Error{ErrorCode::BadRequest, "channel must be an object", std::nullopt});
        for( const auto& id : created )
        {
          relay->release(id, now);
        }
        return;
      }
      const auto user_a = channel.value("a", "");
      const auto user_b = channel.value("b", "");
      const auto protocol = channel.value("protocol", "udp");
      const int lifetime = channel.value("lifetime_sec", 0);
      if( !lobbies.connected_member(lobby_id, user_a) ||
          !lobbies.connected_member(lobby_id, user_b) )
      {
        fail(
            Error{ErrorCode::UnknownUser, "channel users must be connected members", std::nullopt});
        for( const auto& id : created )
        {
          relay->release(id, now);
        }
        return;
      }
      if( protocol == "tcp" && !game->limits.allow_tcp )
      {
        fail(Error{ErrorCode::Limit, "tcp relay is disabled", std::nullopt});
        for( const auto& id : created )
        {
          relay->release(id, now);
        }
        return;
      }
      auto allowed = check_lifetime(lifetime, game->limits.max_channel_lifetime_sec,
                                    catalog.system().max_channel_lifetime_sec);
      if( !allowed )
      {
        fail(allowed.error());
        for( const auto& id : created )
        {
          relay->release(id, now);
        }
        return;
      }
      ChannelRequest spec;
      spec.lobby_id = lobby_id;
      spec.game_id = game->id;
      spec.user_a = user_a;
      spec.user_b = user_b;
      spec.protocol = protocol;
      spec.lifetime_sec = *allowed;
      spec.idle_sec = std::min(game->limits.max_idle_sec, catalog.system().max_idle_sec);
      spec.max_packet = catalog.system().max_udp_packet_bytes;
      spec.bytes_per_sec = catalog.system().max_bytes_per_sec_per_channel;
      spec.public_host = catalog.system().public_host;
      auto info = relay->allocate(spec, now);
      if( !info )
      {
        fail(info.error());
        for( const auto& id : created )
        {
          relay->release(id, now);
        }
        return;
      }
      created.push_back(info->id);
      allocs[info->id] = AllocMeta{lobby_id, game->id};
      ++game_channels[game->id];
      ++lobby_channels[lobby_id];
      allocations.push_back({{"id", info->id},
                             {"expires_at_ms", info->expires_at_ms},
                             {"protocol", info->protocol},
                             {"a", {{"user_id", info->a.user_id}, {"port", info->a.port}}},
                             {"b", {{"user_id", info->b.user_id}, {"port", info->b.port}}}});
      auto event_for = [&](const SideInfo& self, const SideInfo& peer)
      {
        return json{{"op", "event"},
                    {"event", "relay_allocated"},
                    {"lobby_id", lobby_id},
                    {"channel_id", info->id},
                    {"protocol", info->protocol},
                    {"peer_user_id", peer.user_id},
                    {"host", self.host},
                    {"port", self.port},
                    {"token", self.token_hex},
                    {"expires_at_ms", info->expires_at_ms}};
      };
      send_to(info->a.user_id, event_for(info->a, info->b), false);
      send_to(info->b.user_id, event_for(info->b, info->a), false);
    }
    write_log(store.get(), LogLevel::Info, "relay", "relays allocated", actor.game_id);
    json response = frame_ok(message);
    response["allocations"] = allocations;
    send(response, false);
    return;
  }
  fail(Error{ErrorCode::BadRequest, "unknown op", std::nullopt});
}

namespace
{

tcp::endpoint endpoint_from(const std::string& text)
{
  const auto parsed = parse_host_port(text);
  if( !parsed )
  {
    throw std::runtime_error("invalid address: " + text);
  }
  return {boost::asio::ip::make_address(parsed->first), parsed->second};
}

} // namespace

App::App(std::string database_path) : state_(std::make_unique<State>(std::move(database_path)))
{
}

App::~App()
{
  stop();
}

void App::start()
{
  state_->store = open_sqlite_store(state_->database_path);
  state_->store->migrate();
  state_->load_catalog();
  const auto& cfg = state_->catalog.system();
  if( cfg.tls_cert.empty() || cfg.tls_key.empty() )
  {
    throw std::runtime_error("tls_cert and tls_key must be set in the database");
  }
  if( cfg.jwt_secret.empty() )
  {
    throw std::runtime_error("jwt_secret must be set in the database");
  }
  state_->ssl_ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 |
                              ssl::context::no_sslv3 | ssl::context::single_dh_use);
  state_->ssl_ctx.use_certificate_chain_file(cfg.tls_cert);
  state_->ssl_ctx.use_private_key_file(cfg.tls_key, ssl::context::pem);
  const auto min_version = cfg.tls_min_version == "1.3" ? TLS1_3_VERSION : TLS1_2_VERSION;
  if( SSL_CTX_set_min_proto_version(state_->ssl_ctx.native_handle(), min_version) != 1 )
  {
    throw std::runtime_error("failed to set the minimum TLS version");
  }
  state_->apply_limits();
  state_->wire();
  const auto https = endpoint_from(cfg.listen);
  state_->https_acceptor.open(https.protocol());
  state_->https_acceptor.set_option(tcp::acceptor::reuse_address(true));
  state_->https_acceptor.bind(https);
  state_->https_acceptor.listen();
  state_->https_port_ = state_->https_acceptor.local_endpoint().port();
  const auto stats = endpoint_from(cfg.stats_listen);
  state_->stats_acceptor.open(stats.protocol());
  state_->stats_acceptor.set_option(tcp::acceptor::reuse_address(true));
  state_->stats_acceptor.bind(stats);
  state_->stats_acceptor.listen();
  state_->stats_port_ = state_->stats_acceptor.local_endpoint().port();
  const auto cutoff =
      wall_ms() - static_cast<std::int64_t>(cfg.retention_days) * 24 * 60 * 60 * 1000;
  state_->store->prune_logs(cutoff);
  write_log(state_->store.get(), LogLevel::Info, "server",
            "listening on " + std::to_string(state_->https_port_) + " stats " +
                std::to_string(state_->stats_port_));
  state_->do_https_accept();
  state_->do_stats_accept();
  state_->schedule_tick();
  int threads =
      cfg.threads <= 0 ? static_cast<int>(std::thread::hardware_concurrency()) : cfg.threads;
  if( threads < 1 )
  {
    threads = 1;
  }
  for( int i = 0; i < threads; ++i )
  {
    state_->threads.emplace_back(
        [this]
        {
          state_->ioc.run();
        });
  }
}

void App::install_signals()
{
  state_->signals = std::make_unique<boost::asio::signal_set>(state_->ioc, SIGINT, SIGTERM);
#ifdef SIGBREAK
  state_->signals->add(SIGBREAK);
#endif
#ifdef SIGHUP
  state_->signals->add(SIGHUP);
#endif
  state_->arm_signals();
}

void App::stop()
{
  if( !state_ || state_->joined )
  {
    return;
  }
  state_->stopping = true;
  state_->work.reset();
  state_->ioc.stop();
  wait();
}

void App::wait()
{
  if( !state_ || state_->joined )
  {
    return;
  }
  for( auto& thread : state_->threads )
  {
    if( thread.joinable() )
    {
      thread.join();
    }
  }
  state_->joined = true;
}

std::uint16_t App::https_port() const
{
  return state_->https_port_;
}

std::uint16_t App::stats_port() const
{
  return state_->stats_port_;
}

} // namespace matchmaker
