#pragma once

#include "types.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace matchmaker
{

struct SystemConfig
{
  std::string listen = "0.0.0.0:443";
  std::string public_host = "localhost";
  std::string public_url = "https://localhost";
  std::string region_id = "local";
  std::string jwt_secret = "change-me";
  int jwt_ttl_sec = 900;
  int admit_sec = 60;
  int join_hold_sec = 10;
  int host_heartbeat_interval_sec = 2;
  int host_heartbeat_misses = 3;
  int max_threads = 1;
  std::string stats_listen = "127.0.0.1:7711";
  std::string tls_cert;
  std::string tls_key;
  std::string tls_min_version = "1.2";
  bool http_gzip = true;
  int http_gzip_min_bytes = 256;
  bool websocket_permessage_deflate = true;
  int websocket_deflate_level = 3;
  int websocket_max_window_bits = 12;
  int retention_days = 30;
  int max_message_bytes = 65536;
  int max_connections = 10000;
  int max_outbound_queue_bytes = 1048576;
  int max_concurrent_channels = 4096;
  int max_channel_lifetime_sec = 7200;
  int max_idle_sec = 120;
  int max_udp_packet_bytes = 1200;
  int max_bytes_per_sec_per_channel = 0;
  int max_players_per_lobby = 64;
  int max_password_length = 64;
  int max_frame_bytes = 65536;
  std::string relay_bind = "0.0.0.0";
  int relay_port_start = 30000;
  int relay_port_end = 39999;
  bool relay_tcp_enabled = false;
  int auth_fail_limit = 20;
  int auth_fail_window_sec = 60;
  int probe_timeout_ms = 500;

  static SystemConfig from_map(const std::map<std::string, std::string>& values);
};

class Catalog
{
public:
  void replace(SystemConfig system, std::vector<Region> regions, std::vector<GameRecord> games);
  const SystemConfig& system() const
  {
    return system_;
  }
  const std::vector<Region>& regions() const
  {
    return regions_;
  }
  const GameRecord* game(const std::string& id) const;
  std::vector<GameRecord> games() const;

private:
  SystemConfig system_;
  std::vector<Region> regions_;
  std::unordered_map<std::string, GameRecord> games_;
};

void clamp_game(GameRecord& game, const SystemConfig& system);

} // namespace matchmaker
