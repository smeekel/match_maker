#pragma once

#include <cstdint>
#include <string>

namespace matchmaker
{

struct Limits
{
  int max_players_per_lobby = 16;
  int max_password_length = 64;
  int max_concurrent_channels = 256;
  int max_channel_lifetime_sec = 3600;
  int max_idle_sec = 60;
  bool allow_tcp = false;
  int max_lobbies = 1000;
  int max_metadata_bytes = 8192;
  int max_chat_bytes = 512;
  int max_custom_bytes = 4096;
  int max_alias_bytes = 32;
};

struct GameRecord
{
  std::string id;
  std::string name;
  std::string token_hash;
  bool promote_host_on_leave = false;
  bool allow_join_in_progress = false;
  Limits limits;
  std::int64_t created_at_ms = 0;
};

struct Region
{
  std::string id;
  std::string public_url;
  int sort_order = 0;
};

struct LogRecord
{
  std::string id;
  std::int64_t ts_ms = 0;
  std::string level;
  std::string category;
  std::string message;
  std::string game_id;
  std::string detail_json;
};

struct Actor
{
  std::string user_id;
  std::string game_id;
  std::string alias;
  std::string external_id;
};

} // namespace matchmaker
