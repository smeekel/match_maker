#pragma once

#include "result.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace matchmaker
{

struct RelayConfig
{
  std::string bind_host = "0.0.0.0";
  int port_start = 30000;
  int port_end = 39999;
  int max_concurrent = 4096;
  int max_packet = 1200;
  bool tcp_enabled = false;
};

struct ChannelRequest
{
  std::string lobby_id;
  std::string game_id;
  std::string user_a;
  std::string user_b;
  std::string protocol = "udp";
  int lifetime_sec = 60;
  int idle_sec = 60;
  int max_packet = 1200;
  int bytes_per_sec = 0;
  std::string public_host = "127.0.0.1";
};

struct SideInfo
{
  std::string user_id;
  std::string host;
  int port = 0;
  std::string token_hex;
};

struct ChannelInfo
{
  std::string id;
  std::string lobby_id;
  std::string protocol;
  std::int64_t expires_at_ms = 0;
  SideInfo a;
  SideInfo b;
};

struct RelayClosed
{
  std::string id;
  std::string lobby_id;
  std::string user_a;
  std::string user_b;
  std::string reason;
};

class RelayService
{
public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;

  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  RelayService(boost::asio::io_context& ioc, Strand strand);
  Strand strand() const
  {
    return strand_;
  }
  void configure(RelayConfig config);
  Result<ChannelInfo> allocate(const ChannelRequest& request, Time now);
  Status release(const std::string& id, Time now);
  void release_lobby(const std::string& lobby_id, Time now);
  void tick(Time now);
  void set_on_closed(std::function<void(const RelayClosed&)> fn);
  int channels() const;
  std::uint64_t forwarded_bytes() const;
  std::uint64_t drops() const;

private:
  struct Channel;
  void arm_udp(const std::shared_ptr<Channel>& channel, int side);
  void on_udp(const std::shared_ptr<Channel>& channel, int side,
              const boost::asio::ip::udp::endpoint& from, const std::uint8_t* data, std::size_t n,
              Time now);
  void begin_tcp(const std::shared_ptr<Channel>& channel, int side);
  void finalize_close(const std::string& id, const std::string& reason);
  void request_close(const std::shared_ptr<Channel>& channel, std::string reason);
  bool take_port(int port);
  void free_ports(const Channel& channel);

  boost::asio::io_context& ioc_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  RelayConfig config_;
  std::function<void(const RelayClosed&)> on_closed_;
  std::unordered_map<std::string, std::shared_ptr<Channel>> channels_;
  std::unordered_map<int, bool> used_ports_;
  std::atomic<std::uint64_t> forwarded_{0};
  std::atomic<std::uint64_t> drops_{0};
};

Result<int> check_lifetime(int requested, int game_max, int system_max);

} // namespace matchmaker
