#pragma once

#include "crypto.hpp"
#include "types.hpp"

#include <filesystem>

namespace matchmaker::test {

inline std::filesystem::path make_temp_dir() {
  auto path = std::filesystem::temp_directory_path() / ("matchmaker-" + uuid_v4());
  std::filesystem::create_directories(path);
  return path;
}

inline GameRecord sample_game(std::string id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", std::string token = "secret") {
  GameRecord game;
  game.id = std::move(id);
  game.name = "Test";
  game.token_hash = sha256_hex(token);
  game.promote_host_on_leave = false;
  game.allow_join_in_progress = false;
  game.limits.max_players_per_lobby = 4;
  game.limits.max_lobbies = 20;
  game.modes.push_back(Mode{"duel", 2});
  return game;
}

}  // namespace matchmaker::test
