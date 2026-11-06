#include "store.hpp"

#include "util.hpp"

#include <nlohmann/json.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace matchmaker {
namespace {

[[noreturn]] void fail_sql(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "sqlite";
  throw std::runtime_error(what + ": " + (msg ? msg : ""));
}

Limits parse_limits(const std::string& text) {
  Limits limits;
  if (text.empty()) {
    return limits;
  }
  auto json = nlohmann::json::parse(text, nullptr, false);
  if (!json.is_object()) {
    return limits;
  }
  auto set_int = [&](const char* key, int& dest) {
    if (json.contains(key) && json[key].is_number_integer()) {
      dest = json[key].get<int>();
    }
  };
  set_int("max_players_per_lobby", limits.max_players_per_lobby);
  set_int("max_password_length", limits.max_password_length);
  set_int("max_concurrent_channels", limits.max_concurrent_channels);
  set_int("max_channels_per_lobby", limits.max_channels_per_lobby);
  set_int("max_channel_lifetime_sec", limits.max_channel_lifetime_sec);
  set_int("max_idle_sec", limits.max_idle_sec);
  set_int("max_lobbies", limits.max_lobbies);
  set_int("max_metadata_bytes", limits.max_metadata_bytes);
  set_int("max_chat_bytes", limits.max_chat_bytes);
  set_int("max_custom_bytes", limits.max_custom_bytes);
  set_int("max_alias_bytes", limits.max_alias_bytes);
  if (json.contains("allow_tcp") && json["allow_tcp"].is_boolean()) {
    limits.allow_tcp = json["allow_tcp"].get<bool>();
  }
  return limits;
}

std::string limits_json(const Limits& limits) {
  nlohmann::json json = {{"max_players_per_lobby", limits.max_players_per_lobby},
                         {"max_password_length", limits.max_password_length},
                         {"max_concurrent_channels", limits.max_concurrent_channels},
                         {"max_channels_per_lobby", limits.max_channels_per_lobby},
                         {"max_channel_lifetime_sec", limits.max_channel_lifetime_sec},
                         {"max_idle_sec", limits.max_idle_sec},
                         {"allow_tcp", limits.allow_tcp},
                         {"max_lobbies", limits.max_lobbies},
                         {"max_metadata_bytes", limits.max_metadata_bytes},
                         {"max_chat_bytes", limits.max_chat_bytes},
                         {"max_custom_bytes", limits.max_custom_bytes},
                         {"max_alias_bytes", limits.max_alias_bytes}};
  return json.dump();
}

class SqliteStore final : public Store {
 public:
  explicit SqliteStore(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
      fail_sql(db_, "open");
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA busy_timeout=5000;");
  }

  ~SqliteStore() override {
    if (db_) {
      sqlite3_close(db_);
    }
  }

  void migrate() override {
    exec("CREATE TABLE IF NOT EXISTS schema_migrations (version TEXT PRIMARY KEY, applied_at_ms INTEGER NOT NULL)");
    const auto dir = migrations_dir();
    if (!std::filesystem::exists(dir)) {
      throw std::runtime_error("migrations directory not found: " + dir.string());
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".sql") {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
      const auto version = file.filename().string();
      if (applied(version)) {
        continue;
      }
      std::ifstream in(file);
      if (!in) {
        throw std::runtime_error("cannot read migration " + file.string());
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      exec("BEGIN");
      try {
        exec(buffer.str());
        bind_exec("INSERT INTO schema_migrations(version, applied_at_ms) VALUES(?, ?)", version, wall_ms());
        exec("COMMIT");
      } catch (...) {
        exec("ROLLBACK");
        throw;
      }
    }
  }

  std::map<std::string, std::string> load_config() override {
    std::map<std::string, std::string> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT key, value FROM config", -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, "config");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      out.emplace(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                  reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return out;
  }

  void set_config(const std::string& key, const std::string& value) override {
    bind_exec("INSERT INTO config(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", key,
              value);
  }

  std::vector<Region> load_regions() override {
    std::vector<Region> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, public_url, sort_order FROM regions ORDER BY sort_order, id", -1, &stmt,
                           nullptr) != SQLITE_OK) {
      fail_sql(db_, "regions");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      Region region;
      region.id = col_text(stmt, 0);
      region.public_url = col_text(stmt, 1);
      region.sort_order = sqlite3_column_int(stmt, 2);
      out.push_back(std::move(region));
    }
    sqlite3_finalize(stmt);
    return out;
  }

  std::vector<GameRecord> load_games() override {
    std::vector<GameRecord> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id, name, token_hash, promote_host_on_leave, allow_join_in_progress, limits_json, "
                           "created_at_ms FROM games ORDER BY created_at_ms, id",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, "games");
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      GameRecord game;
      game.id = col_text(stmt, 0);
      game.name = col_text(stmt, 1);
      game.token_hash = col_text(stmt, 2);
      game.promote_host_on_leave = sqlite3_column_int(stmt, 3) != 0;
      game.allow_join_in_progress = sqlite3_column_int(stmt, 4) != 0;
      game.limits = parse_limits(col_text(stmt, 5));
      game.created_at_ms = sqlite3_column_int64(stmt, 6);
      out.push_back(std::move(game));
    }
    sqlite3_finalize(stmt);
    for (auto& game : out) {
      sqlite3_stmt* modes = nullptr;
      if (sqlite3_prepare_v2(db_, "SELECT name, players FROM game_modes WHERE game_id=? ORDER BY name", -1, &modes,
                             nullptr) != SQLITE_OK) {
        fail_sql(db_, "modes");
      }
      sqlite3_bind_text(modes, 1, game.id.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(modes) == SQLITE_ROW) {
        Mode mode;
        mode.name = col_text(modes, 0);
        mode.players = sqlite3_column_int(modes, 1);
        if (mode.players >= 1) {
          game.modes.push_back(std::move(mode));
        }
      }
      sqlite3_finalize(modes);
    }
    return out;
  }

  void upsert_game(const GameRecord& game) override {
    if (game.id.empty() || game.name.empty() || game.token_hash.empty()) {
      throw std::runtime_error("game id, name, and token hash are required");
    }
    exec("BEGIN");
    try {
      sqlite3_stmt* stmt = nullptr;
      const char* sql =
          "INSERT INTO games(id, name, token_hash, promote_host_on_leave, allow_join_in_progress, limits_json, "
          "created_at_ms) VALUES(?,?,?,?,?,?,?) "
          "ON CONFLICT(id) DO UPDATE SET name=excluded.name, token_hash=excluded.token_hash, "
          "promote_host_on_leave=excluded.promote_host_on_leave, "
          "allow_join_in_progress=excluded.allow_join_in_progress, limits_json=excluded.limits_json";
      if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        fail_sql(db_, "upsert game");
      }
      sqlite3_bind_text(stmt, 1, game.id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, game.name.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, game.token_hash.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 4, game.promote_host_on_leave ? 1 : 0);
      sqlite3_bind_int(stmt, 5, game.allow_join_in_progress ? 1 : 0);
      const auto limits = limits_json(game.limits);
      sqlite3_bind_text(stmt, 6, limits.c_str(), -1, SQLITE_TRANSIENT);
      const auto created = game.created_at_ms == 0 ? wall_ms() : game.created_at_ms;
      sqlite3_bind_int64(stmt, 7, created);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        fail_sql(db_, "upsert game step");
      }
      sqlite3_finalize(stmt);
      bind_exec("DELETE FROM game_modes WHERE game_id=?", game.id);
      for (const auto& mode : game.modes) {
        if (mode.name.empty() || mode.players < 1) {
          throw std::runtime_error("invalid mode");
        }
        sqlite3_stmt* ms = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT INTO game_modes(game_id, name, players) VALUES(?,?,?)", -1, &ms, nullptr) !=
            SQLITE_OK) {
          fail_sql(db_, "insert mode");
        }
        sqlite3_bind_text(ms, 1, game.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ms, 2, mode.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ms, 3, mode.players);
        if (sqlite3_step(ms) != SQLITE_DONE) {
          sqlite3_finalize(ms);
          fail_sql(db_, "insert mode step");
        }
        sqlite3_finalize(ms);
      }
      exec("COMMIT");
    } catch (...) {
      exec("ROLLBACK");
      throw;
    }
  }

  void insert_log(const LogRecord& row) override {
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO logs(id, ts_ms, level, category, message, game_id, detail_json) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, "insert log");
    }
    sqlite3_bind_text(stmt, 1, row.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, row.ts_ms);
    sqlite3_bind_text(stmt, 3, row.level.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, row.category.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, row.message.c_str(), -1, SQLITE_TRANSIENT);
    if (row.game_id.empty()) {
      sqlite3_bind_null(stmt, 6);
    } else {
      sqlite3_bind_text(stmt, 6, row.game_id.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (row.detail_json.empty()) {
      sqlite3_bind_null(stmt, 7);
    } else {
      sqlite3_bind_text(stmt, 7, row.detail_json.c_str(), -1, SQLITE_TRANSIENT);
    }
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      fail_sql(db_, "insert log step");
    }
  }

  void prune_logs(std::int64_t older_than_ms) override {
    bind_exec("DELETE FROM logs WHERE ts_ms < ?", older_than_ms);
  }

  std::vector<LogRecord> recent_logs(int limit) override {
    std::vector<LogRecord> out;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT id, ts_ms, level, category, message, game_id, detail_json FROM logs "
                           "ORDER BY ts_ms DESC LIMIT ?",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, "logs");
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      LogRecord row;
      row.id = col_text(stmt, 0);
      row.ts_ms = sqlite3_column_int64(stmt, 1);
      row.level = col_text(stmt, 2);
      row.category = col_text(stmt, 3);
      row.message = col_text(stmt, 4);
      row.game_id = col_text(stmt, 5);
      row.detail_json = col_text(stmt, 6);
      out.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return out;
  }

 private:
  static std::string col_text(sqlite3_stmt* stmt, int col) {
    const auto* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
  }

  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::string message = err ? err : "sqlite error";
      sqlite3_free(err);
      throw std::runtime_error(message);
    }
  }

  bool applied(const std::string& version) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM schema_migrations WHERE version=?", -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, "migration lookup");
    }
    sqlite3_bind_text(stmt, 1, version.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return found;
  }

  void bind_exec(const char* sql, const std::string& a, std::int64_t b) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, sql);
    }
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, b);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      fail_sql(db_, sql);
    }
  }

  void bind_exec(const char* sql, const std::string& a, const std::string& b) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, sql);
    }
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, b.c_str(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      fail_sql(db_, sql);
    }
  }

  void bind_exec(const char* sql, const std::string& a) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, sql);
    }
    sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      fail_sql(db_, sql);
    }
  }

  void bind_exec(const char* sql, std::int64_t a) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      fail_sql(db_, sql);
    }
    sqlite3_bind_int64(stmt, 1, a);
    const auto rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
      fail_sql(db_, sql);
    }
  }

  sqlite3* db_ = nullptr;
};

}  // namespace

std::unique_ptr<Store> open_sqlite_store(const std::string& path) { return std::make_unique<SqliteStore>(path); }

}  // namespace matchmaker
