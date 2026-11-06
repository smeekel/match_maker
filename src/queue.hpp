#pragma once

#include "lobby.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace matchmaker {

class QueueService {
 public:
  using Time = LobbyService::Time;
  using MatchFn = std::function<Result<std::string>(const std::string& game_id, const std::vector<Actor>& users, Time now)>;
  using Waiter = std::function<void()>;

  void set_match_fn(MatchFn fn) { match_fn_ = std::move(fn); }

  Result<nlohmann::json> enqueue(const Actor& actor, const GameRecord& game, const std::string& mode, Time now);
  Result<nlohmann::json> status(const Actor& actor, const std::string& ticket_id) const;
  Status cancel(const Actor& actor, const std::string& ticket_id, LobbyService& lobbies, Time now);
  void cancel_queued_user(const std::string& user_id);
  void on_lobby_destroyed(const std::string& lobby_id);
  bool has_open_ticket(const std::string& user_id) const;
  int queued_count(const std::string& game_id) const;
  void add_waiter(const Actor& actor, const std::string& ticket_id, Waiter waiter);

 private:
  struct Ticket {
    std::string id;
    std::string user_id;
    std::string game_id;
    std::string mode;
    std::string lobby_id;
    std::string alias;
    std::string external_id;
    enum class State { Queued, Ready, Cancelled } state = State::Queued;
    std::uint64_t seq = 0;
    std::vector<Waiter> waiters;
  };

  nlohmann::json view(const Ticket& ticket) const;
  int position_of(const Ticket& ticket) const;
  Status try_match(const GameRecord& game, const std::string& mode, int players, Time now);
  void fire(Ticket& ticket);
  Ticket* find(const std::string& id);
  const Ticket* find(const std::string& id) const;

  MatchFn match_fn_;
  std::uint64_t next_seq_ = 1;
  std::unordered_map<std::string, Ticket> tickets_;
  std::unordered_map<std::string, std::string> user_ticket_;
};

}  // namespace matchmaker
