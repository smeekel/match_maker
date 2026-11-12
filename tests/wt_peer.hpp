#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace matchmaker::test
{

// Minimal blocking WebTransport client for tests: one QUIC connection, one
// CONNECT, the server-opened control stream, and datagrams.
class WtPeer
{
public:
  explicit WtPeer(std::uint16_t port);
  ~WtPeer();
  WtPeer(const WtPeer&) = delete;
  WtPeer& operator=(const WtPeer&) = delete;

  // Handshakes, sends CONNECT for |path|, and returns the response status, or
  // 0 when no response arrived in time.
  int connect(const std::string& path, const std::string& origin = {},
              std::chrono::milliseconds timeout = std::chrono::seconds(5));
  void send(const nlohmann::json& message);
  std::optional<nlohmann::json> read(std::chrono::milliseconds timeout);
  // Reads control messages until |pred| matches one.
  std::optional<nlohmann::json> read_until(const std::function<bool(const nlohmann::json&)>& pred,
                                           std::chrono::milliseconds timeout);
  // |payload| is everything after the session id: the 16-byte channel id and
  // the game bytes.
  void send_datagram(std::span<const std::uint8_t> payload);
  std::optional<std::vector<std::uint8_t>> read_datagram(std::chrono::milliseconds timeout);
  // True once the server has closed the session or the connection.
  bool session_closed(std::chrono::milliseconds timeout);
  void close_session();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace matchmaker::test
