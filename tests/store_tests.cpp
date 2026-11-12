#include "crypto.hpp"
#include "log.hpp"
#include "store.hpp"

#include "fixtures.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace matchmaker;
using namespace matchmaker::test;

TEST_CASE("sqlite store migrates config, games, and logs")
{
  const auto dir = make_temp_dir();
  const auto path = (dir / "store.sqlite").string();
  {
    auto store = open_sqlite_store(path);
    store->migrate();
    const auto config = store->load_config();
    CHECK(config.at("listen") == "0.0.0.0:443");
    CHECK(config.at("jwt_secret") == "change-me");
    CHECK(config.at("max_threads") == "1");
    CHECK(config.find("threads") == config.end());
    CHECK(config.at("relay_wt_enabled") == "0");
    CHECK(config.at("wt_listen") == "0.0.0.0:443");
    CHECK(config.at("cors_origins").empty());
    CHECK(config.at("wt_ticket_sec") == "30");
    CHECK(store->load_games().empty());
    CHECK(store->load_regions().size() == 1);

    auto game = sample_game();
    store->upsert_game(game);
    game.name = "Renamed";
    game.limits.allow_wt = true;
    store->upsert_game(game);
    const auto games = store->load_games();
    REQUIRE(games.size() == 1);
    CHECK(games[0].name == "Renamed");
    CHECK(games[0].limits.allow_wt);
    CHECK(games[0].token_hash == sha256_hex("secret"));
    CHECK(games[0].token_hash != "secret");

    write_log(store.get(), LogLevel::Info, "test", "survives reopen", game.id);
  }
  {
    auto store = open_sqlite_store(path);
    store->migrate();
    const auto logs = store->recent_logs(10);
    REQUIRE_FALSE(logs.empty());
    bool found = false;
    for( const auto& row : logs )
    {
      if( row.message == "survives reopen" )
      {
        found = true;
        CHECK(row.category == "test");
        CHECK(row.game_id == sample_game().id);
      }
    }
    CHECK(found);
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
