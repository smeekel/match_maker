#include "log.hpp"

#include "crypto.hpp"
#include "util.hpp"

#include <exception>
#include <iostream>

namespace matchmaker
{

void write_log(Store* store, LogLevel level, std::string category, std::string message,
               std::string game_id, std::string detail_json)
{
  const char* name = level == LogLevel::Error ? "error" : level == LogLevel::Warn ? "warn" : "info";
  std::cout << wall_ms() << ' ' << name << ' ' << category << ' ' << message << '\n';
  std::cout.flush();
  if( !store )
  {
    return;
  }
  LogRecord row;
  row.id = uuid_v4();
  row.ts_ms = wall_ms();
  row.level = name;
  row.category = std::move(category);
  row.message = std::move(message);
  row.game_id = std::move(game_id);
  row.detail_json = std::move(detail_json);
  try
  {
    store->insert_log(row);
  }
  catch( const std::exception& ex )
  {
    std::cerr << "log insert failed: " << ex.what() << '\n';
  }
}

} // namespace matchmaker
