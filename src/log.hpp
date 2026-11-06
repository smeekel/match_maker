#pragma once

#include "store.hpp"

#include <string>

namespace matchmaker {

enum class LogLevel { Info, Warn, Error };

void write_log(Store* store, LogLevel level, std::string category, std::string message,
               std::string game_id = {}, std::string detail_json = {});

}  // namespace matchmaker
