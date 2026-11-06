#include "relay.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <boost/asio.hpp>

#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <type_traits>

using namespace matchmaker;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;

namespace
{

struct RelayHarness
{
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
  RelayService relay;
  std::thread thread;

  RelayHarness()
      : work(ioc.get_executor()), relay(ioc, boost::asio::make_strand(ioc)), thread(
                                                                                 [this]
                                                                                 {
                                                                                   ioc.run();
                                                                                 })
  {
  }

  ~RelayHarness()
  {
    work.reset();
    ioc.stop();
    if( thread.joinable() )
    {
      thread.join();
    }
  }

  template <class F> auto on_strand(F&& fn) -> std::invoke_result_t<F>
  {
    using R = std::invoke_result_t<F>;
    std::promise<R> promise;
    auto future = promise.get_future();
    boost::asio::dispatch(relay.strand(),
                          [&]
                          {
                            if constexpr( std::is_void_v<R> )
                            {
                              fn();
                              promise.set_value();
                            }
                            else
                            {
                              promise.set_value(fn());
                            }
                          });
    return future.get();
  }
};

void set_recv_timeout(boost::asio::ip::udp::socket& socket)
{
#ifdef _WIN32
  DWORD timeout_ms = 400;
  ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  timeval timeout{};
  timeout.tv_usec = 400000;
  ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

void set_recv_timeout(boost::asio::ip::tcp::socket& socket)
{
#ifdef _WIN32
  DWORD timeout_ms = 1500;
  ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  timeval timeout{};
  timeout.tv_sec = 1;
  timeout.tv_usec = 500000;
  ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

ChannelRequest sample_request(std::string protocol)
{
  ChannelRequest request;
  request.lobby_id = "lobby";
  request.game_id = "game";
  request.user_a = "a";
  request.user_b = "b";
  request.protocol = std::move(protocol);
  request.lifetime_sec = 30;
  request.idle_sec = 30;
  request.max_packet = 1200;
  request.public_host = "127.0.0.1";
  return request;
}

bool udp_roundtrip(const ChannelInfo& info)
{
  boost::asio::io_context client;
  udp::socket left(client);
  udp::socket right(client);
  left.open(udp::v4());
  right.open(udp::v4());
  left.bind(udp::endpoint(udp::v4(), 0));
  right.bind(udp::endpoint(udp::v4(), 0));
  set_recv_timeout(left);
  set_recv_timeout(right);
  const auto token_a = hex_decode(info.a.token_hex);
  const auto token_b = hex_decode(info.b.token_hex);
  if( !token_a || !token_b || token_a->size() != 16 || token_b->size() != 16 )
  {
    return false;
  }
  const udp::endpoint dest_a(boost::asio::ip::make_address("127.0.0.1"),
                             static_cast<unsigned short>(info.a.port));
  const udp::endpoint dest_b(boost::asio::ip::make_address("127.0.0.1"),
                             static_cast<unsigned short>(info.b.port));
  for( int attempt = 0; attempt < 8; ++attempt )
  {
    left.send_to(boost::asio::buffer(*token_a), dest_a);
    right.send_to(boost::asio::buffer(*token_b), dest_b);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const std::string alpha = "alpha";
    const std::string beta = "beta";
    left.send_to(boost::asio::buffer(alpha), dest_a);
    std::array<char, 32> buf{};
    udp::endpoint from;
    boost::system::error_code ec;
    const auto n = right.receive_from(boost::asio::buffer(buf), from, 0, ec);
    if( ec || std::string(buf.data(), n) != alpha )
    {
      continue;
    }
    right.send_to(boost::asio::buffer(beta), dest_b);
    const auto back = left.receive_from(boost::asio::buffer(buf), from, 0, ec);
    if( !ec && std::string(buf.data(), back) == beta )
    {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("lifetime above the tighter cap is rejected")
{
  auto over = check_lifetime(100, 50, 80);
  REQUIRE_FALSE(over);
  CHECK(over.error().code == ErrorCode::LifetimeExceedsLimit);
  CHECK(over.error().max_lifetime_sec == 50);
  auto allowed = check_lifetime(40, 50, 80);
  REQUIRE(allowed);
  CHECK(*allowed == 40);
}

TEST_CASE("a short port range rolls back and a wider range forwards udp")
{
  RelayHarness harness;
  RelayConfig narrow;
  narrow.bind_host = "127.0.0.1";
  narrow.port_start = 53110;
  narrow.port_end = 53110;
  narrow.max_concurrent = 8;
  harness.on_strand(
      [&]
      {
        harness.relay.configure(narrow);
      });
  auto failed = harness.on_strand(
      [&]
      {
        return harness.relay.allocate(sample_request("udp"), RelayService::Clock::now());
      });
  REQUIRE_FALSE(failed);
  CHECK(failed.error().code == ErrorCode::Limit);
  CHECK(harness.on_strand(
            [&]
            {
              return harness.relay.channels();
            }) == 0);

  RelayConfig wide = narrow;
  wide.port_start = 53120;
  wide.port_end = 53140;
  harness.on_strand(
      [&]
      {
        harness.relay.configure(wide);
      });
  auto created = harness.on_strand(
      [&]
      {
        return harness.relay.allocate(sample_request("udp"), RelayService::Clock::now());
      });
  REQUIRE(created);
  CHECK(harness.on_strand(
            [&]
            {
              return harness.relay.channels();
            }) == 1);
  CHECK(udp_roundtrip(*created));
  const auto before = harness.relay.forwarded_bytes();
  CHECK(before >= std::string("alpha").size() + std::string("beta").size());
  REQUIRE(harness.on_strand(
      [&]
      {
        return harness.relay.release(created->id, RelayService::Clock::now());
      }));
  for( int i = 0; i < 50 && harness.on_strand(
                                [&]
                                {
                                  return harness.relay.channels();
                                }) != 0;
       ++i )
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(harness.on_strand(
            [&]
            {
              return harness.relay.channels();
            }) == 0);
}

TEST_CASE("tcp splice forwards bytes after the token")
{
  RelayHarness harness;
  RelayConfig config;
  config.bind_host = "127.0.0.1";
  config.port_start = 53200;
  config.port_end = 53220;
  config.tcp_enabled = true;
  harness.on_strand(
      [&]
      {
        harness.relay.configure(config);
      });
  auto disabled = harness.on_strand(
      [&]
      {
        RelayConfig off = config;
        off.tcp_enabled = false;
        harness.relay.configure(off);
        return harness.relay.allocate(sample_request("tcp"), RelayService::Clock::now());
      });
  REQUIRE_FALSE(disabled);
  harness.on_strand(
      [&]
      {
        harness.relay.configure(config);
      });
  auto created = harness.on_strand(
      [&]
      {
        return harness.relay.allocate(sample_request("tcp"), RelayService::Clock::now());
      });
  REQUIRE(created);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  boost::asio::io_context client;
  tcp::socket left(client);
  tcp::socket right(client);
  left.connect(
      {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(created->a.port)});
  right.connect(
      {boost::asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(created->b.port)});
  set_recv_timeout(left);
  set_recv_timeout(right);
  const auto token_a = hex_decode(created->a.token_hex);
  const auto token_b = hex_decode(created->b.token_hex);
  REQUIRE(token_a);
  REQUIRE(token_b);
  boost::asio::write(left, boost::asio::buffer(*token_a));
  boost::asio::write(right, boost::asio::buffer(*token_b));
  const std::string payload = "tcp-payload";
  boost::asio::write(left, boost::asio::buffer(payload));
  std::string got;
  got.resize(payload.size());
  boost::system::error_code ec;
  const auto n = boost::asio::read(right, boost::asio::buffer(got), ec);
  CHECK_FALSE(ec);
  CHECK(n == payload.size());
  CHECK(got == payload);
  REQUIRE(harness.on_strand(
      [&]
      {
        return harness.relay.release(created->id, RelayService::Clock::now());
      }));
}
