#pragma once

#include "error.hpp"
#include "link.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace matchmaker
{

class WtConnection;

// One WebTransport lobby session. The server opens the control stream as soon
// as the CONNECT is accepted; messages sent before that are queued.
class WtSession : public LobbyLink, public std::enable_shared_from_this<WtSession>
{
public:
  enum class Ended
  {
    // The client closed the session, the connection was lost, or the session
    // broke a limit. The user leaves the lobby.
    Peer,
    // The server closed the session after a send with then_close.
    Server
  };

  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;
  using OnEnded = std::function<void(const std::shared_ptr<WtSession>&, Ended)>;

  WtSession(Strand strand, std::weak_ptr<WtConnection> connection, std::size_t max_outbound,
            OnEnded on_ended);

  void send(const nlohmann::json& message, bool then_close) override;
  void probe(std::chrono::milliseconds timeout,
             std::function<void(std::optional<int>)> cb) override;
  std::string_view transport() const override
  {
    return "webtransport";
  }
  // |datagram| is the 16-byte channel id and the payload.
  void send_datagram(std::span<const std::uint8_t> datagram) override;

  // Returns true when |message| answered a probe and was consumed.
  bool take_pong(const nlohmann::json& message);
  void end(Ended how);
  bool ended() const
  {
    return ended_;
  }

  std::string user_id;
  std::string lobby_id;

private:
  void finish_probe(std::optional<int> rtt);

  Strand strand_;
  std::weak_ptr<WtConnection> connection_;
  std::size_t max_outbound_;
  OnEnded on_ended_;
  bool ended_ = false;
  bool probing_ = false;
  std::string probe_id_;
  std::optional<int> last_rtt_;
  std::chrono::steady_clock::time_point probe_started_{};
  std::function<void(std::optional<int>)> probe_cb_;
  std::optional<boost::asio::steady_timer> probe_timer_;
};

struct WtRequest
{
  std::string path;
  std::string query;
  std::string origin;
};

struct WtHandlers
{
  // Reserves a slot under max_connections for a new QUIC connection.
  std::function<bool()> admit;
  std::function<void()> release;
  // Returns the error that refuses the CONNECT. On success the handler has
  // set user_id and lobby_id and may already have sent on the session.
  std::function<std::optional<Error>(const WtRequest&, const std::shared_ptr<WtSession>&)> connect;
  // A control message that is not valid JSON arrives as a discarded value.
  std::function<void(const std::shared_ptr<WtSession>&, const nlohmann::json&)> message;
  std::function<void(const std::shared_ptr<WtSession>&, std::span<const std::uint8_t>)> datagram;
  // Called once for every accepted session.
  std::function<void(const std::shared_ptr<WtSession>&, WtSession::Ended)> ended;
};

struct WtConfig
{
  std::string listen = "0.0.0.0:443";
  std::string cert_file;
  std::string key_file;
  std::chrono::seconds idle_timeout{30};
  std::size_t max_message_bytes = 65536;
  std::size_t max_outbound_bytes = 1 << 20;
};

// Prepares the ngtcp2 OpenSSL backend once per process.
void wt_crypto_init();

// HTTP/3 listener that accepts WebTransport CONNECT requests. Every handler
// and every session method runs on the strand given to the constructor.
class WtServer
{
public:
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  WtServer(boost::asio::io_context& ioc, Strand strand, WtHandlers handlers);
  ~WtServer();
  WtServer(const WtServer&) = delete;
  WtServer& operator=(const WtServer&) = delete;

  // Binds the UDP socket and starts receiving. Throws when the bind fails.
  void start(const WtConfig& config);
  void stop();
  std::uint16_t port() const;

  struct Impl;

private:
  std::shared_ptr<Impl> impl_;
};

} // namespace matchmaker
