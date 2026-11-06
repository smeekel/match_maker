#include "catalog.hpp"

#include <map>

namespace matchmaker
{
namespace
{

int parse_int(const std::map<std::string, std::string>& values, const char* key, int fallback)
{
  const auto it = values.find(key);
  if( it == values.end() )
  {
    return fallback;
  }
  try
  {
    return std::stoi(it->second);
  }
  catch( ... )
  {
    return fallback;
  }
}

bool parse_bool(const std::map<std::string, std::string>& values, const char* key, bool fallback)
{
  const auto it = values.find(key);
  if( it == values.end() )
  {
    return fallback;
  }
  const auto& v = it->second;
  if( v == "1" || v == "true" || v == "TRUE" || v == "yes" )
  {
    return true;
  }
  if( v == "0" || v == "false" || v == "FALSE" || v == "no" )
  {
    return false;
  }
  return fallback;
}

std::string parse_string(const std::map<std::string, std::string>& values, const char* key,
                         std::string fallback)
{
  const auto it = values.find(key);
  if( it == values.end() )
  {
    return fallback;
  }
  return it->second;
}

} // namespace

SystemConfig SystemConfig::from_map(const std::map<std::string, std::string>& values)
{
  SystemConfig cfg;
  cfg.listen = parse_string(values, "listen", cfg.listen);
  cfg.public_host = parse_string(values, "public_host", cfg.public_host);
  cfg.public_url = parse_string(values, "public_url", cfg.public_url);
  cfg.region_id = parse_string(values, "region_id", cfg.region_id);
  cfg.jwt_secret = parse_string(values, "jwt_secret", cfg.jwt_secret);
  cfg.jwt_ttl_sec = parse_int(values, "jwt_ttl_sec", cfg.jwt_ttl_sec);
  cfg.admit_sec = parse_int(values, "admit_sec", cfg.admit_sec);
  cfg.join_hold_sec = parse_int(values, "join_hold_sec", cfg.join_hold_sec);
  cfg.host_heartbeat_interval_sec =
      parse_int(values, "host_heartbeat_interval_sec", cfg.host_heartbeat_interval_sec);
  cfg.host_heartbeat_misses = parse_int(values, "host_heartbeat_misses", cfg.host_heartbeat_misses);
  cfg.max_threads = std::max(1, parse_int(values, "max_threads", cfg.max_threads));
  cfg.stats_listen = parse_string(values, "stats_listen", cfg.stats_listen);
  cfg.tls_cert = parse_string(values, "tls_cert", cfg.tls_cert);
  cfg.tls_key = parse_string(values, "tls_key", cfg.tls_key);
  cfg.tls_min_version = parse_string(values, "tls_min_version", cfg.tls_min_version);
  cfg.http_gzip = parse_bool(values, "http_gzip", cfg.http_gzip);
  cfg.http_gzip_min_bytes = parse_int(values, "http_gzip_min_bytes", cfg.http_gzip_min_bytes);
  cfg.websocket_permessage_deflate =
      parse_bool(values, "websocket_permessage_deflate", cfg.websocket_permessage_deflate);
  cfg.websocket_deflate_level =
      parse_int(values, "websocket_deflate_level", cfg.websocket_deflate_level);
  cfg.websocket_max_window_bits =
      parse_int(values, "websocket_max_window_bits", cfg.websocket_max_window_bits);
  cfg.retention_days = parse_int(values, "retention_days", cfg.retention_days);
  cfg.max_message_bytes = parse_int(values, "max_message_bytes", cfg.max_message_bytes);
  cfg.max_connections = parse_int(values, "max_connections", cfg.max_connections);
  cfg.max_outbound_queue_bytes =
      parse_int(values, "max_outbound_queue_bytes", cfg.max_outbound_queue_bytes);
  cfg.max_concurrent_channels =
      parse_int(values, "max_concurrent_channels", cfg.max_concurrent_channels);
  cfg.max_channel_lifetime_sec =
      parse_int(values, "max_channel_lifetime_sec", cfg.max_channel_lifetime_sec);
  cfg.max_idle_sec = parse_int(values, "max_idle_sec", cfg.max_idle_sec);
  cfg.max_udp_packet_bytes = parse_int(values, "max_udp_packet_bytes", cfg.max_udp_packet_bytes);
  cfg.max_bytes_per_sec_per_channel =
      parse_int(values, "max_bytes_per_sec_per_channel", cfg.max_bytes_per_sec_per_channel);
  cfg.max_players_per_lobby = parse_int(values, "max_players_per_lobby", cfg.max_players_per_lobby);
  cfg.max_password_length = parse_int(values, "max_password_length", cfg.max_password_length);
  cfg.max_frame_bytes = parse_int(values, "max_frame_bytes", cfg.max_frame_bytes);
  cfg.relay_bind = parse_string(values, "relay_bind", cfg.relay_bind);
  cfg.relay_port_start = parse_int(values, "relay_port_start", cfg.relay_port_start);
  cfg.relay_port_end = parse_int(values, "relay_port_end", cfg.relay_port_end);
  cfg.relay_tcp_enabled = parse_bool(values, "relay_tcp_enabled", cfg.relay_tcp_enabled);
  cfg.auth_fail_limit = parse_int(values, "auth_fail_limit", cfg.auth_fail_limit);
  cfg.auth_fail_window_sec = parse_int(values, "auth_fail_window_sec", cfg.auth_fail_window_sec);
  cfg.probe_timeout_ms = parse_int(values, "probe_timeout_ms", cfg.probe_timeout_ms);
  return cfg;
}

void clamp_game(GameRecord& game, const SystemConfig& system)
{
  auto& limits = game.limits;
  limits.max_players_per_lobby =
      std::min(limits.max_players_per_lobby, system.max_players_per_lobby);
  limits.max_password_length = std::min(limits.max_password_length, system.max_password_length);
  limits.max_concurrent_channels =
      std::min(limits.max_concurrent_channels, system.max_concurrent_channels);
  limits.max_channel_lifetime_sec =
      std::min(limits.max_channel_lifetime_sec, system.max_channel_lifetime_sec);
  limits.max_idle_sec = std::min(limits.max_idle_sec, system.max_idle_sec);
  if( !system.relay_tcp_enabled )
  {
    limits.allow_tcp = false;
  }
  if( limits.max_players_per_lobby < 1 )
  {
    limits.max_players_per_lobby = 1;
  }
  if( limits.max_password_length < 1 )
  {
    limits.max_password_length = 1;
  }
}

void Catalog::replace(SystemConfig system, std::vector<Region> regions,
                      std::vector<GameRecord> games)
{
  system_ = std::move(system);
  regions_ = std::move(regions);
  games_.clear();
  for( auto& game : games )
  {
    clamp_game(game, system_);
    games_.emplace(game.id, std::move(game));
  }
}

const GameRecord* Catalog::game(const std::string& id) const
{
  const auto it = games_.find(id);
  if( it == games_.end() )
  {
    return nullptr;
  }
  return &it->second;
}

std::vector<GameRecord> Catalog::games() const
{
  std::vector<GameRecord> out;
  out.reserve(games_.size());
  for( const auto& [_, game] : games_ )
  {
    out.push_back(game);
  }
  return out;
}

} // namespace matchmaker
