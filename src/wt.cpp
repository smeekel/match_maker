#include "wt.hpp"

#include "crypto.hpp"
#include "h3.hpp"
#include "util.hpp"

#include <boost/asio.hpp>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace matchmaker
{
namespace
{

using udp = boost::asio::ip::udp;
using json = nlohmann::json;
using h3::Bytes;

constexpr std::size_t kScidLen = 18;
constexpr std::size_t kPacketBytes = 1452;
constexpr std::size_t kMaxRequestBuffer = 16384;
constexpr std::size_t kMaxDatagramQueue = 256;
constexpr int kMaxPacketsPerWrite = 64;
constexpr auto kLinger = std::chrono::seconds(2);

ngtcp2_tstamp timestamp()
{
  return static_cast<ngtcp2_tstamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

std::string cid_key(const std::uint8_t* data, std::size_t len)
{
  return std::string(reinterpret_cast<const char*>(data), len);
}

ngtcp2_addr addr_of(const udp::endpoint& endpoint)
{
  return ngtcp2_addr{const_cast<ngtcp2_sockaddr*>(
                         reinterpret_cast<const ngtcp2_sockaddr*>(endpoint.data())),
                     static_cast<ngtcp2_socklen>(endpoint.size())};
}

udp::endpoint endpoint_of(const ngtcp2_addr& addr)
{
  udp::endpoint endpoint;
  std::memcpy(endpoint.data(), addr.addr, static_cast<std::size_t>(addr.addrlen));
  endpoint.resize(static_cast<std::size_t>(addr.addrlen));
  return endpoint;
}

bool client_stream(std::int64_t id)
{
  return (id & 0x1) == 0;
}

bool bidi_stream(std::int64_t id)
{
  return (id & 0x2) == 0;
}

int alpn_select(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                unsigned int inlen, void*)
{
  for( unsigned int i = 0; i < inlen; )
  {
    const unsigned int len = in[i];
    if( i + 1 + len > inlen )
    {
      break;
    }
    if( len == 2 && in[i + 1] == 'h' && in[i + 2] == '3' )
    {
      *out = in + i + 1;
      *outlen = static_cast<unsigned char>(len);
      return SSL_TLSEXT_ERR_OK;
    }
    i += 1 + len;
  }
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}

json error_json(const Error& error)
{
  return {{"error", error_code_string(error.code)}, {"message", error.message}};
}

// Bytes handed to ngtcp2 must stay put until they are acknowledged, and
// acknowledgements arrive in offset order.
struct SendBuffer
{
  std::deque<Bytes> chunks;
  std::uint64_t base = 0;
  std::uint64_t sent = 0;
  std::uint64_t total = 0;
  bool fin = false;
  bool fin_sent = false;

  void append(Bytes bytes)
  {
    if( bytes.empty() || fin )
    {
      return;
    }
    total += bytes.size();
    chunks.push_back(std::move(bytes));
  }

  bool has_unsent() const
  {
    return sent < total || (fin && !fin_sent);
  }

  std::size_t gather(ngtcp2_vec* vecs, std::size_t max) const
  {
    std::size_t n = 0;
    std::uint64_t offset = base;
    for( const auto& chunk : chunks )
    {
      if( n == max )
      {
        break;
      }
      const std::uint64_t end = offset + chunk.size();
      if( end > sent )
      {
        const auto skip = static_cast<std::size_t>(sent > offset ? sent - offset : 0);
        vecs[n].base = const_cast<std::uint8_t*>(chunk.data()) + skip;
        vecs[n].len = chunk.size() - skip;
        ++n;
      }
      offset = end;
    }
    return n;
  }

  void ack(std::uint64_t end)
  {
    while( !chunks.empty() && base + chunks.front().size() <= end )
    {
      base += chunks.front().size();
      chunks.pop_front();
    }
  }

  std::uint64_t unacked() const
  {
    return total - base;
  }

  void abandon()
  {
    chunks.clear();
    base = sent = total;
    fin = true;
    fin_sent = true;
  }
};

struct Stream
{
  enum class Kind
  {
    Pending,
    Request,
    Session,
    Control,
    Discard,
    Local
  };
  Kind kind = Kind::Pending;
  Bytes recv;
  bool fin = false;
  bool closed = false;
  bool reset = false;
  SendBuffer out;
};

} // namespace

struct WtServer::Impl : std::enable_shared_from_this<WtServer::Impl>
{
  Impl(boost::asio::io_context& ioc, Strand strand, WtHandlers handlers)
      : ioc(ioc), strand(std::move(strand)), handlers(std::move(handlers)), socket(ioc)
  {
  }

  ~Impl()
  {
    if( ssl_ctx )
    {
      SSL_CTX_free(ssl_ctx);
    }
  }

  void receive();
  void on_datagram(std::size_t n);
  void send_to(const udp::endpoint& to, const std::uint8_t* data, std::size_t n);
  void forget(WtConnection& connection);

  boost::asio::io_context& ioc;
  Strand strand;
  WtHandlers handlers;
  WtConfig config;
  udp::socket socket;
  udp::endpoint local;
  udp::endpoint from;
  SSL_CTX* ssl_ctx = nullptr;
  std::array<std::uint8_t, 32> reset_secret{};
  std::array<std::uint8_t, 65536> buffer{};
  std::unordered_map<std::string, std::shared_ptr<WtConnection>> by_cid;
  std::unordered_set<std::shared_ptr<WtConnection>> connections;
  bool stopped = false;
};

class WtConnection : public std::enable_shared_from_this<WtConnection>
{
public:
  using Impl = WtServer::Impl;

  explicit WtConnection(std::shared_ptr<Impl> server)
      : server_(std::move(server)), timer_(server_->strand), linger_(server_->strand)
  {
  }

  ~WtConnection()
  {
    if( ssl_ )
    {
      SSL_set_app_data(ssl_, nullptr);
      SSL_free(ssl_);
    }
    ngtcp2_crypto_ossl_ctx_del(ossl_);
    ngtcp2_conn_del(conn_);
  }

  bool init(const ngtcp2_pkt_hd& hd, const udp::endpoint& remote)
  {
    ngtcp2_callbacks callbacks{};
    callbacks.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
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
                                          void* user) -> int
    {
      return static_cast<WtConnection*>(user)->on_new_cid(cid, token, len);
    };
    callbacks.remove_connection_id = [](ngtcp2_conn*, const ngtcp2_cid* cid, void* user) -> int
    {
      static_cast<WtConnection*>(user)->on_remove_cid(cid);
      return 0;
    };
    callbacks.recv_stream_data = [](ngtcp2_conn*, std::uint32_t flags, std::int64_t id,
                                    std::uint64_t, const std::uint8_t* data, std::size_t len,
                                    void* user, void*) -> int
    {
      static_cast<WtConnection*>(user)->on_stream_data(flags, id, data, len);
      return 0;
    };
    callbacks.acked_stream_data_offset = [](ngtcp2_conn*, std::int64_t id, std::uint64_t offset,
                                            std::uint64_t len, void* user, void*) -> int
    {
      static_cast<WtConnection*>(user)->on_acked(id, offset + len);
      return 0;
    };
    callbacks.stream_close = [](ngtcp2_conn*, std::uint32_t, std::int64_t id, std::uint64_t,
                                void* user, void*) -> int
    {
      static_cast<WtConnection*>(user)->on_stream_close(id);
      return 0;
    };
    callbacks.stream_reset = [](ngtcp2_conn*, std::int64_t id, std::uint64_t, std::uint64_t,
                                void* user, void*) -> int
    {
      static_cast<WtConnection*>(user)->on_stream_reset(id);
      return 0;
    };
    callbacks.recv_datagram = [](ngtcp2_conn*, std::uint32_t, const std::uint8_t* data,
                                 std::size_t len, void* user) -> int
    {
      static_cast<WtConnection*>(user)->datagrams_in_.emplace_back(data, data + len);
      return 0;
    };

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = timestamp();

    const auto idle = std::max<std::int64_t>(5, server_->config.idle_timeout.count());
    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_streams_bidi = 4;
    params.initial_max_streams_uni = 8;
    params.max_idle_timeout = static_cast<ngtcp2_duration>(idle) * NGTCP2_SECONDS;
    params.max_datagram_frame_size = 65535;
    params.disable_active_migration = 1;
    params.original_dcid = hd.dcid;
    params.original_dcid_present = 1;

    ngtcp2_cid scid;
    scid.datalen = kScidLen;
    random_bytes(std::span<std::uint8_t>(scid.data, kScidLen));

    ngtcp2_path path{addr_of(server_->local), addr_of(remote), nullptr};
    if( ngtcp2_conn_server_new(&conn_, &hd.scid, &scid, &path, hd.version, &callbacks, &settings,
                               &params, nullptr, this) != 0 )
    {
      return false;
    }
    ssl_ = SSL_new(server_->ssl_ctx);
    if( !ssl_ || ngtcp2_crypto_ossl_ctx_new(&ossl_, nullptr) != 0 )
    {
      return false;
    }
    ngtcp2_crypto_ossl_ctx_set_ssl(ossl_, ssl_);
    if( ngtcp2_crypto_ossl_configure_server_session(ssl_) != 0 )
    {
      return false;
    }
    ref_.get_conn = [](ngtcp2_crypto_conn_ref* ref)
    {
      return static_cast<WtConnection*>(ref->user_data)->conn_;
    };
    ref_.user_data = this;
    SSL_set_app_data(ssl_, &ref_);
    SSL_set_accept_state(ssl_);
    ngtcp2_conn_set_tls_native_handle(conn_, ossl_);
    ngtcp2_conn_set_keep_alive_timeout(conn_, static_cast<ngtcp2_duration>(idle) *
                                                  NGTCP2_SECONDS / 3);
    add_cid(cid_key(scid.data, scid.datalen));
    add_cid(cid_key(hd.dcid.data, hd.dcid.datalen));
    return true;
  }

  void on_packet(const udp::endpoint& remote, std::span<const std::uint8_t> packet)
  {
    if( dead_ )
    {
      return;
    }
    auto self = shared_from_this();
    ngtcp2_path path{addr_of(server_->local), addr_of(remote), nullptr};
    ngtcp2_pkt_info info{};
    const int rv =
        ngtcp2_conn_read_pkt(conn_, &path, &info, packet.data(), packet.size(), timestamp());
    if( rv != 0 )
    {
      if( rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_DROP_CONN )
      {
        remove();
        return;
      }
      fail(rv);
      return;
    }
    process();
    write();
  }

  std::uint64_t control_unacked() const
  {
    std::uint64_t pending = 0;
    for( const auto& chunk : pending_control_ )
    {
      pending += chunk.size();
    }
    if( const auto* control = find(control_id_) )
    {
      pending += control->out.unacked();
    }
    return pending;
  }

  void write_control(Bytes data)
  {
    if( dead_ || closing_session_ )
    {
      return;
    }
    if( control_id_ < 0 )
    {
      pending_control_.push_back(std::move(data));
      return;
    }
    if( auto* control = find(control_id_) )
    {
      control->out.append(std::move(data));
      schedule_write();
    }
  }

  void queue_datagram(std::span<const std::uint8_t> datagram)
  {
    if( dead_ || closing_session_ || session_id_ < 0 ||
        datagrams_out_.size() >= kMaxDatagramQueue )
    {
      return;
    }
    Bytes bytes;
    h3::put_varint(bytes, static_cast<std::uint64_t>(session_id_ / 4));
    bytes.insert(bytes.end(), datagram.begin(), datagram.end());
    datagrams_out_.push_back(std::move(bytes));
    schedule_write();
  }

  // Ends the session on the wire. With |after_flush| the control stream is
  // finished first so queued messages reach the client before the close.
  void close_session(bool after_flush)
  {
    if( dead_ || closing_session_ )
    {
      return;
    }
    closing_session_ = true;
    pending_control_.clear();
    datagrams_out_.clear();
    auto* control = find(control_id_);
    if( control )
    {
      control->out.fin = true;
    }
    if( after_flush && control && control->out.unacked() > 0 )
    {
      flushing_ = true;
      arm_linger();
    }
    else
    {
      send_close_capsule();
    }
    schedule_write();
  }

  void remove()
  {
    if( dead_ )
    {
      return;
    }
    auto self = shared_from_this();
    dead_ = true;
    if( session_ && !session_->ended() )
    {
      session_->end(WtSession::Ended::Peer);
    }
    timer_.cancel();
    linger_.cancel();
    server_->forget(*this);
    if( server_->handlers.release )
    {
      server_->handlers.release();
    }
  }

  void close_connection(std::uint64_t code)
  {
    if( dead_ )
    {
      return;
    }
    if( !ngtcp2_conn_in_closing_period2(conn_) && !ngtcp2_conn_in_draining_period2(conn_) )
    {
      ngtcp2_ccerr error;
      ngtcp2_ccerr_default(&error);
      ngtcp2_ccerr_set_application_error(&error, code, nullptr, 0);
      send_close(error);
    }
    remove();
  }

  const std::vector<std::string>& cids() const
  {
    return cids_;
  }

private:
  void add_cid(std::string key)
  {
    server_->by_cid[key] = shared_from_this();
    cids_.push_back(std::move(key));
  }

  int on_new_cid(ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t len)
  {
    cid->datalen = len;
    random_bytes(std::span<std::uint8_t>(cid->data, len));
    if( ngtcp2_crypto_generate_stateless_reset_token(token->data, server_->reset_secret.data(),
                                                     server_->reset_secret.size(), cid) != 0 )
    {
      return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    add_cid(cid_key(cid->data, cid->datalen));
    return 0;
  }

  void on_remove_cid(const ngtcp2_cid* cid)
  {
    const auto key = cid_key(cid->data, cid->datalen);
    server_->by_cid.erase(key);
    std::erase(cids_, key);
  }

  void on_stream_data(std::uint32_t flags, std::int64_t id, const std::uint8_t* data,
                      std::size_t len)
  {
    ngtcp2_conn_extend_max_stream_offset(conn_, id, len);
    ngtcp2_conn_extend_max_offset(conn_, len);
    auto& stream = streams_[id];
    if( stream.kind != Stream::Kind::Discard )
    {
      stream.recv.insert(stream.recv.end(), data, data + len);
    }
    if( flags & NGTCP2_STREAM_DATA_FLAG_FIN )
    {
      stream.fin = true;
    }
    dirty_.push_back(id);
  }

  void on_acked(std::int64_t id, std::uint64_t end)
  {
    auto* stream = find(id);
    if( !stream )
    {
      return;
    }
    stream->out.ack(end);
    if( id == control_id_ && flushing_ && stream->out.unacked() == 0 )
    {
      flushing_ = false;
      send_close_capsule();
    }
  }

  void on_stream_close(std::int64_t id)
  {
    if( auto* stream = find(id) )
    {
      stream->closed = true;
      dirty_.push_back(id);
    }
    if( client_stream(id) )
    {
      if( bidi_stream(id) )
      {
        ngtcp2_conn_extend_max_streams_bidi(conn_, 1);
      }
      else
      {
        ngtcp2_conn_extend_max_streams_uni(conn_, 1);
      }
    }
  }

  void on_stream_reset(std::int64_t id)
  {
    if( auto* stream = find(id) )
    {
      stream->reset = true;
      dirty_.push_back(id);
    }
  }

  Stream* find(std::int64_t id)
  {
    if( id < 0 )
    {
      return nullptr;
    }
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
  }

  const Stream* find(std::int64_t id) const
  {
    if( id < 0 )
    {
      return nullptr;
    }
    const auto it = streams_.find(id);
    return it == streams_.end() ? nullptr : &it->second;
  }

  void process()
  {
    if( !local_streams_ && ngtcp2_conn_get_handshake_completed2(conn_) )
    {
      local_streams_ = true;
      open_uni(h3::control_stream_preamble(true));
      Bytes encoder;
      h3::put_varint(encoder, h3::kStreamQpackEncoder);
      open_uni(std::move(encoder));
      Bytes decoder;
      h3::put_varint(decoder, h3::kStreamQpackDecoder);
      open_uni(std::move(decoder));
    }

    auto datagrams = std::move(datagrams_in_);
    datagrams_in_.clear();
    for( const auto& datagram : datagrams )
    {
      on_session_datagram(datagram);
      if( dead_ )
      {
        return;
      }
    }

    auto ids = std::move(dirty_);
    dirty_.clear();
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for( const auto id : ids )
    {
      auto* stream = find(id);
      if( !stream )
      {
        continue;
      }
      if( id == control_id_ )
      {
        on_control_data(*stream);
      }
      else if( client_stream(id) && bidi_stream(id) )
      {
        on_request_data(id, *stream);
      }
      else if( client_stream(id) )
      {
        on_uni_data(id, *stream);
      }
      if( dead_ )
      {
        return;
      }
      stream = find(id);
      if( !stream )
      {
        continue;
      }
      const bool gone = stream->closed || stream->reset;
      if( (id == session_id_ || id == control_id_) && session_ && !session_->ended() &&
          (gone || (id == session_id_ && stream->fin)) )
      {
        auto session = session_;
        session->end(WtSession::Ended::Peer);
      }
      if( stream->closed )
      {
        streams_.erase(id);
      }
    }
  }

  void open_uni(Bytes preamble)
  {
    std::int64_t id = -1;
    if( ngtcp2_conn_open_uni_stream(conn_, &id, nullptr) != 0 )
    {
      return;
    }
    auto& stream = streams_[id];
    stream.kind = Stream::Kind::Local;
    stream.out.append(std::move(preamble));
  }

  void on_session_datagram(const Bytes& datagram)
  {
    if( !session_ || session_->ended() )
    {
      return;
    }
    const auto quarter = h3::get_varint(datagram);
    if( !quarter || quarter->first * 4 != static_cast<std::uint64_t>(session_id_) )
    {
      return;
    }
    auto session = session_;
    if( server_->handlers.datagram )
    {
      server_->handlers.datagram(session, std::span<const std::uint8_t>(datagram).subspan(
                                              quarter->second));
    }
  }

  void on_uni_data(std::int64_t id, Stream& stream)
  {
    if( stream.kind == Stream::Kind::Pending )
    {
      const auto type = h3::get_varint(stream.recv);
      if( !type )
      {
        return;
      }
      stream.recv.erase(stream.recv.begin(),
                        stream.recv.begin() + static_cast<std::ptrdiff_t>(type->second));
      stream.kind = Stream::Kind::Discard;
      if( type->first == h3::kStreamWtUni )
      {
        ngtcp2_conn_shutdown_stream_read(conn_, 0, id, h3::kRequestRejected);
      }
    }
    stream.recv.clear();
  }

  void on_request_data(std::int64_t id, Stream& stream)
  {
    if( stream.kind == Stream::Kind::Pending )
    {
      const auto first = h3::get_varint(stream.recv);
      if( !first )
      {
        return;
      }
      if( first->first == h3::kSignalWtBidi )
      {
        reject(id, stream);
        return;
      }
      stream.kind = Stream::Kind::Request;
    }
    if( stream.kind == Stream::Kind::Request )
    {
      for( ;; )
      {
        const auto frame = h3::get_frame(stream.recv);
        if( !frame )
        {
          if( stream.recv.size() > kMaxRequestBuffer )
          {
            reject(id, stream);
          }
          return;
        }
        if( frame->type != h3::kFrameHeaders )
        {
          if( frame->type == h3::kFrameData )
          {
            reject(id, stream);
            return;
          }
          stream.recv.erase(stream.recv.begin(),
                            stream.recv.begin() + static_cast<std::ptrdiff_t>(frame->size));
          continue;
        }
        const auto headers = qpack_.decode(id, frame->payload);
        stream.recv.erase(stream.recv.begin(),
                          stream.recv.begin() + static_cast<std::ptrdiff_t>(frame->size));
        if( !headers )
        {
          reject(id, stream);
          return;
        }
        on_request(id, stream, *headers);
        break;
      }
    }
    if( stream.kind == Stream::Kind::Session )
    {
      for( ;; )
      {
        const auto frame = h3::get_frame(stream.recv);
        if( !frame )
        {
          break;
        }
        if( frame->type == h3::kFrameData )
        {
          capsules_.insert(capsules_.end(), frame->payload.begin(), frame->payload.end());
        }
        stream.recv.erase(stream.recv.begin(),
                          stream.recv.begin() + static_cast<std::ptrdiff_t>(frame->size));
      }
      for( ;; )
      {
        const auto type = h3::get_varint(capsules_);
        if( !type )
        {
          break;
        }
        const auto length = h3::get_varint(std::span<const std::uint8_t>(capsules_).subspan(
            type->second));
        if( !length || capsules_.size() - type->second - length->second < length->first )
        {
          break;
        }
        const auto size =
            static_cast<std::ptrdiff_t>(type->second + length->second + length->first);
        capsules_.erase(capsules_.begin(), capsules_.begin() + size);
        if( type->first == h3::kCapsuleCloseSession && session_ && !session_->ended() )
        {
          auto session = session_;
          session->end(WtSession::Ended::Peer);
          return;
        }
      }
    }
    if( stream.kind == Stream::Kind::Discard )
    {
      stream.recv.clear();
    }
  }

  void reject(std::int64_t id, Stream& stream)
  {
    stream.kind = Stream::Kind::Discard;
    stream.recv.clear();
    stream.out.abandon();
    ngtcp2_conn_shutdown_stream(conn_, 0, id, h3::kRequestRejected);
  }

  void respond(std::int64_t id, int status, const json& body, h3::Headers extra, bool fin)
  {
    auto* stream = find(id);
    if( !stream )
    {
      return;
    }
    h3::Headers headers{{":status", std::to_string(status)}};
    for( auto& header : extra )
    {
      headers.push_back(std::move(header));
    }
    std::string text;
    if( !body.is_null() )
    {
      text = body.dump();
      headers.emplace_back("content-type", "application/json; charset=utf-8");
    }
    Bytes out;
    h3::put_frame(out, h3::kFrameHeaders, qpack_.encode(id, headers));
    if( !text.empty() )
    {
      h3::put_frame(out, h3::kFrameData,
                    std::span<const std::uint8_t>(
                        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }
    stream->out.append(std::move(out));
    if( fin )
    {
      stream->out.fin = true;
    }
  }

  void on_request(std::int64_t id, Stream& stream, const h3::Headers& headers)
  {
    const auto method = h3::find_header(headers, ":method");
    const auto protocol = h3::find_header(headers, ":protocol");
    if( session_ )
    {
      reject(id, stream);
      return;
    }
    if( method != "CONNECT" || protocol != "webtransport" )
    {
      stream.kind = Stream::Kind::Discard;
      respond(id, method == "CONNECT" ? 400 : 404,
              error_json(Error{method == "CONNECT" ? ErrorCode::BadRequest : ErrorCode::NotFound,
                               "only WebTransport CONNECT is served here", std::nullopt}),
              {}, true);
      return;
    }
    const auto target = h3::find_header(headers, ":path");
    WtRequest request;
    const auto question = target.find('?');
    request.path = std::string(target.substr(0, question));
    if( question != std::string_view::npos )
    {
      request.query = std::string(target.substr(question + 1));
    }
    request.origin = std::string(h3::find_header(headers, "origin"));

    auto self = shared_from_this();
    auto session = std::make_shared<WtSession>(
        server_->strand, weak_from_this(), server_->config.max_outbound_bytes,
        server_->handlers.ended);
    std::optional<Error> refused;
    if( server_->handlers.connect )
    {
      refused = server_->handlers.connect(request, session);
    }
    if( dead_ )
    {
      return;
    }
    if( refused )
    {
      pending_control_.clear();
      stream.kind = Stream::Kind::Discard;
      respond(id, http_status(refused->code), error_json(*refused), {}, true);
      arm_linger();
      return;
    }
    session_ = session;
    session_id_ = id;
    stream.kind = Stream::Kind::Session;
    h3::Headers extra;
    if( !h3::find_header(headers, "sec-webtransport-http3-draft02").empty() )
    {
      extra.emplace_back("sec-webtransport-http3-draft", "draft02");
    }
    respond(id, 200, nullptr, std::move(extra), false);
    std::int64_t control = -1;
    if( ngtcp2_conn_open_bidi_stream(conn_, &control, nullptr) != 0 )
    {
      session->end(WtSession::Ended::Peer);
      return;
    }
    control_id_ = control;
    auto& control_stream = streams_[control];
    control_stream.kind = Stream::Kind::Control;
    Bytes prefix;
    h3::put_varint(prefix, h3::kSignalWtBidi);
    h3::put_varint(prefix, static_cast<std::uint64_t>(id));
    control_stream.out.append(std::move(prefix));
    for( auto& chunk : pending_control_ )
    {
      control_stream.out.append(std::move(chunk));
    }
    pending_control_.clear();
  }

  void on_control_data(Stream& stream)
  {
    if( !session_ || session_->ended() )
    {
      stream.recv.clear();
      return;
    }
    auto session = session_;
    for( ;; )
    {
      std::string text;
      const auto taken = h3::take_message(stream.recv, server_->config.max_message_bytes, text);
      if( taken == h3::TakeResult::Incomplete )
      {
        return;
      }
      if( taken == h3::TakeResult::TooLarge )
      {
        stream.recv.clear();
        session->end(WtSession::Ended::Peer);
        return;
      }
      const auto message = json::parse(text, nullptr, false);
      if( session->take_pong(message) )
      {
        continue;
      }
      if( server_->handlers.message )
      {
        server_->handlers.message(session, message);
      }
      if( dead_ || session->ended() )
      {
        stream.recv.clear();
        return;
      }
    }
  }

  void send_close_capsule()
  {
    if( auto* stream = find(session_id_) )
    {
      Bytes framed;
      h3::put_frame(framed, h3::kFrameData, h3::close_session_capsule(0, {}));
      stream->out.append(std::move(framed));
      stream->out.fin = true;
    }
    arm_linger();
    schedule_write();
  }

  void arm_linger()
  {
    linger_.expires_after(kLinger);
    linger_.async_wait(
        [self = shared_from_this()](boost::system::error_code ec)
        {
          if( ec || self->dead_ )
          {
            return;
          }
          if( self->flushing_ )
          {
            self->flushing_ = false;
            self->send_close_capsule();
            return;
          }
          self->close_connection(h3::kNoError);
        });
  }

  void schedule_write()
  {
    if( write_posted_ || dead_ )
    {
      return;
    }
    write_posted_ = true;
    boost::asio::post(server_->strand,
                      [self = shared_from_this()]
                      {
                        self->write_posted_ = false;
                        if( !self->dead_ )
                        {
                          self->write();
                        }
                      });
  }

  void write()
  {
    if( dead_ )
    {
      return;
    }
    std::array<std::uint8_t, kPacketBytes> packet{};
    std::unordered_set<std::int64_t> blocked;
    int datagram_tries = 0;
    const auto ts = timestamp();
    for( int packets = 0;; )
    {
      if( packets == kMaxPacketsPerWrite )
      {
        schedule_write();
        break;
      }
      ngtcp2_path_storage ps;
      ngtcp2_path_storage_zero(&ps);
      ngtcp2_pkt_info info{};
      ngtcp2_ssize nwrite = 0;

      if( !datagrams_out_.empty() && datagram_tries < 2 )
      {
        ngtcp2_vec vec{datagrams_out_.front().data(), datagrams_out_.front().size()};
        int accepted = 0;
        nwrite = ngtcp2_conn_writev_datagram(conn_, &ps.path, &info, packet.data(),
                                             packet.size(), &accepted,
                                             NGTCP2_WRITE_DATAGRAM_FLAG_MORE, 0, &vec, 1, ts);
        if( accepted )
        {
          datagrams_out_.pop_front();
          datagram_tries = 0;
        }
        else
        {
          ++datagram_tries;
        }
        if( nwrite == NGTCP2_ERR_WRITE_MORE )
        {
          continue;
        }
        if( nwrite == NGTCP2_ERR_INVALID_ARGUMENT )
        {
          if( !accepted && !datagrams_out_.empty() )
          {
            datagrams_out_.pop_front();
          }
          datagram_tries = 0;
          continue;
        }
        if( nwrite < 0 )
        {
          fail(static_cast<int>(nwrite));
          return;
        }
        if( nwrite == 0 )
        {
          if( datagram_tries >= 2 && !datagrams_out_.empty() )
          {
            datagrams_out_.pop_front();
            datagram_tries = 0;
          }
          if( datagrams_out_.empty() )
          {
            continue;
          }
          break;
        }
        server_->send_to(endpoint_of(ps.path.remote), packet.data(),
                         static_cast<std::size_t>(nwrite));
        ++packets;
        continue;
      }
      if( datagram_tries >= 2 && !datagrams_out_.empty() )
      {
        datagrams_out_.pop_front();
        datagram_tries = 0;
      }

      std::int64_t id = -1;
      Stream* stream = nullptr;
      for( auto& [sid, candidate] : streams_ )
      {
        if( candidate.out.has_unsent() && !blocked.contains(sid) )
        {
          id = sid;
          stream = &candidate;
          break;
        }
      }
      std::array<ngtcp2_vec, 16> vecs{};
      std::size_t count = 0;
      std::uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
      if( stream )
      {
        count = stream->out.gather(vecs.data(), vecs.size());
        std::uint64_t gathered = 0;
        for( std::size_t i = 0; i < count; ++i )
        {
          gathered += vecs[i].len;
        }
        if( stream->out.fin && stream->out.sent + gathered == stream->out.total )
        {
          flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
        }
      }
      ngtcp2_ssize consumed = -1;
      nwrite = ngtcp2_conn_writev_stream(conn_, &ps.path, &info, packet.data(), packet.size(),
                                         &consumed, flags, id, vecs.data(), count, ts);
      if( nwrite < 0 )
      {
        if( nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED )
        {
          blocked.insert(id);
          continue;
        }
        if( nwrite == NGTCP2_ERR_STREAM_SHUT_WR || nwrite == NGTCP2_ERR_STREAM_NOT_FOUND )
        {
          if( stream )
          {
            stream->out.abandon();
          }
          continue;
        }
        if( nwrite == NGTCP2_ERR_WRITE_MORE )
        {
          account(stream, consumed, flags);
          continue;
        }
        fail(static_cast<int>(nwrite));
        return;
      }
      account(stream, consumed, flags);
      if( nwrite == 0 )
      {
        break;
      }
      server_->send_to(endpoint_of(ps.path.remote), packet.data(),
                       static_cast<std::size_t>(nwrite));
      ++packets;
    }
    ngtcp2_conn_update_pkt_tx_time(conn_, ts);
    arm_timer();
  }

  static void account(Stream* stream, ngtcp2_ssize consumed, std::uint32_t flags)
  {
    if( !stream || consumed < 0 )
    {
      return;
    }
    stream->out.sent += static_cast<std::uint64_t>(consumed);
    if( (flags & NGTCP2_WRITE_STREAM_FLAG_FIN) && stream->out.sent == stream->out.total )
    {
      stream->out.fin_sent = true;
    }
  }

  void arm_timer()
  {
    const auto expiry = ngtcp2_conn_get_expiry2(conn_);
    if( expiry == UINT64_MAX )
    {
      timer_.cancel();
      return;
    }
    const auto now = timestamp();
    const auto delay = expiry > now ? expiry - now : 0;
    timer_.expires_after(std::chrono::nanoseconds(delay));
    timer_.async_wait(
        [self = shared_from_this()](boost::system::error_code ec)
        {
          if( ec || self->dead_ )
          {
            return;
          }
          const int rv = ngtcp2_conn_handle_expiry(self->conn_, timestamp());
          if( rv != 0 )
          {
            if( rv == NGTCP2_ERR_IDLE_CLOSE )
            {
              self->remove();
              return;
            }
            self->fail(rv);
            return;
          }
          self->write();
        });
  }

  void send_close(const ngtcp2_ccerr& error)
  {
    std::array<std::uint8_t, kPacketBytes> packet{};
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info info{};
    const auto nwrite = ngtcp2_conn_write_connection_close(conn_, &ps.path, &info, packet.data(),
                                                           packet.size(), &error, timestamp());
    if( nwrite > 0 )
    {
      server_->send_to(endpoint_of(ps.path.remote), packet.data(),
                       static_cast<std::size_t>(nwrite));
    }
  }

  void fail(int liberr)
  {
    if( dead_ )
    {
      return;
    }
    if( !ngtcp2_conn_in_closing_period2(conn_) && !ngtcp2_conn_in_draining_period2(conn_) )
    {
      ngtcp2_ccerr error;
      ngtcp2_ccerr_default(&error);
      if( liberr == NGTCP2_ERR_CRYPTO )
      {
        ngtcp2_ccerr_set_tls_alert(&error, ngtcp2_conn_get_tls_alert2(conn_), nullptr, 0);
      }
      else
      {
        ngtcp2_ccerr_set_liberr(&error, liberr, nullptr, 0);
      }
      send_close(error);
    }
    remove();
  }

  std::shared_ptr<Impl> server_;
  ngtcp2_conn* conn_ = nullptr;
  SSL* ssl_ = nullptr;
  ngtcp2_crypto_ossl_ctx* ossl_ = nullptr;
  ngtcp2_crypto_conn_ref ref_{};
  boost::asio::steady_timer timer_;
  boost::asio::steady_timer linger_;
  std::map<std::int64_t, Stream> streams_;
  std::vector<std::int64_t> dirty_;
  std::vector<Bytes> datagrams_in_;
  std::deque<Bytes> datagrams_out_;
  std::vector<Bytes> pending_control_;
  Bytes capsules_;
  h3::Qpack qpack_;
  std::vector<std::string> cids_;
  std::shared_ptr<WtSession> session_;
  std::int64_t session_id_ = -1;
  std::int64_t control_id_ = -1;
  bool local_streams_ = false;
  bool write_posted_ = false;
  bool closing_session_ = false;
  bool flushing_ = false;
  bool dead_ = false;
};

void WtServer::Impl::receive()
{
  socket.async_receive_from(
      boost::asio::buffer(buffer), from,
      boost::asio::bind_executor(strand,
                                 [self = shared_from_this()](boost::system::error_code ec,
                                                             std::size_t n)
                                 {
                                   if( self->stopped )
                                   {
                                     return;
                                   }
                                   if( !ec )
                                   {
                                     self->on_datagram(n);
                                   }
                                   if( ec == boost::asio::error::operation_aborted ||
                                       ec == boost::asio::error::bad_descriptor )
                                   {
                                     return;
                                   }
                                   self->receive();
                                 }));
}

void WtServer::Impl::on_datagram(std::size_t n)
{
  const std::uint8_t* data = buffer.data();
  ngtcp2_version_cid vc{};
  const int rv = ngtcp2_pkt_decode_version_cid(&vc, data, n, kScidLen);
  if( rv == NGTCP2_ERR_VERSION_NEGOTIATION )
  {
    std::array<std::uint8_t, kPacketBytes> packet{};
    std::uint8_t unused = 0;
    random_bytes(std::span<std::uint8_t>(&unused, 1));
    const std::uint32_t versions[] = {NGTCP2_PROTO_VER_V1};
    const auto nwrite = ngtcp2_pkt_write_version_negotiation(
        packet.data(), packet.size(), unused, vc.scid, vc.scidlen, vc.dcid, vc.dcidlen, versions,
        1);
    if( nwrite > 0 )
    {
      send_to(from, packet.data(), static_cast<std::size_t>(nwrite));
    }
    return;
  }
  if( rv != 0 )
  {
    return;
  }
  const auto remote = from;
  const auto found = by_cid.find(cid_key(vc.dcid, vc.dcidlen));
  if( found != by_cid.end() )
  {
    auto connection = found->second;
    connection->on_packet(remote, std::span<const std::uint8_t>(data, n));
    return;
  }
  ngtcp2_pkt_hd hd{};
  if( ngtcp2_accept(&hd, data, n) != 0 )
  {
    return;
  }
  if( handlers.admit && !handlers.admit() )
  {
    return;
  }
  auto connection = std::make_shared<WtConnection>(shared_from_this());
  connections.insert(connection);
  if( !connection->init(hd, remote) )
  {
    connection->remove();
    return;
  }
  connection->on_packet(remote, std::span<const std::uint8_t>(data, n));
}

void WtServer::Impl::send_to(const udp::endpoint& to, const std::uint8_t* data, std::size_t n)
{
  boost::system::error_code ec;
  socket.send_to(boost::asio::buffer(data, n), to, 0, ec);
}

void WtServer::Impl::forget(WtConnection& connection)
{
  for( const auto& key : connection.cids() )
  {
    const auto it = by_cid.find(key);
    if( it != by_cid.end() && it->second.get() == &connection )
    {
      by_cid.erase(it);
    }
  }
  for( auto it = connections.begin(); it != connections.end(); ++it )
  {
    if( it->get() == &connection )
    {
      connections.erase(it);
      break;
    }
  }
}

WtSession::WtSession(Strand strand, std::weak_ptr<WtConnection> connection,
                     std::size_t max_outbound, OnEnded on_ended)
    : strand_(std::move(strand)), connection_(std::move(connection)),
      max_outbound_(max_outbound), on_ended_(std::move(on_ended))
{
}

void WtSession::send(const nlohmann::json& message, bool then_close)
{
  if( ended_ )
  {
    return;
  }
  auto connection = connection_.lock();
  if( !connection )
  {
    return;
  }
  Bytes framed;
  h3::put_message(framed, message.dump());
  if( connection->control_unacked() + framed.size() > max_outbound_ )
  {
    end(Ended::Peer);
    return;
  }
  connection->write_control(std::move(framed));
  if( then_close )
  {
    end(Ended::Server);
  }
}

void WtSession::probe(std::chrono::milliseconds timeout,
                      std::function<void(std::optional<int>)> cb)
{
  if( probing_ )
  {
    cb(last_rtt_);
    return;
  }
  if( ended_ )
  {
    cb(std::nullopt);
    return;
  }
  probing_ = true;
  probe_id_ = random_hex(8);
  probe_started_ = std::chrono::steady_clock::now();
  probe_cb_ = std::move(cb);
  probe_timer_.emplace(strand_);
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
  send({{"op", "ping"}, {"id", probe_id_}}, false);
}

bool WtSession::take_pong(const nlohmann::json& message)
{
  if( !message.is_object() || message.value("op", "") != "pong" )
  {
    return false;
  }
  if( probing_ && message.contains("id") && message["id"].is_string() &&
      message["id"].get<std::string>() == probe_id_ )
  {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - probe_started_)
                        .count();
    finish_probe(static_cast<int>(ms));
  }
  return true;
}

void WtSession::finish_probe(std::optional<int> rtt)
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

void WtSession::send_datagram(std::span<const std::uint8_t> datagram)
{
  if( ended_ )
  {
    return;
  }
  if( auto connection = connection_.lock() )
  {
    connection->queue_datagram(datagram);
  }
}

void WtSession::end(Ended how)
{
  if( ended_ )
  {
    return;
  }
  ended_ = true;
  auto self = shared_from_this();
  finish_probe(std::nullopt);
  if( auto connection = connection_.lock() )
  {
    connection->close_session(how == Ended::Server);
  }
  if( on_ended_ )
  {
    on_ended_(self, how);
  }
}

WtServer::WtServer(boost::asio::io_context& ioc, Strand strand, WtHandlers handlers)
    : impl_(std::make_shared<Impl>(ioc, std::move(strand), std::move(handlers)))
{
}

WtServer::~WtServer()
{
  stop();
}

void wt_crypto_init()
{
  static std::once_flag once;
  std::call_once(once,
                 []
                 {
                   ngtcp2_crypto_ossl_init();
                 });
}

void WtServer::start(const WtConfig& config)
{
  impl_->config = config;
  wt_crypto_init();
  auto* ctx = SSL_CTX_new(TLS_server_method());
  if( !ctx )
  {
    throw std::runtime_error("SSL_CTX_new failed for WebTransport");
  }
  impl_->ssl_ctx = ctx;
  SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);
  SSL_CTX_set_options(ctx, (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                               SSL_OP_SINGLE_ECDH_USE | SSL_OP_CIPHER_SERVER_PREFERENCE |
                               SSL_OP_NO_ANTI_REPLAY);
  SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
  SSL_CTX_set_alpn_select_cb(ctx, alpn_select, nullptr);
  if( SSL_CTX_use_certificate_chain_file(ctx, config.cert_file.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(ctx) != 1 )
  {
    throw std::runtime_error("failed to load the WebTransport certificate or key");
  }
  random_bytes(impl_->reset_secret);

  const auto parsed = parse_host_port(config.listen);
  if( !parsed )
  {
    throw std::runtime_error("invalid wt_listen: " + config.listen);
  }
  const udp::endpoint endpoint(boost::asio::ip::make_address(parsed->first), parsed->second);
  impl_->socket.open(endpoint.protocol());
  impl_->socket.bind(endpoint);
  impl_->local = impl_->socket.local_endpoint();
  boost::asio::dispatch(impl_->strand,
                        [impl = impl_]
                        {
                          impl->receive();
                        });
}

void WtServer::stop()
{
  auto impl = impl_;
  if( !impl || impl->stopped )
  {
    return;
  }
  impl->stopped = true;
  boost::system::error_code ec;
  impl->socket.close(ec);
  auto connections = std::move(impl->connections);
  impl->connections.clear();
  impl->by_cid.clear();
  connections.clear();
}

std::uint16_t WtServer::port() const
{
  return impl_->local.port();
}

} // namespace matchmaker
