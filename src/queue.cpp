#include "queue.hpp"

#include "crypto.hpp"

#include <algorithm>

namespace matchmaker
{

QueueService::Ticket* QueueService::find(const std::string& id)
{
  const auto it = tickets_.find(id);
  return it == tickets_.end() ? nullptr : &it->second;
}

const QueueService::Ticket* QueueService::find(const std::string& id) const
{
  const auto it = tickets_.find(id);
  return it == tickets_.end() ? nullptr : &it->second;
}

int QueueService::position_of(const Ticket& ticket) const
{
  int position = 1;
  for( const auto& [_, other] : tickets_ )
  {
    if( other.state == Ticket::State::Queued && other.game_id == ticket.game_id &&
        other.mode == ticket.mode && other.seq < ticket.seq )
    {
      ++position;
    }
  }
  return position;
}

nlohmann::json QueueService::view(const Ticket& ticket) const
{
  nlohmann::json body = {{"ticket_id", ticket.id}};
  if( ticket.state == Ticket::State::Queued )
  {
    body["status"] = "queued";
    body["position"] = position_of(ticket);
  }
  else if( ticket.state == Ticket::State::Ready )
  {
    body["status"] = "match_ready";
    body["lobby_id"] = ticket.lobby_id;
    body["position"] = 0;
  }
  else
  {
    body["status"] = "cancelled";
  }
  return body;
}

void QueueService::fire(Ticket& ticket)
{
  auto waiters = std::move(ticket.waiters);
  ticket.waiters.clear();
  for( auto& waiter : waiters )
  {
    waiter();
  }
}

Status QueueService::try_match(const GameRecord& game, const std::string& mode, int players,
                               Time now)
{
  if( !match_fn_ || players < 1 )
  {
    return Err(ErrorCode::ModeNotConfigured, "mode is not configured");
  }
  while( true )
  {
    std::vector<Ticket*> queued;
    for( auto& [_, ticket] : tickets_ )
    {
      if( ticket.state == Ticket::State::Queued && ticket.game_id == game.id &&
          ticket.mode == mode )
      {
        queued.push_back(&ticket);
      }
    }
    if( static_cast<int>(queued.size()) < players )
    {
      return true;
    }
    std::sort(queued.begin(), queued.end(),
              [](const Ticket* a, const Ticket* b)
              {
                return a->seq < b->seq;
              });
    std::vector<Actor> actors;
    std::vector<std::string> ids;
    actors.reserve(static_cast<std::size_t>(players));
    for( int i = 0; i < players; ++i )
    {
      Actor actor;
      actor.user_id = queued[i]->user_id;
      actor.game_id = queued[i]->game_id;
      actor.alias = queued[i]->alias;
      actor.external_id = queued[i]->external_id;
      actors.push_back(std::move(actor));
      ids.push_back(queued[i]->id);
    }
    auto created = match_fn_(game.id, actors, now);
    if( !created )
    {
      return std::unexpected(created.error());
    }
    for( const auto& id : ids )
    {
      auto* ticket = find(id);
      if( !ticket )
      {
        continue;
      }
      ticket->state = Ticket::State::Ready;
      ticket->lobby_id = *created;
      fire(*ticket);
    }
  }
}

Result<nlohmann::json> QueueService::enqueue(const Actor& actor, const GameRecord& game,
                                             const std::string& mode, Time now)
{
  if( actor.game_id != game.id )
  {
    return Err(ErrorCode::UnknownGame, "unknown game");
  }
  if( user_ticket_.contains(actor.user_id) )
  {
    return Err(ErrorCode::AlreadyQueued, "already queued");
  }
  const Mode* found = nullptr;
  for( const auto& item : game.modes )
  {
    if( item.name == mode )
    {
      found = &item;
      break;
    }
  }
  if( !found )
  {
    return Err(ErrorCode::ModeNotConfigured, "mode is not configured");
  }
  if( found->players > game.limits.max_players_per_lobby )
  {
    return Err(ErrorCode::Limit, "mode exceeds the player cap");
  }
  Ticket ticket;
  ticket.id = uuid_v4();
  ticket.user_id = actor.user_id;
  ticket.game_id = actor.game_id;
  ticket.mode = mode;
  ticket.alias = actor.alias;
  ticket.external_id = actor.external_id;
  ticket.seq = next_seq_++;
  const auto id = ticket.id;
  tickets_.emplace(id, std::move(ticket));
  user_ticket_[actor.user_id] = id;
  if( auto matched = try_match(game, mode, found->players, now); !matched )
  {
    if( auto* mine = find(id); mine && mine->state == Ticket::State::Queued )
    {
      mine->state = Ticket::State::Cancelled;
      user_ticket_.erase(actor.user_id);
      fire(*mine);
    }
    return std::unexpected(matched.error());
  }
  return view(tickets_.at(id));
}

Result<nlohmann::json> QueueService::status(const Actor& actor, const std::string& ticket_id) const
{
  const auto* ticket = find(ticket_id);
  if( !ticket || ticket->user_id != actor.user_id || ticket->game_id != actor.game_id )
  {
    return Err(ErrorCode::NotFound, "ticket not found");
  }
  return view(*ticket);
}

void QueueService::add_waiter(const Actor& actor, const std::string& ticket_id, Waiter waiter)
{
  auto* ticket = find(ticket_id);
  if( !ticket || ticket->user_id != actor.user_id )
  {
    waiter();
    return;
  }
  if( ticket->state != Ticket::State::Queued )
  {
    waiter();
    return;
  }
  ticket->waiters.push_back(std::move(waiter));
}

Status QueueService::cancel(const Actor& actor, const std::string& ticket_id, LobbyService& lobbies,
                            Time now)
{
  auto* ticket = find(ticket_id);
  if( !ticket || ticket->user_id != actor.user_id || ticket->game_id != actor.game_id )
  {
    return Err(ErrorCode::NotFound, "ticket not found");
  }
  if( ticket->state == Ticket::State::Cancelled )
  {
    return true;
  }
  const auto lobby_id = ticket->lobby_id;
  const bool ready = ticket->state == Ticket::State::Ready;
  ticket->state = Ticket::State::Cancelled;
  const auto user = user_ticket_.find(actor.user_id);
  if( user != user_ticket_.end() && user->second == ticket_id )
  {
    user_ticket_.erase(user);
  }
  fire(*ticket);
  if( ready && !lobby_id.empty() )
  {
    lobbies.drop_unconnected(actor.user_id, now);
  }
  return true;
}

void QueueService::cancel_queued_user(const std::string& user_id)
{
  const auto found = user_ticket_.find(user_id);
  if( found == user_ticket_.end() )
  {
    return;
  }
  auto* ticket = find(found->second);
  if( !ticket || ticket->state != Ticket::State::Queued )
  {
    return;
  }
  ticket->state = Ticket::State::Cancelled;
  user_ticket_.erase(found);
  fire(*ticket);
}

void QueueService::on_lobby_destroyed(const std::string& lobby_id)
{
  std::vector<std::string> ids;
  for( const auto& [id, ticket] : tickets_ )
  {
    if( ticket.lobby_id == lobby_id && ticket.state == Ticket::State::Ready )
    {
      ids.push_back(id);
    }
  }
  for( const auto& id : ids )
  {
    auto* ticket = find(id);
    if( !ticket )
    {
      continue;
    }
    ticket->state = Ticket::State::Cancelled;
    const auto user = user_ticket_.find(ticket->user_id);
    if( user != user_ticket_.end() && user->second == id )
    {
      user_ticket_.erase(user);
    }
    fire(*ticket);
  }
}

bool QueueService::has_open_ticket(const std::string& user_id) const
{
  return user_ticket_.contains(user_id);
}

int QueueService::queued_count(const std::string& game_id) const
{
  int count = 0;
  for( const auto& [_, ticket] : tickets_ )
  {
    if( ticket.game_id == game_id && ticket.state == Ticket::State::Queued )
    {
      ++count;
    }
  }
  return count;
}

} // namespace matchmaker
