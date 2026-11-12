#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace matchmaker
{

// One member's live lobby connection, whichever transport carries it.
class LobbyLink
{
public:
  virtual ~LobbyLink() = default;
  virtual void send(const nlohmann::json& message, bool then_close) = 0;
  virtual void probe(std::chrono::milliseconds timeout,
                     std::function<void(std::optional<int>)> cb) = 0;
  virtual std::string_view transport() const = 0;
  virtual void send_datagram(std::span<const std::uint8_t> datagram)
  {
    (void)datagram;
  }
};

} // namespace matchmaker
