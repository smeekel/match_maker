#pragma once

#include "result.hpp"
#include "types.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matchmaker {

struct Timing {
  std::chrono::seconds admit{60};
  std::chrono::seconds join_hold{10};
  std::chrono::seconds heartbeat_interval{2};
  int heartbeat_misses = 3;
};

class LobbyService {
 public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  using Emit = std::function<void(const nlohmann::json& event, const std::vector<std::string>& recipients)>;
  using RulesFn = std::function<const GameRecord*(const std::string& game_id)>;
  using DestroyedFn = std::function<void(const std::string& lobby_id)>;

  void set_emit(Emit emit) { emit_ = std::move(emit); }
  void set_rules(RulesFn rules) { rules_ = std::move(rules); }
  void set_on_destroyed(DestroyedFn fn) { on_destroyed_ = std::move(fn); }
  void set_timing(Timing timing) { timing_ = timing; }
  void set_system_player_cap(int cap) { system_player_cap_ = cap; }
  void set_system_password_cap(int cap) { system_password_cap_ = cap; }

  Result<nlohmann::json> create(const Actor& actor, Time now);
  Result<std::string> create_match(const std::string& game_id, const std::vector<Actor>& users, Time now);
  Result<nlohmann::json> begin_join(const Actor& actor, const std::string& lobby_id, const std::optional<std::string>& password,
                                    Time now);
  Result<nlohmann::json> complete_connect(const std::string& user_id, const std::string& lobby_id, Time now);
  Status leave(const std::string& user_id, Time now);
  Status close_lobby(const Actor& actor, Time now);
  Status set_phase(const Actor& actor, const std::string& lobby_id, const std::string& phase);
  Status set_slots(const Actor& actor, const std::string& lobby_id, int slots);
  Status set_late_join(const Actor& actor, const std::string& lobby_id, bool allowed);
  Status set_password(const Actor& actor, const std::string& lobby_id, const std::optional<std::string>& password);
  Status set_short_code(const Actor& actor, const std::string& lobby_id, const std::optional<std::string>& code);
  Status set_metadata(const Actor& actor, const std::string& lobby_id, const nlohmann::json& patch);
  Status set_alias(const Actor& actor, const std::string& lobby_id, const std::string& alias);
  Status set_ready(const Actor& actor, const std::string& lobby_id, bool ready);
  Status chat(const Actor& actor, const std::string& lobby_id, const std::string& text);
  Status custom(const Actor& actor, const std::string& lobby_id, const std::string& name, const nlohmann::json& data,
                const std::optional<std::string>& target);
  Status heartbeat(const Actor& actor, const std::string& lobby_id, Time now);

  Result<nlohmann::json> list(const std::string& game_id, const std::string& cursor, int limit) const;
  Result<nlohmann::json> get(const std::string& game_id, const std::string& lobby_id) const;
  Result<nlohmann::json> get_by_code(const std::string& game_id, const std::string& code) const;
  std::optional<std::string> lobby_of(const std::string& user_id) const;
  std::optional<std::string> connected_host(const std::string& lobby_id) const;
  bool user_in_lobby(const std::string& user_id) const;
  bool connected_member(const std::string& lobby_id, const std::string& user_id) const;
  int lobby_count(const std::string& game_id) const;
  void tick(Time now);
  void drop_unconnected(const std::string& user_id, Time now);

 private:
  struct Member {
    std::string user_id;
    std::string alias;
    std::string external_id;
    bool ready = false;
    bool connected = false;
    enum class Hold { None, Host, Match, Join } hold = Hold::None;
    Time deadline{};
  };
  struct Lobby {
    std::string id;
    std::string game_id;
    std::string host_id;
    std::string phase = "closed";
    int slots = 1;
    std::uint64_t seq = 0;
    std::optional<std::string> password;
    bool allow_late_join = false;
    std::optional<std::string> short_code;
    nlohmann::json metadata = nlohmann::json::object();
    std::vector<Member> members;
    Time last_heartbeat{};
    bool host_ever_connected = false;
  };

  const GameRecord* rules(const std::string& game_id) const;
  int player_cap(const GameRecord& game) const;
  int password_cap(const GameRecord& game) const;
  int occupied(const Lobby& lobby) const;
  Member* find_member(Lobby& lobby, const std::string& user_id);
  const Member* find_member(const Lobby& lobby, const std::string& user_id) const;
  Lobby* find_lobby(const std::string& id);
  const Lobby* find_lobby(const std::string& id) const;
  bool listable(const Lobby& lobby) const;
  nlohmann::json summary(const Lobby& lobby) const;
  nlohmann::json snapshot(const Lobby& lobby) const;
  void emit(const nlohmann::json& event, const std::vector<std::string>& recipients) const;
  std::vector<std::string> connected_ids(const Lobby& lobby, const std::string& except = {}) const;
  void destroy(const std::string& id, const std::string& reason);
  void forget_user(const std::string& user_id, const std::string& lobby_id);
  Status require_host(const Actor& actor, Lobby& lobby);
  Status require_member(const Actor& actor, Lobby& lobby);

  Emit emit_;
  RulesFn rules_;
  DestroyedFn on_destroyed_;
  Timing timing_{};
  int system_player_cap_ = 64;
  int system_password_cap_ = 64;
  std::uint64_t next_seq_ = 1;
  std::unordered_map<std::string, Lobby> lobbies_;
  std::unordered_map<std::string, std::string> user_lobby_;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> codes_;
};

}  // namespace matchmaker
