#pragma once

#include "types.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace matchmaker
{

class Store
{
public:
  virtual ~Store() = default;
  virtual void migrate() = 0;
  virtual std::map<std::string, std::string> load_config() = 0;
  virtual void set_config(const std::string& key, const std::string& value) = 0;
  virtual std::vector<Region> load_regions() = 0;
  virtual std::vector<GameRecord> load_games() = 0;
  virtual void upsert_game(const GameRecord& game) = 0;
  // Returns false when no row had that id.
  virtual bool delete_game(const std::string& id) = 0;
  virtual void upsert_region(const Region& region) = 0;
  virtual bool delete_region(const std::string& id) = 0;
  virtual void insert_log(const LogRecord& row) = 0;
  virtual void prune_logs(std::int64_t older_than_ms) = 0;
  virtual std::vector<LogRecord> recent_logs(int limit) = 0;
};

std::unique_ptr<Store> open_sqlite_store(const std::string& path);

} // namespace matchmaker
