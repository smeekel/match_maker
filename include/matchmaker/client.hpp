#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace matchmaker {

struct ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 443;
  bool insecure = true;
  std::chrono::milliseconds timeout{5000};
};

struct HttpResult {
  int status = 0;
  nlohmann::json body = nlohmann::json::object();
  std::string raw_body;
  std::string content_encoding;
  std::string error;
};

class Client {
 public:
  explicit Client(ClientOptions options);
  ~Client();
  Client(Client&&) noexcept;
  Client& operator=(Client&&) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  HttpResult request(const std::string& method, const std::string& target, const nlohmann::json& body = {},
                     const std::string& bearer = {}, bool accept_gzip = true);
  void ws_connect(const std::string& lobby_id, const std::string& bearer);
  void ws_send(const nlohmann::json& message);
  nlohmann::json ws_read(std::chrono::milliseconds timeout);
  void ws_close();
  std::string ws_offered_extensions() const;
  bool ws_open() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace matchmaker
