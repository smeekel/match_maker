#include "lobby.hpp"

#include "crypto.hpp"

#include <algorithm>
#include <cctype>

namespace matchmaker
{
namespace
{

nlohmann::json event_of(const char* name, const nlohmann::json& fields)
{
  nlohmann::json event = fields;
  event["op"] = "event";
  event["event"] = name;
  return event;
}

std::optional<std::string> normalize_code(const std::string& raw)
{
  if( raw.empty() )
  {
    return std::string{};
  }
  if( raw.size() > 16 )
  {
    return std::nullopt;
  }
  std::string out;
  out.reserve(raw.size());
  for( unsigned char c : raw )
  {
    if( !std::isalnum(c) )
    {
      return std::nullopt;
    }
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

} // namespace

const GameRecord* LobbyService::rules(const std::string& game_id) const
{
  return rules_ ? rules_(game_id) : nullptr;
}

int LobbyService::player_cap(const GameRecord& game) const
{
  return std::max(1, std::min(game.limits.max_players_per_lobby, system_player_cap_));
}

int LobbyService::password_cap(const GameRecord& game) const
{
  return std::max(1, std::min(game.limits.max_password_length, system_password_cap_));
}

int LobbyService::occupied(const Lobby& lobby) const
{
  return static_cast<int>(lobby.members.size());
}

LobbyService::Member* LobbyService::find_member(Lobby& lobby, const std::string& user_id)
{
  for( auto& member : lobby.members )
  {
    if( member.user_id == user_id )
    {
      return &member;
    }
  }
  return nullptr;
}

const LobbyService::Member* LobbyService::find_member(const Lobby& lobby,
                                                      const std::string& user_id) const
{
  for( const auto& member : lobby.members )
  {
    if( member.user_id == user_id )
    {
      return &member;
    }
  }
  return nullptr;
}

LobbyService::Lobby* LobbyService::find_lobby(const std::string& id)
{
  const auto it = lobbies_.find(id);
  return it == lobbies_.end() ? nullptr : &it->second;
}

const LobbyService::Lobby* LobbyService::find_lobby(const std::string& id) const
{
  const auto it = lobbies_.find(id);
  return it == lobbies_.end() ? nullptr : &it->second;
}

bool LobbyService::listable(const Lobby& lobby) const
{
  if( lobby.phase == "open" )
  {
    return true;
  }
  if( lobby.phase != "in_progress" )
  {
    return false;
  }
  const auto* game = rules(lobby.game_id);
  return game && game->allow_join_in_progress && lobby.allow_late_join;
}

nlohmann::json LobbyService::summary(const Lobby& lobby) const
{
  return {{"lobby_id", lobby.id},
          {"host_id", lobby.host_id},
          {"phase", lobby.phase},
          {"slots", lobby.slots},
          {"member_count", occupied(lobby)},
          {"locked", lobby.password.has_value()},
          {"allow_late_join", lobby.allow_late_join},
          {"short_code",
           lobby.short_code ? nlohmann::json(*lobby.short_code) : nlohmann::json(nullptr)},
          {"metadata", lobby.metadata}};
}

nlohmann::json LobbyService::snapshot(const Lobby& lobby) const
{
  auto snap = summary(lobby);
  snap["op"] = "lobby_snapshot";
  nlohmann::json members = nlohmann::json::array();
  for( const auto& member : lobby.members )
  {
    members.push_back({{"user_id", member.user_id},
                       {"alias", member.alias},
                       {"external_id", member.external_id},
                       {"ready", member.ready},
                       {"connected", member.connected}});
  }
  snap["members"] = std::move(members);
  return snap;
}

void LobbyService::emit(const nlohmann::json& event,
                        const std::vector<std::string>& recipients) const
{
  if( emit_ && !recipients.empty() )
  {
    emit_(event, recipients);
  }
}

std::vector<std::string> LobbyService::connected_ids(const Lobby& lobby,
                                                     const std::string& except) const
{
  std::vector<std::string> ids;
  for( const auto& member : lobby.members )
  {
    if( member.connected && member.user_id != except )
    {
      ids.push_back(member.user_id);
    }
  }
  return ids;
}

void LobbyService::forget_user(const std::string& user_id, const std::string& lobby_id)
{
  const auto it = user_lobby_.find(user_id);
  if( it != user_lobby_.end() && it->second == lobby_id )
  {
    user_lobby_.erase(it);
  }
}

void LobbyService::destroy(const std::string& lobby_id, const std::string& reason)
{
  const std::string id = lobby_id;
  auto it = lobbies_.find(id);
  if( it == lobbies_.end() )
  {
    return;
  }
  Lobby lobby = std::move(it->second);
  lobbies_.erase(it);
  for( const auto& member : lobby.members )
  {
    forget_user(member.user_id, id);
  }
  if( lobby.short_code )
  {
    auto codes = codes_.find(lobby.game_id);
    if( codes != codes_.end() )
    {
      codes->second.erase(*lobby.short_code);
    }
  }
  if( on_destroyed_ )
  {
    on_destroyed_(id);
  }
  auto recipients = connected_ids(lobby);
  emit(event_of("lobby_closed", {{"lobby_id", id}, {"reason", reason}}), recipients);
}

Status LobbyService::require_host(const Actor& actor, Lobby& lobby)
{
  if( actor.game_id != lobby.game_id )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( actor.user_id != lobby.host_id )
  {
    return Err(ErrorCode::NotHost, "host only");
  }
  return true;
}

Status LobbyService::require_member(const Actor& actor, Lobby& lobby)
{
  if( actor.game_id != lobby.game_id )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( !find_member(lobby, actor.user_id) )
  {
    return Err(ErrorCode::NotMember, "not in lobby");
  }
  return true;
}

Result<nlohmann::json> LobbyService::create(const Actor& actor, Time now)
{
  if( user_in_lobby(actor.user_id) )
  {
    return Err(ErrorCode::AlreadyInLobby, "already in a lobby");
  }
  const auto* game = rules(actor.game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( lobby_count(actor.game_id) >= game->limits.max_lobbies )
  {
    return Err(ErrorCode::Limit, "lobby limit");
  }
  Lobby lobby;
  lobby.id = uuid_v4();
  lobby.game_id = actor.game_id;
  lobby.host_id = actor.user_id;
  lobby.phase = "closed";
  lobby.slots = player_cap(*game);
  lobby.seq = next_seq_++;
  Member host;
  host.user_id = actor.user_id;
  host.alias = actor.alias;
  host.external_id = actor.external_id;
  host.hold = Member::Hold::Host;
  host.deadline = now + timing_.admit;
  lobby.members.push_back(host);
  user_lobby_[actor.user_id] = lobby.id;
  const auto id = lobby.id;
  lobbies_.emplace(id, std::move(lobby));
  return summary(lobbies_.at(id));
}

Result<nlohmann::json> LobbyService::begin_join(const Actor& actor, const std::string& lobby_id,
                                                const std::optional<std::string>& password,
                                                Time now)
{
  if( user_in_lobby(actor.user_id) )
  {
    return Err(ErrorCode::AlreadyInLobby, "already in a lobby");
  }
  auto* lobby = find_lobby(lobby_id);
  if( !lobby || lobby->game_id != actor.game_id )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  const auto* game = rules(actor.game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( lobby->phase == "closed" )
  {
    return Err(ErrorCode::LobbyNotOpen, "lobby is closed");
  }
  if( lobby->phase == "in_progress" && !(game->allow_join_in_progress && lobby->allow_late_join) )
  {
    return Err(ErrorCode::LateJoinDisabled, "late join is disabled");
  }
  if( lobby->phase != "open" && lobby->phase != "in_progress" )
  {
    return Err(ErrorCode::LobbyNotOpen, "lobby is not joinable");
  }
  if( lobby->password )
  {
    if( !password || !ct_equal(*password, *lobby->password) )
    {
      return Err(ErrorCode::BadPassword, "bad password");
    }
  }
  const int cap = std::min(lobby->slots, player_cap(*game));
  if( occupied(*lobby) >= cap )
  {
    return Err(ErrorCode::Full, "lobby is full");
  }
  Member member;
  member.user_id = actor.user_id;
  member.alias = actor.alias;
  member.external_id = actor.external_id;
  member.hold = Member::Hold::Join;
  member.deadline = now + timing_.join_hold;
  lobby->members.push_back(member);
  user_lobby_[actor.user_id] = lobby->id;
  return nlohmann::json{{"ok", true}, {"lobby_id", lobby->id}};
}

Result<nlohmann::json> LobbyService::complete_connect(const std::string& user_id,
                                                      const std::string& lobby_id, Time now)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  auto* member = find_member(*lobby, user_id);
  if( !member || member->hold == Member::Hold::None )
  {
    return Err(ErrorCode::NotMember, "no join reservation");
  }
  if( now > member->deadline )
  {
    return Err(ErrorCode::NotMember, "reservation expired");
  }
  const bool was_host = member->hold == Member::Hold::Host;
  member->hold = Member::Hold::None;
  member->connected = true;
  if( was_host || user_id == lobby->host_id )
  {
    lobby->last_heartbeat = now;
    lobby->host_ever_connected = true;
  }
  emit(event_of("user_entered",
                {{"lobby_id", lobby->id}, {"user_id", member->user_id}, {"alias", member->alias}}),
       connected_ids(*lobby, user_id));
  return snapshot(*lobby);
}

Status LobbyService::leave(const std::string& user_id, Time now)
{
  const auto found = user_lobby_.find(user_id);
  if( found == user_lobby_.end() )
  {
    return true;
  }
  auto* lobby = find_lobby(found->second);
  if( !lobby )
  {
    user_lobby_.erase(found);
    return true;
  }
  const bool was_host = lobby->host_id == user_id;
  const auto lobby_id = lobby->id;
  lobby->members.erase(std::remove_if(lobby->members.begin(), lobby->members.end(),
                                      [&](const Member& member)
                                      {
                                        return member.user_id == user_id;
                                      }),
                       lobby->members.end());
  forget_user(user_id, lobby_id);
  if( lobby->members.empty() )
  {
    destroy(lobby_id, "empty");
    return true;
  }
  if( was_host )
  {
    const auto* game = rules(lobby->game_id);
    if( !game || !game->promote_host_on_leave )
    {
      destroy(lobby_id, "host_left");
      return true;
    }
    lobby->host_id = lobby->members.front().user_id;
    if( lobby->members.front().connected )
    {
      lobby->last_heartbeat = now;
    }
    emit(event_of("user_left", {{"lobby_id", lobby_id}, {"user_id", user_id}}),
         connected_ids(*lobby));
    emit(event_of("host_changed", {{"lobby_id", lobby_id}, {"host_id", lobby->host_id}}),
         connected_ids(*lobby));
    return true;
  }
  emit(event_of("user_left", {{"lobby_id", lobby_id}, {"user_id", user_id}}),
       connected_ids(*lobby));
  return true;
}

Status LobbyService::close_lobby(const Actor& actor, Time now)
{
  (void)now;
  auto* lobby = find_lobby(user_lobby_.count(actor.user_id) ? user_lobby_.at(actor.user_id) : "");
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  destroy(lobby->id, "closed");
  return true;
}

void LobbyService::drop_unconnected(const std::string& user_id, Time now)
{
  const auto found = user_lobby_.find(user_id);
  if( found == user_lobby_.end() )
  {
    return;
  }
  auto* lobby = find_lobby(found->second);
  if( !lobby )
  {
    return;
  }
  auto* member = find_member(*lobby, user_id);
  if( !member || member->connected )
  {
    return;
  }
  leave(user_id, now);
}

Status LobbyService::set_phase(const Actor& actor, const std::string& lobby_id,
                               const std::string& phase)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  if( phase != "closed" && phase != "open" && phase != "in_progress" )
  {
    return Err(ErrorCode::BadRequest, "unknown phase");
  }
  lobby->phase = phase;
  emit(event_of("lobby_phase_changed", {{"lobby_id", lobby_id}, {"phase", phase}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_slots(const Actor& actor, const std::string& lobby_id, int slots)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  const auto* game = rules(lobby->game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  const int cap = player_cap(*game);
  if( slots < 1 || slots < occupied(*lobby) )
  {
    return Err(ErrorCode::BadRequest, "slots below current members");
  }
  if( slots > cap )
  {
    return Err(ErrorCode::Limit, "slots exceed the game cap");
  }
  lobby->slots = slots;
  emit(event_of("slots_changed", {{"lobby_id", lobby_id}, {"slots", slots}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_late_join(const Actor& actor, const std::string& lobby_id, bool allowed)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  lobby->allow_late_join = allowed;
  emit(event_of("late_join_changed", {{"lobby_id", lobby_id}, {"allow_late_join", allowed}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_password(const Actor& actor, const std::string& lobby_id,
                                  const std::optional<std::string>& password)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  if( !password || password->empty() )
  {
    lobby->password.reset();
  }
  else
  {
    const auto* game = rules(lobby->game_id);
    if( !game )
    {
      return Err(ErrorCode::UnknownGame, "unknown game");
    }
    if( static_cast<int>(password->size()) > password_cap(*game) )
    {
      return Err(ErrorCode::TooLarge, "password too long");
    }
    lobby->password = *password;
  }
  emit(event_of("lock_changed", {{"lobby_id", lobby_id}, {"locked", lobby->password.has_value()}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_short_code(const Actor& actor, const std::string& lobby_id,
                                    const std::optional<std::string>& code)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  std::optional<std::string> next;
  if( !code || code->empty() )
  {
    next.reset();
  }
  else
  {
    next = normalize_code(*code);
    if( !next || next->empty() )
    {
      return Err(ErrorCode::InvalidCode, "short code must be 1 to 16 letters or digits");
    }
    auto& table = codes_[lobby->game_id];
    const auto taken = table.find(*next);
    if( taken != table.end() && taken->second != lobby->id )
    {
      return Err(ErrorCode::CodeTaken, "short code is in use");
    }
  }
  if( lobby->short_code )
  {
    codes_[lobby->game_id].erase(*lobby->short_code);
  }
  lobby->short_code = next;
  if( next )
  {
    codes_[lobby->game_id][*next] = lobby->id;
  }
  emit(event_of("short_code_changed",
                {{"lobby_id", lobby_id},
                 {"short_code", next ? nlohmann::json(*next) : nlohmann::json(nullptr)}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_metadata(const Actor& actor, const std::string& lobby_id,
                                  const nlohmann::json& patch)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_host(actor, *lobby); !status )
  {
    return status;
  }
  if( !patch.is_object() )
  {
    return Err(ErrorCode::BadRequest, "metadata must be an object");
  }
  const auto* game = rules(lobby->game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  auto next = lobby->metadata;
  for( auto it = patch.begin(); it != patch.end(); ++it )
  {
    if( it.value().is_null() )
    {
      next.erase(it.key());
    }
    else
    {
      next[it.key()] = it.value();
    }
  }
  if( static_cast<int>(next.dump().size()) > game->limits.max_metadata_bytes )
  {
    return Err(ErrorCode::TooLarge, "metadata too large");
  }
  lobby->metadata = std::move(next);
  emit(event_of("lobby_metadata", {{"lobby_id", lobby_id}, {"metadata", lobby->metadata}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_alias(const Actor& actor, const std::string& lobby_id,
                               const std::string& alias)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_member(actor, *lobby); !status )
  {
    return status;
  }
  const auto* game = rules(lobby->game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( static_cast<int>(alias.size()) > game->limits.max_alias_bytes )
  {
    return Err(ErrorCode::TooLarge, "alias too long");
  }
  auto* member = find_member(*lobby, actor.user_id);
  member->alias = alias;
  emit(event_of("alias_changed",
                {{"lobby_id", lobby_id}, {"user_id", actor.user_id}, {"alias", alias}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::set_ready(const Actor& actor, const std::string& lobby_id, bool ready)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_member(actor, *lobby); !status )
  {
    return status;
  }
  auto* member = find_member(*lobby, actor.user_id);
  member->ready = ready;
  emit(event_of("ready_changed",
                {{"lobby_id", lobby_id}, {"user_id", actor.user_id}, {"ready", ready}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::chat(const Actor& actor, const std::string& lobby_id, const std::string& text)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_member(actor, *lobby); !status )
  {
    return status;
  }
  const auto* game = rules(lobby->game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( static_cast<int>(text.size()) > game->limits.max_chat_bytes )
  {
    return Err(ErrorCode::TooLarge, "chat too long");
  }
  const auto* member = find_member(*lobby, actor.user_id);
  emit(event_of("chat", {{"lobby_id", lobby_id},
                         {"user_id", actor.user_id},
                         {"alias", member->alias},
                         {"text", text}}),
       connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::custom(const Actor& actor, const std::string& lobby_id,
                            const std::string& name, const nlohmann::json& data,
                            const std::optional<std::string>& target)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_member(actor, *lobby); !status )
  {
    return status;
  }
  const auto* game = rules(lobby->game_id);
  if( !game )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( name.empty() || name.size() > 64 )
  {
    return Err(ErrorCode::BadRequest, "custom name is required");
  }
  if( static_cast<int>(data.dump().size()) > game->limits.max_custom_bytes )
  {
    return Err(ErrorCode::TooLarge, "custom data too large");
  }
  nlohmann::json fields = {
      {"lobby_id", lobby_id}, {"user_id", actor.user_id}, {"name", name}, {"data", data}};
  if( target )
  {
    if( !find_member(*lobby, *target) )
    {
      return Err(ErrorCode::UnknownUser, "target is not in the lobby");
    }
    fields["target_user_id"] = *target;
    const auto* dest = find_member(*lobby, *target);
    if( dest->connected )
    {
      emit(event_of("custom", fields), {*target});
    }
    return true;
  }
  emit(event_of("custom", fields), connected_ids(*lobby, actor.user_id));
  return true;
}

Status LobbyService::heartbeat(const Actor& actor, const std::string& lobby_id, Time now)
{
  auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  if( auto status = require_member(actor, *lobby); !status )
  {
    return status;
  }
  if( actor.user_id == lobby->host_id )
  {
    lobby->last_heartbeat = now;
  }
  return true;
}

Result<nlohmann::json> LobbyService::list(const std::string& game_id, const std::string& cursor,
                                          int limit) const
{
  if( limit < 1 )
  {
    limit = 1;
  }
  if( limit > 200 )
  {
    limit = 200;
  }
  std::vector<const Lobby*> rows;
  for( const auto& [_, lobby] : lobbies_ )
  {
    if( lobby.game_id == game_id && listable(lobby) )
    {
      rows.push_back(&lobby);
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const Lobby* a, const Lobby* b)
            {
              if( a->seq != b->seq )
              {
                return a->seq < b->seq;
              }
              return a->id < b->id;
            });
  std::size_t start = 0;
  if( !cursor.empty() )
  {
    bool found = false;
    for( std::size_t i = 0; i < rows.size(); ++i )
    {
      if( rows[i]->id == cursor )
      {
        start = i + 1;
        found = true;
        break;
      }
    }
    if( !found )
    {
      start = 0;
    }
  }
  nlohmann::json items = nlohmann::json::array();
  std::size_t i = start;
  for( ; i < rows.size() && static_cast<int>(items.size()) < limit; ++i )
  {
    items.push_back(summary(*rows[i]));
  }
  nlohmann::json body = {{"lobbies", items}, {"next_cursor", nullptr}};
  if( i < rows.size() && !items.empty() )
  {
    body["next_cursor"] = items.back().value("lobby_id", "");
  }
  return body;
}

Result<nlohmann::json> LobbyService::get(const std::string& game_id,
                                         const std::string& lobby_id) const
{
  const auto* lobby = find_lobby(lobby_id);
  if( !lobby || lobby->game_id != game_id )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  return summary(*lobby);
}

Result<nlohmann::json> LobbyService::get_by_code(const std::string& game_id,
                                                 const std::string& code) const
{
  auto normalized = normalize_code(code);
  if( !normalized || normalized->empty() )
  {
    return Err(ErrorCode::InvalidCode, "invalid short code");
  }
  const auto game_codes = codes_.find(game_id);
  if( game_codes == codes_.end() )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  const auto found = game_codes->second.find(*normalized);
  if( found == game_codes->second.end() )
  {
    return Err(ErrorCode::NotFound, "lobby not found");
  }
  return get(game_id, found->second);
}

std::optional<std::string> LobbyService::lobby_of(const std::string& user_id) const
{
  const auto it = user_lobby_.find(user_id);
  if( it == user_lobby_.end() )
  {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::string> LobbyService::connected_host(const std::string& lobby_id) const
{
  const auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return std::nullopt;
  }
  const auto* host = find_member(*lobby, lobby->host_id);
  if( !host || !host->connected )
  {
    return std::nullopt;
  }
  return lobby->host_id;
}

bool LobbyService::user_in_lobby(const std::string& user_id) const
{
  return user_lobby_.contains(user_id);
}

bool LobbyService::connected_member(const std::string& lobby_id, const std::string& user_id) const
{
  const auto* lobby = find_lobby(lobby_id);
  if( !lobby )
  {
    return false;
  }
  const auto* member = find_member(*lobby, user_id);
  return member && member->connected;
}

int LobbyService::lobby_count(const std::string& game_id) const
{
  int count = 0;
  for( const auto& [_, lobby] : lobbies_ )
  {
    if( lobby.game_id == game_id )
    {
      ++count;
    }
  }
  return count;
}

void LobbyService::tick(Time now)
{
  std::vector<std::string> expire_users;
  std::vector<std::string> heartbeat_hosts;
  for( auto& [id, lobby] : lobbies_ )
  {
    for( const auto& member : lobby.members )
    {
      if( member.hold != Member::Hold::None && now > member.deadline )
      {
        expire_users.push_back(member.user_id);
      }
    }
    const auto* host = find_member(lobby, lobby.host_id);
    if( host && host->connected )
    {
      const auto limit = timing_.heartbeat_interval * std::max(1, timing_.heartbeat_misses);
      if( now - lobby.last_heartbeat > limit )
      {
        heartbeat_hosts.push_back(lobby.host_id);
      }
    }
  }
  for( const auto& user : expire_users )
  {
    const auto found = user_lobby_.find(user);
    if( found == user_lobby_.end() )
    {
      continue;
    }
    auto* lobby = find_lobby(found->second);
    if( !lobby )
    {
      continue;
    }
    auto* member = find_member(*lobby, user);
    if( member && member->hold != Member::Hold::None && now > member->deadline )
    {
      leave(user, now);
    }
  }
  for( const auto& host : heartbeat_hosts )
  {
    const auto found = user_lobby_.find(host);
    if( found == user_lobby_.end() )
    {
      continue;
    }
    auto* lobby = find_lobby(found->second);
    if( !lobby || lobby->host_id != host )
    {
      continue;
    }
    const auto* member = find_member(*lobby, host);
    if( !member || !member->connected )
    {
      continue;
    }
    const auto limit = timing_.heartbeat_interval * std::max(1, timing_.heartbeat_misses);
    if( now - lobby->last_heartbeat > limit )
    {
      leave(host, now);
    }
  }
}

} // namespace matchmaker
