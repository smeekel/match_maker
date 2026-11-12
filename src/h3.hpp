#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// HTTP/3 and WebTransport wire pieces shared by the WebTransport listener and
// its test peer. QPACK runs with no dynamic table on either side.
namespace matchmaker::h3
{

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kFrameData = 0x00;
constexpr std::uint64_t kFrameHeaders = 0x01;
constexpr std::uint64_t kFrameSettings = 0x04;

constexpr std::uint64_t kStreamControl = 0x00;
constexpr std::uint64_t kStreamQpackEncoder = 0x02;
constexpr std::uint64_t kStreamQpackDecoder = 0x03;
constexpr std::uint64_t kStreamWtUni = 0x54;
constexpr std::uint64_t kSignalWtBidi = 0x41;

constexpr std::uint64_t kCapsuleCloseSession = 0x2843;

constexpr std::uint64_t kNoError = 0x100;
constexpr std::uint64_t kStreamCreationError = 0x103;
constexpr std::uint64_t kRequestRejected = 0x10b;
constexpr std::uint64_t kWtBufferedStreamRejected = 0x3994bd84;

void put_varint(Bytes& out, std::uint64_t value);
// Value and encoded length, or nullopt while the varint is incomplete.
std::optional<std::pair<std::uint64_t, std::size_t>> get_varint(std::span<const std::uint8_t> in);

void put_frame(Bytes& out, std::uint64_t type, std::span<const std::uint8_t> payload);

struct Frame
{
  std::uint64_t type = 0;
  std::span<const std::uint8_t> payload;
  std::size_t size = 0;
};
// One complete frame from the front of |in|, or nullopt while it is incomplete.
std::optional<Frame> get_frame(std::span<const std::uint8_t> in);

// Stream type and SETTINGS frame that open an endpoint's control stream.
Bytes control_stream_preamble(bool server);

Bytes close_session_capsule(std::uint32_t code, std::string_view reason);

// Control messages on the WebTransport lobby stream: big-endian uint32 length,
// then that many bytes of UTF-8 JSON.
void put_message(Bytes& out, std::string_view text);
enum class TakeResult
{
  Message,
  Incomplete,
  TooLarge
};
TakeResult take_message(Bytes& buffer, std::size_t max_bytes, std::string& text);

using Header = std::pair<std::string, std::string>;
using Headers = std::vector<Header>;
std::string_view find_header(const Headers& headers, std::string_view name);

class Qpack
{
public:
  Qpack();
  ~Qpack();
  Qpack(const Qpack&) = delete;
  Qpack& operator=(const Qpack&) = delete;

  Bytes encode(std::int64_t stream_id, const Headers& headers);
  std::optional<Headers> decode(std::int64_t stream_id, std::span<const std::uint8_t> block);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace matchmaker::h3
