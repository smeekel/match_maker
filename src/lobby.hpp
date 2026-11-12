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

namespace matchmaker
{

struct Timing
{
  std::chrono::seconds admit{60};
  std::chrono::seconds join_hold{10};
  std::chrono::seconds heartbeat_interval{2};
  int heartbeat_misses = 3;
};

class LobbyService
{
public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;
  using Emit =
      std::function<void(const nlohmann::json& event, const std::vector<std::string>& recipients)>;
  using RulesFn = std::function<const GameRecord*(const std::string& game_id)>;
  using DestroyedFn = std::function<void(const std::string& lobby_id)>;

  void set_emit(Emit emit)
  {
    emit_ = std::move(emit);
  }
  void set_rules(RulesFn rules)
  {
    rules_ = std::move(rules);
  }
  void set_on_destroyed(DestroyedFn fn)
  {
    on_destroyed_ = std::move(fn);
  }
  void set_timing(Timing timing)
  {
    timing_ = timing;
  }
  void set_system_player_cap(int cap)
  {
    system_player_cap_ = cap;
  }
  void set_system_password_cap(int cap)
  {
    system_password_cap_ = cap;
  }

  Result<nlohmann::json> create(const Actor& actor, Time now);
  Result<nlohmann::json> begin_join(const Actor& actor, const std::string& lobby_id,
                                    const std::optional<std::string>& password, Time now);
  Result<nlohmann::json> complete_connect(const std::string& user_id, const std::string& lobby_id,
                                          Time now, const std::string& transport = "websocket");
  Status leave(const std::string& user_id, Time now);
  Status close_lobby(const Actor& actor, Time now);
  Status set_phase(const Actor& actor, const std::string& lobby_id, const std::string& phase)
  {
    return set_phase_as(&actor, lobby_id, phase);
  }
  Status set_slots(const Actor& actor, const std::string& lobby_id, int slots)
  {
    return set_slots_as(&actor, lobby_id, slots);
  }
  Status set_late_join(const Actor& actor, const std::string& lobby_id, bool allowed)
  {
    return set_late_join_as(&actor, lobby_id, allowed);
  }
  Status set_password(const Actor& actor, const std::string& lobby_id,
                      const std::optional<std::string>& password)
  {
    return set_password_as(&actor, lobby_id, password);
  }
  Status set_short_code(const Actor& actor, const std::string& lobby_id,
                        const std::optional<std::string>& code)
  {
    return set_short_code_as(&actor, lobby_id, code);
  }
  Status set_metadata(const Actor& actor, const std::string& lobby_id, const nlohmann::json& patch)
  {
    return set_metadata_as(&actor, lobby_id, patch);
  }

  // Operator versions of the host controls. They skip the host check, apply the same
  // validation, and send the resulting event to every connected member, host included.
  Status admin_set_phase(const std::string& lobby_id, const std::string& phase)
  {
    return set_phase_as(nullptr, lobby_id, phase);
  }
  Status admin_set_slots(const std::string& lobby_id, int slots)
  {
    return set_slots_as(nullptr, lobby_id, slots);
  }
  Status admin_set_late_join(const std::string& lobby_id, bool allowed)
  {
    return set_late_join_as(nullptr, lobby_id, allowed);
  }
  Status admin_set_password(const std::string& lobby_id,
                            const std::optional<std::string>& password)
  {
    return set_password_as(nullptr, lobby_id, password);
  }
  Status admin_set_short_code(const std::string& lobby_id, const std::optional<std::string>& code)
  {
    return set_short_code_as(nullptr, lobby_id, code);
  }
  Status admin_set_metadata(const std::string& lobby_id, const nlohmann::json& patch)
  {
    return set_metadata_as(nullptr, lobby_id, patch);
  }
  // Removes |user_id| from |lobby_id| as if they left. The removed user receives a `kicked`
  // event, which closes their socket.
  Status admin_kick(const std::string& lobby_id, const std::string& user_id, Time now);
  Status admin_close(const std::string& lobby_id);
  // Every live lobby across games, closed ones included. Never includes the password.
  nlohmann::json admin_list() const;
  // admin_list's row plus members and their reservation state.
  Result<nlohmann::json> admin_view(const std::string& lobby_id) const;
  Status set_alias(const Actor& actor, const std::string& lobby_id, const std::string& alias);
  Status set_ready(const Actor& actor, const std::string& lobby_id, bool ready);
  Status chat(const Actor& actor, const std::string& lobby_id, const std::string& text);
  Status custom(const Actor& actor, const std::string& lobby_id, const std::string& name,
                const nlohmann::json& data, const std::optional<std::string>& target);
  Status heartbeat(const Actor& actor, const std::string& lobby_id, Time now);

  Result<nlohmann::json> list(const std::string& game_id, const std::string& cursor,
                              int limit) const;
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
  struct Member
  {
    std::string user_id;
    std::string alias;
    std::string external_id;
    bool ready = false;
    bool connected = false;
    std::string transport;
    enum class Hold
    {
      None,
      Host,
      Join
    } hold = Hold::None;
    Time deadline{};
  };
  struct Lobby
  {
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
  // |actor| is null for an operator, which skips the host check.
  Result<Lobby*> controlled(const Actor* actor, const std::string& lobby_id);
  std::vector<std::string> audience(const Lobby& lobby, const Actor* actor) const;
  nlohmann::json admin_row(const Lobby& lobby) const;
  Status set_phase_as(const Actor* actor, const std::string& lobby_id, const std::string& phase);
  Status set_slots_as(const Actor* actor, const std::string& lobby_id, int slots);
  Status set_late_join_as(const Actor* actor, const std::string& lobby_id, bool allowed);
  Status set_password_as(const Actor* actor, const std::string& lobby_id,
                         const std::optional<std::string>& password);
  Status set_short_code_as(const Actor* actor, const std::string& lobby_id,
                           const std::optional<std::string>& code);
  Status set_metadata_as(const Actor* actor, const std::string& lobby_id,
                         const nlohmann::json& patch);

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

} // namespace matchmaker
