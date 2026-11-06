#include "relay.hpp"

#include "crypto.hpp"
#include "util.hpp"

#include <boost/asio/strand.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

namespace matchmaker
{
namespace
{

std::int64_t to_ns(RelayService::Time time)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

RelayService::Time from_ns(std::int64_t ns)
{
  return RelayService::Time(
      std::chrono::duration_cast<RelayService::Time::duration>(std::chrono::nanoseconds(ns)));
}

} // namespace

struct RelayService::Channel
{
  struct UdpSide
  {
    explicit UdpSide(boost::asio::io_context& ioc) : socket(ioc)
    {
    }
    boost::asio::ip::udp::socket socket;
    std::array<std::uint8_t, 16> token{};
    std::optional<boost::asio::ip::udp::endpoint> peer;
    struct Datagram
    {
      boost::asio::ip::udp::endpoint endpoint;
      std::vector<std::uint8_t> bytes;
    };
    std::deque<Datagram> out;
    bool writing = false;
  };

  struct TcpSide : std::enable_shared_from_this<TcpSide>
  {
    explicit TcpSide(boost::asio::io_context& ioc) : acceptor(ioc), socket(ioc)
    {
    }
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket socket;
    std::array<std::uint8_t, 16> token{};
    std::array<std::uint8_t, 16> inbound{};
    std::array<std::uint8_t, 8192> buf{};
    bool authed = false;
    bool accepted = false;
    bool reading = false;
    std::vector<std::uint8_t> pending;
    std::deque<std::vector<std::uint8_t>> out;
    bool writing = false;
    std::shared_ptr<TcpSide> peer;
    std::function<void()> fail;
    std::function<void()> activity;
    std::optional<Strand> io_strand;

    void listen()
    {
      acceptor.async_accept(boost::asio::bind_executor(
          *io_strand,
          [self = shared_from_this()](boost::system::error_code ec,
                                      boost::asio::ip::tcp::socket sock)
          {
            if( ec )
            {
              return;
            }
            if( self->accepted )
            {
              sock.close(ec);
              self->listen();
              return;
            }
            self->accepted = true;
            self->socket = std::move(sock);
            boost::asio::async_read(
                self->socket, boost::asio::buffer(self->inbound),
                boost::asio::bind_executor(*self->io_strand,
                                           [self](boost::system::error_code read_ec, std::size_t n)
                                           {
                                             if( read_ec || n != self->token.size() ||
                                                 std::memcmp(self->inbound.data(),
                                                             self->token.data(), n) != 0 )
                                             {
                                               boost::system::error_code ignore;
                                               self->socket.close(ignore);
                                               self->accepted = false;
                                               self->listen();
                                               return;
                                             }
                                             self->authed = true;
                                             if( self->activity )
                                             {
                                               self->activity();
                                             }
                                             self->flush_pending();
                                             if( self->peer )
                                             {
                                               self->peer->flush_pending();
                                             }
                                             self->do_read();
                                           }));
          }));
    }

    void flush_pending()
    {
      if( !authed || pending.empty() )
      {
        return;
      }
      out.push_back(std::move(pending));
      pending.clear();
      pump();
    }

    void do_read()
    {
      if( reading || !authed )
      {
        return;
      }
      reading = true;
      socket.async_read_some(
          boost::asio::buffer(buf),
          boost::asio::bind_executor(
              *io_strand,
              [self = shared_from_this()](boost::system::error_code ec, std::size_t n)
              {
                self->reading = false;
                if( ec )
                {
                  if( self->fail )
                  {
                    self->fail();
                  }
                  return;
                }
                if( self->activity )
                {
                  self->activity();
                }
                std::vector<std::uint8_t> bytes(self->buf.begin(),
                                                self->buf.begin() + static_cast<std::ptrdiff_t>(n));
                if( !self->peer || !self->peer->authed )
                {
                  if( self->pending.size() + bytes.size() > 65536 )
                  {
                    if( self->fail )
                    {
                      self->fail();
                    }
                    return;
                  }
                  self->pending.insert(self->pending.end(), bytes.begin(), bytes.end());
                  self->do_read();
                  return;
                }
                self->peer->out.push_back(std::move(bytes));
                self->peer->pump();
                self->do_read();
              }));
    }

    void pump()
    {
      if( writing || out.empty() || !authed )
      {
        return;
      }
      writing = true;
      auto self = shared_from_this();
      boost::asio::async_write(
          socket, boost::asio::buffer(out.front()),
          boost::asio::bind_executor(*io_strand,
                                     [self](boost::system::error_code ec, std::size_t)
                                     {
                                       self->writing = false;
                                       if( !self->out.empty() )
                                       {
                                         self->out.pop_front();
                                       }
                                       if( ec )
                                       {
                                         if( self->fail )
                                         {
                                           self->fail();
                                         }
                                         return;
                                       }
                                       self->pump();
                                       if( self->peer )
                                       {
                                         self->peer->do_read();
                                       }
                                     }));
    }
  };

  std::string id;
  std::string lobby_id;
  std::string user_a;
  std::string user_b;
  std::string protocol;
  Time expires{};
  std::atomic<std::int64_t> last_ns{0};
  int idle_sec = 60;
  int max_packet = 1200;
  int bytes_per_sec = 0;
  double bucket = 0;
  Time bucket_at{};
  int port_a = 0;
  int port_b = 0;
  std::optional<Strand> io_strand;
  std::atomic<bool> closing{false};
  std::unique_ptr<UdpSide> udp[2];
  std::shared_ptr<TcpSide> tcp[2];
};

Result<int> check_lifetime(int requested, int game_max, int system_max)
{
  if( requested < 1 )
  {
    return Err(ErrorCode::BadRequest, "lifetime must be positive");
  }
  const int cap = std::min(game_max, system_max);
  if( requested > cap )
  {
    return Err(ErrorCode::LifetimeExceedsLimit, "lifetime exceeds the configured maximum", cap);
  }
  return requested;
}

RelayService::RelayService(boost::asio::io_context& ioc, Strand strand)
    : ioc_(ioc), strand_(std::move(strand))
{
}

void RelayService::configure(RelayConfig config)
{
  config_ = std::move(config);
}

void RelayService::set_on_closed(std::function<void(const RelayClosed&)> fn)
{
  on_closed_ = std::move(fn);
}

int RelayService::channels() const
{
  return static_cast<int>(channels_.size());
}

std::uint64_t RelayService::forwarded_bytes() const
{
  return forwarded_.load();
}

std::uint64_t RelayService::drops() const
{
  return drops_.load();
}

bool RelayService::take_port(int port)
{
  if( used_ports_.contains(port) )
  {
    return false;
  }
  used_ports_[port] = true;
  return true;
}

void RelayService::free_ports(const Channel& channel)
{
  used_ports_.erase(channel.port_a);
  used_ports_.erase(channel.port_b);
}

void RelayService::finalize_close(const std::string& id, const std::string& reason)
{
  const auto it = channels_.find(id);
  if( it == channels_.end() )
  {
    return;
  }
  RelayClosed closed;
  closed.id = it->second->id;
  closed.lobby_id = it->second->lobby_id;
  closed.user_a = it->second->user_a;
  closed.user_b = it->second->user_b;
  closed.reason = reason;
  free_ports(*it->second);
  channels_.erase(it);
  if( on_closed_ )
  {
    on_closed_(closed);
  }
}

void RelayService::request_close(const std::shared_ptr<Channel>& channel, std::string reason)
{
  boost::asio::dispatch(*channel->io_strand,
                        [this, channel, reason]
                        {
                          if( channel->closing.exchange(true) )
                          {
                            return;
                          }
                          boost::system::error_code ec;
                          for( auto& side : channel->udp )
                          {
                            if( side )
                            {
                              side->socket.close(ec);
                            }
                          }
                          for( auto& side : channel->tcp )
                          {
                            if( side )
                            {
                              side->acceptor.close(ec);
                              side->socket.close(ec);
                            }
                          }
                          boost::asio::dispatch(strand_,
                                                [this, id = channel->id, reason]
                                                {
                                                  finalize_close(id, reason);
                                                });
                        });
}

Result<ChannelInfo> RelayService::allocate(const ChannelRequest& request, Time now)
{
  if( request.protocol != "udp" && request.protocol != "tcp" )
  {
    return Err(ErrorCode::BadRequest, "protocol must be udp or tcp");
  }
  if( request.protocol == "tcp" && !config_.tcp_enabled )
  {
    return Err(ErrorCode::Limit, "tcp relay is disabled");
  }
  if( static_cast<int>(channels_.size()) >= config_.max_concurrent )
  {
    return Err(ErrorCode::Limit, "relay channel limit");
  }
  if( request.user_a.empty() || request.user_b.empty() || request.user_a == request.user_b )
  {
    return Err(ErrorCode::BadRequest, "a channel needs two distinct users");
  }
  auto grab = [&]() -> std::optional<int>
  {
    if( config_.port_start > config_.port_end )
    {
      return std::nullopt;
    }
    for( int port = config_.port_start; port <= config_.port_end; ++port )
    {
      if( take_port(port) )
      {
        return port;
      }
    }
    return std::nullopt;
  };
  const auto port_a = grab();
  if( !port_a )
  {
    return Err(ErrorCode::Limit, "no relay ports available");
  }
  const auto port_b = grab();
  if( !port_b )
  {
    used_ports_.erase(*port_a);
    return Err(ErrorCode::Limit, "no relay ports available");
  }
  auto release_ports = [&]
  {
    used_ports_.erase(*port_a);
    used_ports_.erase(*port_b);
  };
  boost::asio::ip::address address;
  try
  {
    address = boost::asio::ip::make_address(config_.bind_host);
  }
  catch( ... )
  {
    release_ports();
    return Err(ErrorCode::Internal, "relay bind address is invalid");
  }

  auto channel = std::make_shared<Channel>();
  channel->id = uuid_v4();
  channel->lobby_id = request.lobby_id;
  channel->user_a = request.user_a;
  channel->user_b = request.user_b;
  channel->protocol = request.protocol;
  channel->expires = now + std::chrono::seconds(request.lifetime_sec);
  channel->last_ns.store(to_ns(now));
  channel->idle_sec = request.idle_sec;
  channel->max_packet = request.max_packet > 0 ? request.max_packet : config_.max_packet;
  channel->bytes_per_sec = request.bytes_per_sec;
  channel->bucket = request.bytes_per_sec > 0 ? request.bytes_per_sec : 0;
  channel->bucket_at = now;
  channel->port_a = *port_a;
  channel->port_b = *port_b;
  channel->io_strand = boost::asio::make_strand(ioc_);

  auto make_token = []
  {
    std::array<std::uint8_t, 16> token{};
    random_bytes(token);
    return token;
  };
  const auto token_a = make_token();
  const auto token_b = make_token();

  try
  {
    if( request.protocol == "udp" )
    {
      for( int side = 0; side < 2; ++side )
      {
        channel->udp[side] = std::make_unique<Channel::UdpSide>(ioc_);
        channel->udp[side]->token = side == 0 ? token_a : token_b;
        const int port = side == 0 ? *port_a : *port_b;
        channel->udp[side]->socket.open(address.is_v6() ? boost::asio::ip::udp::v6()
                                                        : boost::asio::ip::udp::v4());
        channel->udp[side]->socket.bind(
            boost::asio::ip::udp::endpoint(address, static_cast<unsigned short>(port)));
      }
    }
    else
    {
      for( int side = 0; side < 2; ++side )
      {
        auto tcp = std::make_shared<Channel::TcpSide>(ioc_);
        tcp->token = side == 0 ? token_a : token_b;
        tcp->io_strand = channel->io_strand;
        const int port = side == 0 ? *port_a : *port_b;
        boost::asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(port));
        tcp->acceptor.open(endpoint.protocol());
        tcp->acceptor.bind(endpoint);
        tcp->acceptor.listen();
        channel->tcp[side] = tcp;
      }
      channel->tcp[0]->peer = channel->tcp[1];
      channel->tcp[1]->peer = channel->tcp[0];
      for( auto& tcp : channel->tcp )
      {
        tcp->activity = [channel]
        {
          channel->last_ns.store(to_ns(RelayService::Clock::now()));
        };
        tcp->fail = [this, channel]
        {
          request_close(channel, "closed");
        };
      }
    }
  }
  catch( ... )
  {
    release_ports();
    return Err(ErrorCode::Limit, "failed to bind relay port");
  }

  ChannelInfo info;
  info.id = channel->id;
  info.lobby_id = channel->lobby_id;
  info.protocol = channel->protocol;
  info.expires_at_ms = wall_ms() + static_cast<std::int64_t>(request.lifetime_sec) * 1000;
  info.a = SideInfo{request.user_a, request.public_host, *port_a,
                    hex_encode(token_a.data(), token_a.size())};
  info.b = SideInfo{request.user_b, request.public_host, *port_b,
                    hex_encode(token_b.data(), token_b.size())};
  channels_.emplace(channel->id, channel);

  if( request.protocol == "udp" )
  {
    boost::asio::dispatch(*channel->io_strand,
                          [this, channel]
                          {
                            if( channel->closing.load() )
                            {
                              return;
                            }
                            arm_udp(channel, 0);
                            arm_udp(channel, 1);
                          });
  }
  else
  {
    boost::asio::dispatch(*channel->io_strand,
                          [channel]
                          {
                            if( channel->closing.load() )
                            {
                              return;
                            }
                            channel->tcp[0]->listen();
                            channel->tcp[1]->listen();
                          });
  }
  return info;
}

void RelayService::arm_udp(const std::shared_ptr<Channel>& channel, int side)
{
  if( channel->closing.load() || !channel->udp[side] )
  {
    return;
  }
  auto buffer = std::make_shared<std::vector<std::uint8_t>>(65535);
  auto from = std::make_shared<boost::asio::ip::udp::endpoint>();
  channel->udp[side]->socket.async_receive_from(
      boost::asio::buffer(*buffer), *from,
      boost::asio::bind_executor(
          *channel->io_strand,
          [this, channel, side, buffer, from](boost::system::error_code ec, std::size_t n)
          {
            if( ec || channel->closing.load() )
            {
              return;
            }
            on_udp(channel, side, *from, buffer->data(), n, Clock::now());
            if( !channel->closing.load() )
            {
              arm_udp(channel, side);
            }
          }));
}

void RelayService::on_udp(const std::shared_ptr<Channel>& channel, int side,
                          const boost::asio::ip::udp::endpoint& from, const std::uint8_t* data,
                          std::size_t n, Time now)
{
  auto& local = *channel->udp[side];
  auto& other = *channel->udp[side == 0 ? 1 : 0];
  if( n == local.token.size() && std::memcmp(data, local.token.data(), local.token.size()) == 0 )
  {
    local.peer = from;
    channel->last_ns.store(to_ns(now));
    return;
  }
  if( !local.peer || from != *local.peer )
  {
    drops_.fetch_add(1);
    return;
  }
  if( static_cast<int>(n) > channel->max_packet )
  {
    drops_.fetch_add(1);
    return;
  }
  if( channel->bytes_per_sec > 0 )
  {
    const double elapsed = std::chrono::duration<double>(now - channel->bucket_at).count();
    channel->bucket = std::min<double>(channel->bytes_per_sec,
                                       channel->bucket + elapsed * channel->bytes_per_sec);
    channel->bucket_at = now;
    if( channel->bucket < static_cast<double>(n) )
    {
      drops_.fetch_add(1);
      return;
    }
    channel->bucket -= static_cast<double>(n);
  }
  if( !other.peer )
  {
    drops_.fetch_add(1);
    return;
  }
  channel->last_ns.store(to_ns(now));
  forwarded_.fetch_add(n);
  Channel::UdpSide::Datagram datagram;
  datagram.endpoint = *other.peer;
  datagram.bytes.assign(data, data + n);
  other.out.push_back(std::move(datagram));
  if( other.writing )
  {
    return;
  }
  auto pump = std::make_shared<std::function<void()>>();
  *pump = [this, channel, side_other = side == 0 ? 1 : 0, pump]
  {
    auto& dest = *channel->udp[side_other];
    if( dest.out.empty() || channel->closing.load() )
    {
      dest.writing = false;
      return;
    }
    dest.writing = true;
    auto& front = dest.out.front();
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::move(front.bytes));
    auto endpoint = front.endpoint;
    dest.out.pop_front();
    dest.socket.async_send_to(
        boost::asio::buffer(*bytes), endpoint,
        boost::asio::bind_executor(*channel->io_strand,
                                   [channel, bytes, pump](boost::system::error_code, std::size_t)
                                   {
                                     (*pump)();
                                   }));
  };
  (*pump)();
}

Status RelayService::release(const std::string& id, Time)
{
  const auto it = channels_.find(id);
  if( it == channels_.end() )
  {
    return Err(ErrorCode::NotFound, "relay not found");
  }
  request_close(it->second, "released");
  return true;
}

void RelayService::release_lobby(const std::string& lobby_id, Time)
{
  std::vector<std::shared_ptr<Channel>> found;
  for( const auto& [_, channel] : channels_ )
  {
    if( channel->lobby_id == lobby_id )
    {
      found.push_back(channel);
    }
  }
  for( const auto& channel : found )
  {
    request_close(channel, "released");
  }
}

void RelayService::tick(Time now)
{
  std::vector<std::pair<std::shared_ptr<Channel>, std::string>> due;
  for( const auto& [_, channel] : channels_ )
  {
    if( now >= channel->expires )
    {
      due.emplace_back(channel, "expired");
      continue;
    }
    const auto idle_for = now - from_ns(channel->last_ns.load());
    if( idle_for > std::chrono::seconds(channel->idle_sec) )
    {
      due.emplace_back(channel, "idle");
    }
  }
  for( const auto& [channel, reason] : due )
  {
    request_close(channel, reason);
  }
}

} // namespace matchmaker
