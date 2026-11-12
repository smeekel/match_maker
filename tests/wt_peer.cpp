#include "wt_peer.hpp"

#include "crypto.hpp"
#include "h3.hpp"
#include "wt.hpp"

#include <boost/asio.hpp>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>

namespace matchmaker::test
{
namespace
{

using udp = boost::asio::ip::udp;
using json = nlohmann::json;
using h3::Bytes;
using Clock = std::chrono::steady_clock;

ngtcp2_tstamp timestamp()
{
  return static_cast<ngtcp2_tstamp>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count());
}

ngtcp2_addr addr_of(const udp::endpoint& endpoint)
{
  return ngtcp2_addr{const_cast<ngtcp2_sockaddr*>(
                         reinterpret_cast<const ngtcp2_sockaddr*>(endpoint.data())),
                     static_cast<ngtcp2_socklen>(endpoint.size())};
}

struct Stream
{
  Bytes recv;
  bool fin = false;
  bool closed = false;
  bool reset = false;
  // Sent bytes stay alive for the whole test so ngtcp2 can retransmit them.
  std::deque<Bytes> chunks;
  std::uint64_t sent = 0;
  std::uint64_t total = 0;
  bool out_fin = false;
  bool out_fin_sent = false;

  void append(Bytes bytes)
  {
    total += bytes.size();
    chunks.push_back(std::move(bytes));
  }

  bool has_unsent() const
  {
    return sent < total || (out_fin && !out_fin_sent);
  }
};

} // namespace

struct WtPeer::Impl
{
  boost::asio::io_context ioc;
  udp::socket socket{ioc};
  udp::endpoint server;
  udp::endpoint local;
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  ngtcp2_crypto_ossl_ctx* ossl = nullptr;
  ngtcp2_conn* conn = nullptr;
  ngtcp2_crypto_conn_ref ref{};
  std::map<std::int64_t, Stream> streams;
  h3::Qpack qpack;
  std::int64_t request_id = -1;
  std::int64_t control_id = -1;
  int status = 0;
  bool session_closed = false;
  bool dead = false;
  Bytes capsules;
  std::deque<json> messages;
  std::deque<Bytes> datagrams_in;
  std::deque<Bytes> datagrams_out;
  std::size_t max_message = 1 << 20;

  ~Impl()
  {
    if( conn && !dead )
    {
      ngtcp2_ccerr error;
      ngtcp2_ccerr_default(&error);
      ngtcp2_ccerr_set_application_error(&error, h3::kNoError, nullptr, 0);
      std::array<std::uint8_t, 1452> packet{};
      ngtcp2_path_storage ps;
      ngtcp2_path_storage_zero(&ps);
      ngtcp2_pkt_info info{};
      const auto n = ngtcp2_conn_write_connection_close(conn, &ps.path, &info, packet.data(),
                                                        packet.size(), &error, timestamp());
      if( n > 0 )
      {
        boost::system::error_code ec;
        socket.send_to(boost::asio::buffer(packet.data(), static_cast<std::size_t>(n)), server,
                       0, ec);
      }
    }
    if( ssl )
    {
      SSL_set_app_data(ssl, nullptr);
      SSL_free(ssl);
    }
    ngtcp2_crypto_ossl_ctx_del(ossl);
    ngtcp2_conn_del(conn);
    if( ctx )
    {
      SSL_CTX_free(ctx);
    }
  }

  void init(std::uint16_t port)
  {
    wt_crypto_init();
    server = udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port);
    socket.open(udp::v4());
    socket.bind(udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    socket.non_blocking(true);
    local = socket.local_endpoint();

    ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    ssl = SSL_new(ctx);
    if( ngtcp2_crypto_ossl_ctx_new(&ossl, nullptr) != 0 )
    {
      throw std::runtime_error("ossl ctx");
    }
    ngtcp2_crypto_ossl_ctx_set_ssl(ossl, ssl);
    if( ngtcp2_crypto_ossl_configure_client_session(ssl) != 0 )
    {
      throw std::runtime_error("configure client session");
    }
    ref.get_conn = [](ngtcp2_crypto_conn_ref* r)
    {
      return static_cast<Impl*>(r->user_data)->conn;
    };
    ref.user_data = this;
    SSL_set_app_data(ssl, &ref);
    SSL_set_connect_state(ssl);
    static const unsigned char alpn[] = {2, 'h', '3'};
    SSL_set_alpn_protos(ssl, alpn, sizeof(alpn));
    SSL_set_tlsext_host_name(ssl, "localhost");

    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    callbacks.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    callbacks.rand = [](std::uint8_t* dest, std::size_t len, const ngtcp2_rand_ctx*)
    {
      random_bytes(std::span<std::uint8_t>(dest, len));
    };
    callbacks.get_new_connection_id2 = [](ngtcp2_conn*, ngtcp2_cid* cid,
                                          ngtcp2_stateless_reset_token* token, std::size_t len,
                                          void*) -> int
    {
      cid->datalen = len;
      random_bytes(std::span<std::uint8_t>(cid->data, len));
      random_bytes(std::span<std::uint8_t>(token->data, sizeof(token->data)));
      return 0;
    };
    callbacks.recv_stream_data = [](ngtcp2_conn* c, std::uint32_t flags, std::int64_t id,
                                    std::uint64_t, const std::uint8_t* data, std::size_t len,
                                    void* user, void*) -> int
    {
      auto* self = static_cast<Impl*>(user);
      auto& stream = self->streams[id];
      stream.recv.insert(stream.recv.end(), data, data + len);
      if( flags & NGTCP2_STREAM_DATA_FLAG_FIN )
      {
        stream.fin = true;
      }
      ngtcp2_conn_extend_max_stream_offset(c, id, len);
      ngtcp2_conn_extend_max_offset(c, len);
      return 0;
    };
    callbacks.stream_close = [](ngtcp2_conn*, std::uint32_t, std::int64_t id, std::uint64_t,
                                void* user, void*) -> int
    {
      static_cast<Impl*>(user)->streams[id].closed = true;
      return 0;
    };
    callbacks.stream_reset = [](ngtcp2_conn*, std::int64_t id, std::uint64_t, std::uint64_t,
                                void* user, void*) -> int
    {
      static_cast<Impl*>(user)->streams[id].reset = true;
      return 0;
    };
    callbacks.recv_datagram = [](ngtcp2_conn*, std::uint32_t, const std::uint8_t* data,
                                 std::size_t len, void* user) -> int
    {
      const auto quarter = h3::get_varint(std::span<const std::uint8_t>(data, len));
      if( quarter )
      {
        static_cast<Impl*>(user)->datagrams_in.emplace_back(data + quarter->second, data + len);
      }
      return 0;
    };

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp();
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = 8;
    params.initial_max_streams_uni = 8;
    params.max_idle_timeout = 30 * NGTCP2_SECONDS;
    params.max_datagram_frame_size = 65535;

    ngtcp2_cid dcid;
    ngtcp2_cid scid;
    dcid.datalen = 18;
    scid.datalen = 18;
    random_bytes(std::span<std::uint8_t>(dcid.data, dcid.datalen));
    random_bytes(std::span<std::uint8_t>(scid.data, scid.datalen));
    ngtcp2_path path{addr_of(local), addr_of(server), nullptr};
    if( ngtcp2_conn_client_new(&conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks,
                               &settings, &params, nullptr, this) != 0 )
    {
      throw std::runtime_error("ngtcp2_conn_client_new");
    }
    ngtcp2_conn_set_tls_native_handle(conn, ossl);
  }

  bool drive(Clock::time_point deadline, const std::function<bool()>& done)
  {
    std::array<std::uint8_t, 65536> buffer{};
    for( ;; )
    {
      if( done() )
      {
        return true;
      }
      for( ;; )
      {
        udp::endpoint from;
        boost::system::error_code ec;
        const auto n = socket.receive_from(boost::asio::buffer(buffer), from, 0, ec);
        if( ec )
        {
          break;
        }
        ngtcp2_path path{addr_of(local), addr_of(from), nullptr};
        ngtcp2_pkt_info info{};
        const int rv = ngtcp2_conn_read_pkt(conn, &path, &info, buffer.data(), n, timestamp());
        if( rv != 0 )
        {
          dead = true;
          session_closed = true;
          break;
        }
      }
      process();
      if( !dead && ngtcp2_conn_get_expiry2(conn) <= timestamp() )
      {
        if( ngtcp2_conn_handle_expiry(conn, timestamp()) != 0 )
        {
          dead = true;
          session_closed = true;
        }
      }
      if( !dead )
      {
        write();
      }
      if( dead || Clock::now() >= deadline )
      {
        return done();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void process()
  {
    for( auto& [id, stream] : streams )
    {
      if( id == request_id )
      {
        on_request_stream(stream);
      }
      else if( (id & 0x3) == 0x1 )
      {
        on_server_bidi(id, stream);
      }
      else if( (id & 0x1) == 0x1 )
      {
        stream.recv.clear();
      }
    }
  }

  void on_request_stream(Stream& stream)
  {
    for( ;; )
    {
      const auto frame = h3::get_frame(stream.recv);
      if( !frame )
      {
        break;
      }
      if( frame->type == h3::kFrameHeaders && status == 0 )
      {
        const auto headers = qpack.decode(request_id, frame->payload);
        if( headers )
        {
          status = std::atoi(std::string(h3::find_header(*headers, ":status")).c_str());
        }
      }
      else if( frame->type == h3::kFrameData && status == 200 )
      {
        capsules.insert(capsules.end(), frame->payload.begin(), frame->payload.end());
      }
      stream.recv.erase(stream.recv.begin(),
                        stream.recv.begin() + static_cast<std::ptrdiff_t>(frame->size));
    }
    const auto type = h3::get_varint(capsules);
    if( type && type->first == h3::kCapsuleCloseSession )
    {
      session_closed = true;
    }
    if( status == 200 && (stream.fin || stream.reset || stream.closed) )
    {
      session_closed = true;
    }
  }

  void on_server_bidi(std::int64_t id, Stream& stream)
  {
    if( control_id < 0 )
    {
      const auto signal = h3::get_varint(stream.recv);
      if( !signal )
      {
        return;
      }
      const auto session = h3::get_varint(std::span<const std::uint8_t>(stream.recv).subspan(
          signal->second));
      if( !session )
      {
        return;
      }
      if( signal->first != h3::kSignalWtBidi ||
          session->first != static_cast<std::uint64_t>(request_id) )
      {
        stream.recv.clear();
        return;
      }
      stream.recv.erase(stream.recv.begin(),
                        stream.recv.begin() +
                            static_cast<std::ptrdiff_t>(signal->second + session->second));
      control_id = id;
    }
    if( id != control_id )
    {
      return;
    }
    for( ;; )
    {
      std::string text;
      if( h3::take_message(stream.recv, max_message, text) != h3::TakeResult::Message )
      {
        break;
      }
      messages.push_back(json::parse(text, nullptr, false));
    }
  }

  void write()
  {
    std::array<std::uint8_t, 1452> packet{};
    const auto ts = timestamp();
    for( int guard = 0; guard < 256; ++guard )
    {
      ngtcp2_path_storage ps;
      ngtcp2_path_storage_zero(&ps);
      ngtcp2_pkt_info info{};
      ngtcp2_ssize nwrite = 0;
      if( !datagrams_out.empty() )
      {
        ngtcp2_vec vec{datagrams_out.front().data(), datagrams_out.front().size()};
        int accepted = 0;
        nwrite = ngtcp2_conn_writev_datagram(conn, &ps.path, &info, packet.data(), packet.size(),
                                             &accepted, NGTCP2_WRITE_DATAGRAM_FLAG_MORE, 0,
                                             &vec, 1, ts);
        if( accepted || nwrite < 0 )
        {
          datagrams_out.pop_front();
        }
        if( nwrite == NGTCP2_ERR_WRITE_MORE || nwrite < 0 )
        {
          continue;
        }
      }
      else
      {
        std::int64_t id = -1;
        Stream* stream = nullptr;
        for( auto& [sid, candidate] : streams )
        {
          if( candidate.has_unsent() )
          {
            id = sid;
            stream = &candidate;
            break;
          }
        }
        std::vector<ngtcp2_vec> vecs;
        std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if( stream )
        {
          std::uint64_t offset = 0;
          for( auto& chunk : stream->chunks )
          {
            const auto end = offset + chunk.size();
            if( end > stream->sent )
            {
              const auto skip = static_cast<std::size_t>(
                  stream->sent > offset ? stream->sent - offset : 0);
              vecs.push_back(ngtcp2_vec{chunk.data() + skip, chunk.size() - skip});
            }
            offset = end;
          }
          if( stream->out_fin )
          {
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
          }
        }
        ngtcp2_ssize consumed = -1;
        nwrite = ngtcp2_conn_writev_stream(conn, &ps.path, &info, packet.data(), packet.size(),
                                           &consumed, flags, id, vecs.data(), vecs.size(), ts);
        if( stream && consumed >= 0 )
        {
          stream->sent += static_cast<std::uint64_t>(consumed);
          if( (flags & NGTCP2_WRITE_STREAM_FLAG_FIN) && stream->sent == stream->total )
          {
            stream->out_fin_sent = true;
          }
        }
        if( nwrite == NGTCP2_ERR_WRITE_MORE )
        {
          continue;
        }
        if( nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED )
        {
          break;
        }
        if( nwrite == NGTCP2_ERR_STREAM_SHUT_WR || nwrite == NGTCP2_ERR_STREAM_NOT_FOUND )
        {
          if( stream )
          {
            stream->sent = stream->total;
            stream->out_fin_sent = true;
          }
          continue;
        }
        if( nwrite < 0 )
        {
          dead = true;
          session_closed = true;
          return;
        }
      }
      if( nwrite == 0 )
      {
        break;
      }
      boost::system::error_code ec;
      socket.send_to(boost::asio::buffer(packet.data(), static_cast<std::size_t>(nwrite)),
                     server, 0, ec);
    }
    ngtcp2_conn_update_pkt_tx_time(conn, ts);
  }
};

WtPeer::WtPeer(std::uint16_t port) : impl_(std::make_unique<Impl>())
{
  impl_->init(port);
}

WtPeer::~WtPeer() = default;

int WtPeer::connect(const std::string& path, const std::string& origin,
                    std::chrono::milliseconds timeout)
{
  auto& impl = *impl_;
  const auto deadline = Clock::now() + timeout;
  if( !impl.drive(deadline,
                  [&]
                  {
                    return ngtcp2_conn_get_handshake_completed2(impl.conn) != 0;
                  }) )
  {
    return 0;
  }
  std::int64_t control = -1;
  if( ngtcp2_conn_open_uni_stream(impl.conn, &control, nullptr) == 0 )
  {
    impl.streams[control].append(h3::control_stream_preamble(false));
  }
  if( ngtcp2_conn_open_bidi_stream(impl.conn, &impl.request_id, nullptr) != 0 )
  {
    return 0;
  }
  h3::Headers headers{{":method", "CONNECT"},
                      {":protocol", "webtransport"},
                      {":scheme", "https"},
                      {":authority", "127.0.0.1"},
                      {":path", path},
                      {"sec-webtransport-http3-draft02", "1"}};
  if( !origin.empty() )
  {
    headers.emplace_back("origin", origin);
  }
  Bytes frame;
  h3::put_frame(frame, h3::kFrameHeaders, impl.qpack.encode(impl.request_id, headers));
  impl.streams[impl.request_id].append(std::move(frame));
  impl.drive(deadline,
             [&]
             {
               return impl.status != 0;
             });
  return impl.status;
}

void WtPeer::send(const nlohmann::json& message)
{
  auto& impl = *impl_;
  if( impl.control_id < 0 )
  {
    impl.drive(Clock::now() + std::chrono::seconds(2),
               [&]
               {
                 return impl.control_id >= 0;
               });
  }
  if( impl.control_id < 0 )
  {
    throw std::runtime_error("no control stream");
  }
  Bytes framed;
  h3::put_message(framed, message.dump());
  impl.streams[impl.control_id].append(std::move(framed));
  impl.drive(Clock::now(),
             []
             {
               return false;
             });
}

std::optional<nlohmann::json> WtPeer::read(std::chrono::milliseconds timeout)
{
  auto& impl = *impl_;
  impl.drive(Clock::now() + timeout,
             [&]
             {
               return !impl.messages.empty();
             });
  if( impl.messages.empty() )
  {
    return std::nullopt;
  }
  auto message = std::move(impl.messages.front());
  impl.messages.pop_front();
  return message;
}

std::optional<nlohmann::json> WtPeer::read_until(
    const std::function<bool(const nlohmann::json&)>& pred, std::chrono::milliseconds timeout)
{
  const auto deadline = Clock::now() + timeout;
  while( Clock::now() < deadline )
  {
    auto message = read(std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now()));
    if( !message )
    {
      return std::nullopt;
    }
    if( message->value("op", "") == "ping" )
    {
      send({{"op", "pong"}, {"id", (*message)["id"]}});
      continue;
    }
    if( pred(*message) )
    {
      return message;
    }
  }
  return std::nullopt;
}

void WtPeer::send_datagram(std::span<const std::uint8_t> payload)
{
  auto& impl = *impl_;
  Bytes datagram;
  h3::put_varint(datagram, static_cast<std::uint64_t>(impl.request_id / 4));
  datagram.insert(datagram.end(), payload.begin(), payload.end());
  impl.datagrams_out.push_back(std::move(datagram));
  impl.drive(Clock::now(),
             []
             {
               return false;
             });
}

std::optional<std::vector<std::uint8_t>> WtPeer::read_datagram(std::chrono::milliseconds timeout)
{
  auto& impl = *impl_;
  impl.drive(Clock::now() + timeout,
             [&]
             {
               return !impl.datagrams_in.empty();
             });
  if( impl.datagrams_in.empty() )
  {
    return std::nullopt;
  }
  auto datagram = std::move(impl.datagrams_in.front());
  impl.datagrams_in.pop_front();
  return datagram;
}

bool WtPeer::session_closed(std::chrono::milliseconds timeout)
{
  auto& impl = *impl_;
  return impl.drive(Clock::now() + timeout,
                    [&]
                    {
                      return impl.session_closed;
                    });
}

void WtPeer::close_session()
{
  auto& impl = *impl_;
  if( impl.request_id < 0 )
  {
    return;
  }
  Bytes framed;
  h3::put_frame(framed, h3::kFrameData, h3::close_session_capsule(0, {}));
  auto& stream = impl.streams[impl.request_id];
  stream.append(std::move(framed));
  stream.out_fin = true;
  impl.drive(Clock::now() + std::chrono::milliseconds(200),
             []
             {
               return false;
             });
}

} // namespace matchmaker::test
