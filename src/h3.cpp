#include "h3.hpp"

#include <nghttp3/nghttp3.h>

#include <stdexcept>

namespace matchmaker::h3
{

namespace
{

constexpr std::uint64_t kSettingEnableConnectProtocol = 0x08;
constexpr std::uint64_t kSettingH3Datagram = 0x33;
constexpr std::uint64_t kSettingH3DatagramDraft04 = 0xffd277;
constexpr std::uint64_t kSettingEnableWebTransportDraft02 = 0x2b603742;
constexpr std::uint64_t kSettingWebTransportMaxSessionsDraft07 = 0xc671706a;
constexpr std::uint64_t kSettingWtMaxSessions = 0x14e9cd29;

} // namespace

void put_varint(Bytes& out, std::uint64_t value)
{
  if( value < (1ULL << 6) )
  {
    out.push_back(static_cast<std::uint8_t>(value));
  }
  else if( value < (1ULL << 14) )
  {
    out.push_back(static_cast<std::uint8_t>(0x40 | (value >> 8)));
    out.push_back(static_cast<std::uint8_t>(value));
  }
  else if( value < (1ULL << 30) )
  {
    out.push_back(static_cast<std::uint8_t>(0x80 | (value >> 24)));
    for( int shift = 16; shift >= 0; shift -= 8 )
    {
      out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }
  else
  {
    out.push_back(static_cast<std::uint8_t>(0xc0 | (value >> 56)));
    for( int shift = 48; shift >= 0; shift -= 8 )
    {
      out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }
}

std::optional<std::pair<std::uint64_t, std::size_t>> get_varint(std::span<const std::uint8_t> in)
{
  if( in.empty() )
  {
    return std::nullopt;
  }
  const std::size_t len = std::size_t{1} << (in[0] >> 6);
  if( in.size() < len )
  {
    return std::nullopt;
  }
  std::uint64_t value = in[0] & 0x3f;
  for( std::size_t i = 1; i < len; ++i )
  {
    value = (value << 8) | in[i];
  }
  return std::make_pair(value, len);
}

void put_frame(Bytes& out, std::uint64_t type, std::span<const std::uint8_t> payload)
{
  put_varint(out, type);
  put_varint(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
}

std::optional<Frame> get_frame(std::span<const std::uint8_t> in)
{
  const auto type = get_varint(in);
  if( !type )
  {
    return std::nullopt;
  }
  const auto length = get_varint(in.subspan(type->second));
  if( !length )
  {
    return std::nullopt;
  }
  const std::size_t header = type->second + length->second;
  if( in.size() - header < length->first )
  {
    return std::nullopt;
  }
  Frame frame;
  frame.type = type->first;
  frame.payload = in.subspan(header, static_cast<std::size_t>(length->first));
  frame.size = header + static_cast<std::size_t>(length->first);
  return frame;
}

Bytes control_stream_preamble(bool server)
{
  Bytes settings;
  auto setting = [&](std::uint64_t id, std::uint64_t value)
  {
    put_varint(settings, id);
    put_varint(settings, value);
  };
  if( server )
  {
    setting(kSettingEnableConnectProtocol, 1);
  }
  setting(kSettingH3Datagram, 1);
  setting(kSettingH3DatagramDraft04, 1);
  setting(kSettingEnableWebTransportDraft02, 1);
  setting(kSettingWebTransportMaxSessionsDraft07, 1);
  setting(kSettingWtMaxSessions, 1);
  Bytes out;
  put_varint(out, kStreamControl);
  put_frame(out, kFrameSettings, settings);
  return out;
}

Bytes close_session_capsule(std::uint32_t code, std::string_view reason)
{
  Bytes out;
  put_varint(out, kCapsuleCloseSession);
  put_varint(out, 4 + reason.size());
  for( int shift = 24; shift >= 0; shift -= 8 )
  {
    out.push_back(static_cast<std::uint8_t>(code >> shift));
  }
  out.insert(out.end(), reason.begin(), reason.end());
  return out;
}

void put_message(Bytes& out, std::string_view text)
{
  const auto n = static_cast<std::uint32_t>(text.size());
  for( int shift = 24; shift >= 0; shift -= 8 )
  {
    out.push_back(static_cast<std::uint8_t>(n >> shift));
  }
  out.insert(out.end(), text.begin(), text.end());
}

TakeResult take_message(Bytes& buffer, std::size_t max_bytes, std::string& text)
{
  if( buffer.size() < 4 )
  {
    return TakeResult::Incomplete;
  }
  const std::size_t n = (std::size_t{buffer[0]} << 24) | (std::size_t{buffer[1]} << 16) |
                        (std::size_t{buffer[2]} << 8) | std::size_t{buffer[3]};
  if( n > max_bytes )
  {
    return TakeResult::TooLarge;
  }
  if( buffer.size() < 4 + n )
  {
    return TakeResult::Incomplete;
  }
  text.assign(reinterpret_cast<const char*>(buffer.data()) + 4, n);
  buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(4 + n));
  return TakeResult::Message;
}

std::string_view find_header(const Headers& headers, std::string_view name)
{
  for( const auto& [key, value] : headers )
  {
    if( key == name )
    {
      return value;
    }
  }
  return {};
}

struct Qpack::Impl
{
  nghttp3_qpack_encoder* encoder = nullptr;
  nghttp3_qpack_decoder* decoder = nullptr;
};

Qpack::Qpack() : impl_(std::make_unique<Impl>())
{
  const auto* mem = nghttp3_mem_default();
  if( nghttp3_qpack_encoder_new(&impl_->encoder, 0, mem) != 0 ||
      nghttp3_qpack_decoder_new(&impl_->decoder, 0, 0, mem) != 0 )
  {
    throw std::runtime_error("qpack init failed");
  }
}

Qpack::~Qpack()
{
  nghttp3_qpack_encoder_del(impl_->encoder);
  nghttp3_qpack_decoder_del(impl_->decoder);
}

Bytes Qpack::encode(std::int64_t stream_id, const Headers& headers)
{
  std::vector<nghttp3_nv> nva;
  nva.reserve(headers.size());
  for( const auto& [name, value] : headers )
  {
    nva.push_back(nghttp3_nv{reinterpret_cast<const std::uint8_t*>(name.data()),
                             reinterpret_cast<const std::uint8_t*>(value.data()), name.size(),
                             value.size(), NGHTTP3_NV_FLAG_NONE});
  }
  const auto* mem = nghttp3_mem_default();
  nghttp3_buf prefix;
  nghttp3_buf body;
  nghttp3_buf encoder_stream;
  nghttp3_buf_init(&prefix);
  nghttp3_buf_init(&body);
  nghttp3_buf_init(&encoder_stream);
  const int rv = nghttp3_qpack_encoder_encode(impl_->encoder, &prefix, &body, &encoder_stream,
                                              stream_id, nva.data(), nva.size());
  Bytes out;
  if( rv == 0 )
  {
    out.assign(prefix.pos, prefix.last);
    out.insert(out.end(), body.pos, body.last);
  }
  nghttp3_buf_free(&prefix, mem);
  nghttp3_buf_free(&body, mem);
  nghttp3_buf_free(&encoder_stream, mem);
  if( rv != 0 )
  {
    throw std::runtime_error("qpack encode failed");
  }
  return out;
}

std::optional<Headers> Qpack::decode(std::int64_t stream_id, std::span<const std::uint8_t> block)
{
  const auto* mem = nghttp3_mem_default();
  nghttp3_qpack_stream_context* context = nullptr;
  if( nghttp3_qpack_stream_context_new(&context, stream_id, mem) != 0 )
  {
    return std::nullopt;
  }
  Headers headers;
  const std::uint8_t* pos = block.data();
  std::size_t left = block.size();
  bool ok = false;
  for( ;; )
  {
    nghttp3_qpack_nv nv{};
    std::uint8_t flags = NGHTTP3_QPACK_DECODE_FLAG_NONE;
    const auto n =
        nghttp3_qpack_decoder_read_request(impl_->decoder, context, &nv, &flags, pos, left, 1);
    if( n < 0 )
    {
      break;
    }
    pos += n;
    left -= static_cast<std::size_t>(n);
    if( flags & NGHTTP3_QPACK_DECODE_FLAG_EMIT )
    {
      const auto name = nghttp3_rcbuf_get_buf(nv.name);
      const auto value = nghttp3_rcbuf_get_buf(nv.value);
      headers.emplace_back(std::string(reinterpret_cast<const char*>(name.base), name.len),
                           std::string(reinterpret_cast<const char*>(value.base), value.len));
      nghttp3_rcbuf_decref(nv.name);
      nghttp3_rcbuf_decref(nv.value);
    }
    if( flags & NGHTTP3_QPACK_DECODE_FLAG_FINAL )
    {
      ok = true;
      break;
    }
    if( (flags & NGHTTP3_QPACK_DECODE_FLAG_BLOCKED) ||
        (n == 0 && !(flags & NGHTTP3_QPACK_DECODE_FLAG_EMIT)) )
    {
      break;
    }
  }
  nghttp3_qpack_stream_context_del(context);
  if( !ok )
  {
    return std::nullopt;
  }
  return headers;
}

} // namespace matchmaker::h3
