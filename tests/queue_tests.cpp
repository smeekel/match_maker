#include "lobby.hpp"
#include "queue.hpp"

#include "fixtures.hpp"

#include <doctest/doctest.h>

#include <unordered_map>

using namespace matchmaker;
using namespace matchmaker::test;
using Clock = LobbyService::Clock;

namespace
{

struct QueueWorld
{
  std::unordered_map<std::string, GameRecord> games;
  LobbyService lobbies;
  QueueService queue;

  QueueWorld()
  {
    auto game = sample_game();
    games.emplace(game.id, game);
    lobbies.set_rules(
        [this](const std::string& id) -> const GameRecord*
        {
          const auto it = games.find(id);
          return it == games.end() ? nullptr : &it->second;
        });
    lobbies.set_system_player_cap(64);
    queue.set_match_fn(
        [this](const std::string& game_id, const std::vector<Actor>& users, LobbyService::Time now)
        {
          return lobbies.create_match(game_id, users, now);
        });
    lobbies.set_on_destroyed(
        [this](const std::string& id)
        {
          queue.on_lobby_destroyed(id);
        });
  }

  Actor user(std::string id) const
  {
    return Actor{std::move(id), sample_game().id, "q", "ext"};
  }
};

} // namespace

TEST_CASE("fill-to-n match, unknown mode, and admission expiry")
{
  QueueWorld world;
  Timing timing;
  timing.admit = std::chrono::seconds(1);
  world.lobbies.set_timing(timing);
  const auto now = Clock::now();
  auto unknown =
      world.queue.enqueue(world.user("a"), world.games.at(sample_game().id), "missing", now);
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().code == ErrorCode::ModeNotConfigured);

  auto first = world.queue.enqueue(world.user("a"), world.games.at(sample_game().id), "duel", now);
  REQUIRE(first);
  CHECK((*first)["status"] == "queued");
  CHECK((*first)["position"] == 1);
  auto duplicate =
      world.queue.enqueue(world.user("a"), world.games.at(sample_game().id), "duel", now);
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == ErrorCode::AlreadyQueued);

  auto second = world.queue.enqueue(world.user("b"), world.games.at(sample_game().id), "duel", now);
  REQUIRE(second);
  CHECK((*second)["status"] == "match_ready");
  const auto lobby_id = (*second)["lobby_id"].get<std::string>();
  auto first_status = world.queue.status(world.user("a"), (*first)["ticket_id"].get<std::string>());
  REQUIRE(first_status);
  CHECK((*first_status)["status"] == "match_ready");
  CHECK((*first_status)["lobby_id"] == lobby_id);
  CHECK(world.lobbies.user_in_lobby("a"));
  CHECK(world.lobbies.user_in_lobby("b"));

  world.lobbies.tick(now + std::chrono::seconds(2));
  CHECK_FALSE(world.lobbies.lobby_of("a"));
  auto cancelled = world.queue.status(world.user("a"), (*first)["ticket_id"].get<std::string>());
  REQUIRE(cancelled);
  CHECK((*cancelled)["status"] == "cancelled");
}

TEST_CASE("cancelling a queued ticket frees the user")
{
  QueueWorld world;
  const auto now = Clock::now();
  auto ticket = world.queue.enqueue(world.user("a"), world.games.at(sample_game().id), "duel", now);
  REQUIRE(ticket);
  REQUIRE(world.queue.cancel(world.user("a"), (*ticket)["ticket_id"].get<std::string>(),
                             world.lobbies, now));
  CHECK_FALSE(world.queue.has_open_ticket("a"));
  auto again = world.queue.enqueue(world.user("a"), world.games.at(sample_game().id), "duel", now);
  REQUIRE(again);
  CHECK((*again)["status"] == "queued");
}
