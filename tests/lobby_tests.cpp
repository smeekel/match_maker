#include "lobby.hpp"

#include "fixtures.hpp"

#include <doctest/doctest.h>

#include <unordered_map>

using namespace matchmaker;
using namespace matchmaker::test;
using Clock = LobbyService::Clock;

namespace {

struct World {
  std::unordered_map<std::string, GameRecord> games;
  LobbyService lobbies;
  std::vector<nlohmann::json> events;

  World() {
    auto game = sample_game();
    games.emplace(game.id, game);
    bind();
  }

  void bind() {
    lobbies.set_rules([this](const std::string& id) -> const GameRecord* {
      const auto it = games.find(id);
      return it == games.end() ? nullptr : &it->second;
    });
    lobbies.set_emit([this](const nlohmann::json& event, const std::vector<std::string>&) { events.push_back(event); });
    lobbies.set_system_player_cap(64);
    lobbies.set_system_password_cap(64);
  }

  Actor user(std::string id, std::string game = sample_game().id) const {
    return Actor{std::move(id), std::move(game), "player", "ext"};
  }
};

std::string lobby_id_of(const nlohmann::json& body) { return body.at("lobby_id").get<std::string>(); }

}  // namespace

TEST_CASE("a user can hold only one lobby") {
  World world;
  const auto now = Clock::now();
  auto first = world.lobbies.create(world.user("host"), now);
  REQUIRE(first);
  auto second = world.lobbies.create(world.user("host"), now);
  REQUIRE_FALSE(second);
  CHECK(second.error().code == ErrorCode::AlreadyInLobby);
  auto guest = world.lobbies.begin_join(world.user("host"), lobby_id_of(*first), std::nullopt, now);
  REQUIRE_FALSE(guest);
  CHECK(guest.error().code == ErrorCode::AlreadyInLobby);

  REQUIRE(world.lobbies.leave("host", now));
  CHECK_FALSE(world.lobbies.user_in_lobby("host"));
  auto again = world.lobbies.create(world.user("host"), now);
  REQUIRE(again);
}

TEST_CASE("join rules cover slots, phase, lock, and short codes") {
  World world;
  const auto now = Clock::now();
  auto created = world.lobbies.create(world.user("host"), now);
  REQUIRE(created);
  const auto id = lobby_id_of(*created);
  REQUIRE(world.lobbies.complete_connect("host", id, now));

  auto closed = world.lobbies.begin_join(world.user("a"), id, std::nullopt, now);
  REQUIRE_FALSE(closed);
  CHECK(closed.error().code == ErrorCode::LobbyNotOpen);

  REQUIRE(world.lobbies.set_phase(world.user("host"), id, "open"));
  auto too_many = world.lobbies.set_slots(world.user("host"), id, 5);
  REQUIRE_FALSE(too_many);
  CHECK(too_many.error().code == ErrorCode::Limit);
  REQUIRE(world.lobbies.set_slots(world.user("host"), id, 1));
  auto full = world.lobbies.begin_join(world.user("a"), id, std::nullopt, now);
  REQUIRE_FALSE(full);
  CHECK(full.error().code == ErrorCode::Full);
  REQUIRE(world.lobbies.set_slots(world.user("host"), id, 4));

  REQUIRE(world.lobbies.set_phase(world.user("host"), id, "in_progress"));
  auto late = world.lobbies.begin_join(world.user("a"), id, std::nullopt, now);
  REQUIRE_FALSE(late);
  CHECK(late.error().code == ErrorCode::LateJoinDisabled);
  world.games[sample_game().id].allow_join_in_progress = true;
  REQUIRE(world.lobbies.set_late_join(world.user("host"), id, true));
  auto allowed = world.lobbies.begin_join(world.user("a"), id, std::nullopt, now);
  REQUIRE(allowed);
  REQUIRE(world.lobbies.leave("a", now));

  REQUIRE(world.lobbies.set_password(world.user("host"), id, std::string("secret-room")));
  auto missing = world.lobbies.begin_join(world.user("b"), id, std::nullopt, now);
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == ErrorCode::BadPassword);
  auto wrong = world.lobbies.begin_join(world.user("b"), id, std::string("nope"), now);
  REQUIRE_FALSE(wrong);
  CHECK(wrong.error().code == ErrorCode::BadPassword);
  auto locked = world.lobbies.begin_join(world.user("b"), id, std::string("secret-room"), now);
  REQUIRE(locked);
  REQUIRE(world.lobbies.leave("b", now));
  REQUIRE(world.lobbies.set_password(world.user("host"), id, std::nullopt));

  REQUIRE(world.lobbies.set_short_code(world.user("host"), id, std::string("Ab12")));
  auto by_code = world.lobbies.get_by_code(sample_game().id, "ab12");
  REQUIRE(by_code);
  CHECK(lobby_id_of(*by_code) == id);
  CHECK((*by_code)["short_code"] == "AB12");

  auto other = world.lobbies.create(world.user("other-host"), now);
  REQUIRE(other);
  const auto other_id = lobby_id_of(*other);
  auto taken = world.lobbies.set_short_code(world.user("other-host"), other_id, std::string("ab12"));
  REQUIRE_FALSE(taken);
  CHECK(taken.error().code == ErrorCode::CodeTaken);
  auto bad_code = world.lobbies.set_short_code(world.user("host"), id, std::string("has space"));
  REQUIRE_FALSE(bad_code);
  CHECK(bad_code.error().code == ErrorCode::InvalidCode);
  REQUIRE(world.lobbies.set_short_code(world.user("host"), id, std::nullopt));
  auto disabled = world.lobbies.get_by_code(sample_game().id, "ab12");
  REQUIRE_FALSE(disabled);
  CHECK(disabled.error().code == ErrorCode::NotFound);

  auto listed = world.lobbies.list(sample_game().id, "", 50);
  REQUIRE(listed);
  CHECK((*listed)["lobbies"].size() == 1);
  CHECK_FALSE((*listed)["lobbies"][0].contains("metadata"));
}

TEST_CASE("host leave, heartbeat, admission, and game isolation") {
  World world;
  auto now = Clock::now();
  world.games[sample_game().id].promote_host_on_leave = false;
  auto created = world.lobbies.create(world.user("host"), now);
  REQUIRE(created);
  const auto id = lobby_id_of(*created);
  REQUIRE(world.lobbies.complete_connect("host", id, now));
  REQUIRE(world.lobbies.set_phase(world.user("host"), id, "open"));
  REQUIRE(world.lobbies.begin_join(world.user("guest"), id, std::nullopt, now));
  REQUIRE(world.lobbies.complete_connect("guest", id, now));
  REQUIRE(world.lobbies.leave("host", now));
  CHECK_FALSE(world.lobbies.lobby_of("guest"));
  bool closed = false;
  for (const auto& event : world.events) {
    if (event.value("event", "") == "lobby_closed") {
      closed = true;
    }
  }
  CHECK(closed);

  world.events.clear();
  world.games[sample_game().id].promote_host_on_leave = true;
  created = world.lobbies.create(world.user("host"), now);
  REQUIRE(created);
  const auto promoted = lobby_id_of(*created);
  REQUIRE(world.lobbies.complete_connect("host", promoted, now));
  REQUIRE(world.lobbies.set_phase(world.user("host"), promoted, "open"));
  REQUIRE(world.lobbies.begin_join(world.user("guest"), promoted, std::nullopt, now));
  REQUIRE(world.lobbies.complete_connect("guest", promoted, now));
  REQUIRE(world.lobbies.leave("host", now));
  auto still = world.lobbies.get(sample_game().id, promoted);
  REQUIRE(still);
  CHECK((*still)["host_id"] == "guest");
  REQUIRE(world.lobbies.leave("guest", now));
  CHECK_FALSE(world.lobbies.get(sample_game().id, promoted));

  Timing timing;
  timing.admit = std::chrono::seconds(60);
  timing.join_hold = std::chrono::seconds(10);
  timing.heartbeat_interval = std::chrono::seconds(1);
  timing.heartbeat_misses = 2;
  world.lobbies.set_timing(timing);
  created = world.lobbies.create(world.user("beating"), now);
  REQUIRE(created);
  const auto beating = lobby_id_of(*created);
  REQUIRE(world.lobbies.complete_connect("beating", beating, now));
  world.lobbies.tick(now + std::chrono::seconds(3));
  CHECK_FALSE(world.lobbies.lobby_of("beating"));

  timing.admit = std::chrono::seconds(1);
  world.lobbies.set_timing(timing);
  const auto admitted_at = Clock::now();
  created = world.lobbies.create(world.user("late-host"), admitted_at);
  REQUIRE(created);
  world.lobbies.tick(admitted_at + std::chrono::seconds(2));
  CHECK_FALSE(world.lobbies.lobby_of("late-host"));

  const auto other_id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  world.games.emplace(other_id, sample_game(other_id, "other"));
  created = world.lobbies.create(world.user("home", sample_game().id), Clock::now());
  REQUIRE(created);
  auto foreign = world.lobbies.get(other_id, lobby_id_of(*created));
  REQUIRE_FALSE(foreign);
  CHECK(foreign.error().code == ErrorCode::NotFound);
  auto foreign_list = world.lobbies.list(other_id, "", 50);
  REQUIRE(foreign_list);
  CHECK((*foreign_list)["lobbies"].empty());
}

TEST_CASE("match admission joins a closed locked lobby") {
  World world;
  const auto now = Clock::now();
  std::vector<Actor> users{world.user("p0"), world.user("p1")};
  auto created = world.lobbies.create_match(sample_game().id, users, now);
  REQUIRE(created);
  REQUIRE(world.lobbies.set_password(world.user("p0"), *created, std::string("locked")));
  auto voluntary = world.lobbies.begin_join(world.user("stranger"), *created, std::string("locked"), now);
  REQUIRE_FALSE(voluntary);
  CHECK(voluntary.error().code == ErrorCode::LobbyNotOpen);
  auto admitted = world.lobbies.complete_connect("p1", *created, now);
  REQUIRE(admitted);
  CHECK((*admitted)["op"] == "lobby_snapshot");
  CHECK((*admitted)["phase"] == "closed");
  CHECK_FALSE((*admitted).contains("password"));
}
