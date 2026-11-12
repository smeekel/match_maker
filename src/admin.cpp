#include "admin.hpp"

#include "crypto.hpp"
#include "log.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

namespace matchmaker
{
namespace
{

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

AdminReply fail(ErrorCode code, const std::string& message)
{
  return {http_status(code), {{"error", error_code_string(code)}, {"message", message}}};
}

AdminReply fail(const Error& error)
{
  return fail(error.code, error.message);
}

void audit(AdminContext& ctx, const std::string& message, const std::string& game_id = {},
           const json& detail = nullptr)
{
  write_log(&ctx.store, LogLevel::Info, "admin", message, game_id,
            detail.is_null() ? std::string{} : detail.dump());
}

std::vector<std::string> split_path(const std::string& path)
{
  std::vector<std::string> parts;
  std::size_t start = 0;
  while( start <= path.size() )
  {
    auto end = path.find('/', start);
    if( end == std::string::npos )
    {
      end = path.size();
    }
    if( end > start )
    {
      parts.push_back(url_decode(path.substr(start, end - start)));
    }
    start = end + 1;
  }
  return parts;
}

bool valid_id(const std::string& id)
{
  if( id.empty() || id.size() > 64 )
  {
    return false;
  }
  return std::all_of(id.begin(), id.end(),
                     [](unsigned char c)
                     {
                       return std::isalnum(c) || c == '-' || c == '_' || c == '.';
                     });
}

// Config

json config_view(AdminContext& ctx)
{
  const auto stored = ctx.store.load_config();
  const auto live = ctx.catalog.system().to_map();
  const auto boot = ctx.startup.to_map();
  json keys = json::array();
  for( const auto& key : config_keys() )
  {
    const auto stored_it = stored.find(key.name);
    const auto& running = key.restart ? boot : live;
    const auto live_it = running.find(key.name);
    const std::string live_value = live_it == running.end() ? std::string{} : live_it->second;
    json stored_value = nullptr;
    bool pending = false;
    if( stored_it != stored.end() )
    {
      stored_value = stored_it->second;
      pending = stored_it->second != live_value;
    }
    keys.push_back({{"key", key.name},
                    {"kind", config_kind_name(key.kind)},
                    {"apply", key.restart ? "restart" : "reload"},
                    {"stored", stored_value},
                    {"live", live_value},
                    {"pending", pending}});
  }
  return {{"keys", keys}};
}

AdminReply write_config(AdminContext& ctx, const json& body)
{
  if( !body.contains("values") || !body["values"].is_object() || body["values"].empty() )
  {
    return fail(ErrorCode::BadRequest, "values must be a non-empty object");
  }
  std::vector<std::pair<std::string, std::string>> writes;
  for( auto it = body["values"].begin(); it != body["values"].end(); ++it )
  {
    const auto* key = find_config_key(it.key());
    if( !key )
    {
      return fail(ErrorCode::BadRequest, "unknown config key: " + it.key());
    }
    std::string value;
    if( it.value().is_string() )
    {
      value = it.value().get<std::string>();
    }
    else if( it.value().is_number_integer() )
    {
      value = std::to_string(it.value().get<long long>());
    }
    else if( it.value().is_boolean() )
    {
      value = it.value().get<bool>() ? "1" : "0";
    }
    else
    {
      return fail(ErrorCode::BadRequest, it.key() + " must be a string");
    }
    if( auto problem = check_config_value(*key, value) )
    {
      return fail(ErrorCode::BadRequest, *problem);
    }
    writes.emplace_back(it.key(), std::move(value));
  }
  json names = json::array();
  for( const auto& [key, value] : writes )
  {
    ctx.store.set_config(key, value);
    names.push_back(key);
  }
  audit(ctx, "config updated", {}, {{"keys", names}});
  return {200, config_view(ctx)};
}

// Games

json limits_json(const Limits& limits)
{
  return {{"max_players_per_lobby", limits.max_players_per_lobby},
          {"max_password_length", limits.max_password_length},
          {"max_concurrent_channels", limits.max_concurrent_channels},
          {"max_channel_lifetime_sec", limits.max_channel_lifetime_sec},
          {"max_idle_sec", limits.max_idle_sec},
          {"allow_tcp", limits.allow_tcp},
          {"allow_wt", limits.allow_wt},
          {"max_lobbies", limits.max_lobbies},
          {"max_metadata_bytes", limits.max_metadata_bytes},
          {"max_chat_bytes", limits.max_chat_bytes},
          {"max_custom_bytes", limits.max_custom_bytes},
          {"max_alias_bytes", limits.max_alias_bytes}};
}

std::optional<std::string> apply_limits(const json& patch, Limits& limits)
{
  if( !patch.is_object() )
  {
    return "limits must be an object";
  }
  const std::pair<const char*, int*> ints[] = {
      {"max_players_per_lobby", &limits.max_players_per_lobby},
      {"max_password_length", &limits.max_password_length},
      {"max_concurrent_channels", &limits.max_concurrent_channels},
      {"max_channel_lifetime_sec", &limits.max_channel_lifetime_sec},
      {"max_idle_sec", &limits.max_idle_sec},
      {"max_lobbies", &limits.max_lobbies},
      {"max_metadata_bytes", &limits.max_metadata_bytes},
      {"max_chat_bytes", &limits.max_chat_bytes},
      {"max_custom_bytes", &limits.max_custom_bytes},
      {"max_alias_bytes", &limits.max_alias_bytes}};
  const std::pair<const char*, bool*> flags[] = {{"allow_tcp", &limits.allow_tcp},
                                                 {"allow_wt", &limits.allow_wt}};
  for( auto it = patch.begin(); it != patch.end(); ++it )
  {
    bool known = false;
    for( const auto& [name, dest] : ints )
    {
      if( it.key() == name )
      {
        known = true;
        if( !it.value().is_number_integer() || it.value().get<long long>() < 0 ||
            it.value().get<long long>() > std::numeric_limits<int>::max() )
        {
          return it.key() + " must be a non-negative integer";
        }
        *dest = it.value().get<int>();
      }
    }
    for( const auto& [name, dest] : flags )
    {
      if( it.key() == name )
      {
        known = true;
        if( !it.value().is_boolean() )
        {
          return it.key() + " must be true or false";
        }
        *dest = it.value().get<bool>();
      }
    }
    if( !known )
    {
      return "unknown limit: " + it.key();
    }
  }
  return std::nullopt;
}

bool same_game(const GameRecord& a, const GameRecord& b)
{
  return a.name == b.name && a.token_hash == b.token_hash &&
         a.promote_host_on_leave == b.promote_host_on_leave &&
         a.allow_join_in_progress == b.allow_join_in_progress &&
         limits_json(a.limits) == limits_json(b.limits);
}

json game_json(const GameRecord& game)
{
  return {{"id", game.id},
          {"name", game.name},
          {"promote_host_on_leave", game.promote_host_on_leave},
          {"allow_join_in_progress", game.allow_join_in_progress},
          {"limits", limits_json(game.limits)},
          {"created_at_ms", game.created_at_ms}};
}

json games_view(AdminContext& ctx)
{
  const auto stored = ctx.store.load_games();
  std::set<std::string> seen;
  json rows = json::array();
  for( const auto& game : stored )
  {
    seen.insert(game.id);
    auto row = game_json(game);
    row["stored"] = true;
    row["lobbies"] = ctx.lobbies.lobby_count(game.id);
    const auto* live = ctx.catalog.game(game.id);
    if( live )
    {
      auto expected = game;
      clamp_game(expected, ctx.catalog.system());
      row["live"] = game_json(*live);
      row["pending"] = !same_game(expected, *live);
    }
    else
    {
      row["live"] = nullptr;
      row["pending"] = true;
    }
    rows.push_back(std::move(row));
  }
  for( const auto& live : ctx.catalog.games() )
  {
    if( seen.contains(live.id) )
    {
      continue;
    }
    auto row = game_json(live);
    row["stored"] = false;
    row["lobbies"] = ctx.lobbies.lobby_count(live.id);
    row["live"] = game_json(live);
    row["pending"] = true;
    rows.push_back(std::move(row));
  }
  return {{"games", rows}};
}

AdminReply write_game(AdminContext& ctx, const json& body)
{
  if( !body.is_object() )
  {
    return fail(ErrorCode::BadRequest, "body must be an object");
  }
  std::string id;
  if( body.contains("id") && !body["id"].is_null() )
  {
    if( !body["id"].is_string() || !valid_id(body["id"].get<std::string>()) )
    {
      return fail(ErrorCode::BadRequest, "id must be 1 to 64 letters, digits, '-', '_', or '.'");
    }
    id = body["id"].get<std::string>();
  }
  GameRecord game;
  bool created = true;
  if( !id.empty() )
  {
    for( const auto& existing : ctx.store.load_games() )
    {
      if( existing.id == id )
      {
        game = existing;
        created = false;
        break;
      }
    }
  }
  game.id = id.empty() ? uuid_v4() : id;
  if( body.contains("name") )
  {
    if( !body["name"].is_string() || body["name"].get<std::string>().empty() )
    {
      return fail(ErrorCode::BadRequest, "name must be a non-empty string");
    }
    game.name = body["name"].get<std::string>();
  }
  if( game.name.empty() )
  {
    return fail(ErrorCode::BadRequest, "name is required");
  }
  if( body.contains("token") && !body["token"].is_null() )
  {
    if( !body["token"].is_string() || body["token"].get<std::string>().empty() )
    {
      return fail(ErrorCode::BadRequest, "token must be a non-empty string");
    }
    game.token_hash = sha256_hex(body["token"].get<std::string>());
  }
  if( game.token_hash.empty() )
  {
    return fail(ErrorCode::BadRequest, "token is required for a new game");
  }
  for( const auto& [name, dest] : {std::pair<const char*, bool*>{"promote_host_on_leave",
                                                                 &game.promote_host_on_leave},
                                   std::pair<const char*, bool*>{"allow_join_in_progress",
                                                                 &game.allow_join_in_progress}} )
  {
    if( body.contains(name) )
    {
      if( !body[name].is_boolean() )
      {
        return fail(ErrorCode::BadRequest, std::string(name) + " must be true or false");
      }
      *dest = body[name].get<bool>();
    }
  }
  if( body.contains("limits") )
  {
    if( auto problem = apply_limits(body["limits"], game.limits) )
    {
      return fail(ErrorCode::BadRequest, *problem);
    }
  }
  ctx.store.upsert_game(game);
  audit(ctx, created ? "game created" : "game updated", game.id);
  for( const auto& saved : ctx.store.load_games() )
  {
    if( saved.id == game.id )
    {
      game = saved;
    }
  }
  return {200, {{"game", game_json(game)}, {"created", created}}};
}

AdminReply delete_game(AdminContext& ctx, const std::string& id)
{
  if( const int live = ctx.lobbies.lobby_count(id); live > 0 )
  {
    return fail(ErrorCode::Conflict,
                "game has " + std::to_string(live) + " live lobbies; close them first");
  }
  if( !ctx.store.delete_game(id) )
  {
    return fail(ErrorCode::NotFound, "game not found");
  }
  audit(ctx, "game deleted", id);
  return {200, {{"deleted", id}}};
}

// Regions

json region_json(const Region& region)
{
  return {{"id", region.id}, {"public_url", region.public_url}, {"sort_order", region.sort_order}};
}

json regions_view(AdminContext& ctx)
{
  const auto stored = ctx.store.load_regions();
  const auto& live = ctx.catalog.regions();
  const auto& current = ctx.catalog.system().region_id;
  auto find_live = [&](const std::string& id) -> const Region*
  {
    for( const auto& region : live )
    {
      if( region.id == id )
      {
        return &region;
      }
    }
    return nullptr;
  };
  std::set<std::string> seen;
  json rows = json::array();
  for( const auto& region : stored )
  {
    seen.insert(region.id);
    auto row = region_json(region);
    const auto* running = find_live(region.id);
    row["stored"] = true;
    row["live"] = running ? region_json(*running) : json(nullptr);
    row["pending"] = !running || running->public_url != region.public_url ||
                     running->sort_order != region.sort_order;
    row["current"] = region.id == current;
    rows.push_back(std::move(row));
  }
  for( const auto& region : live )
  {
    if( seen.contains(region.id) )
    {
      continue;
    }
    auto row = region_json(region);
    row["stored"] = false;
    row["live"] = region_json(region);
    row["pending"] = true;
    row["current"] = region.id == current;
    rows.push_back(std::move(row));
  }
  return {{"regions", rows}, {"region_id", current}};
}

AdminReply write_region(AdminContext& ctx, const json& body)
{
  if( !body.is_object() || !body.contains("id") || !body["id"].is_string() ||
      !valid_id(body["id"].get<std::string>()) )
  {
    return fail(ErrorCode::BadRequest, "id must be 1 to 64 letters, digits, '-', '_', or '.'");
  }
  if( !body.contains("public_url") || !body["public_url"].is_string() ||
      body["public_url"].get<std::string>().empty() )
  {
    return fail(ErrorCode::BadRequest, "public_url must be a non-empty string");
  }
  Region region;
  region.id = body["id"].get<std::string>();
  region.public_url = body["public_url"].get<std::string>();
  for( const auto& existing : ctx.store.load_regions() )
  {
    if( existing.id == region.id )
    {
      region.sort_order = existing.sort_order;
    }
  }
  if( body.contains("sort_order") )
  {
    if( !body["sort_order"].is_number_integer() )
    {
      return fail(ErrorCode::BadRequest, "sort_order must be an integer");
    }
    region.sort_order = body["sort_order"].get<int>();
  }
  ctx.store.upsert_region(region);
  audit(ctx, "region saved", {}, {{"region_id", region.id}});
  return {200, {{"region", region_json(region)}}};
}

AdminReply delete_region(AdminContext& ctx, const std::string& id)
{
  const auto stored = ctx.store.load_config();
  const auto stored_region = stored.find("region_id");
  if( id == ctx.catalog.system().region_id ||
      (stored_region != stored.end() && stored_region->second == id) )
  {
    return fail(ErrorCode::Conflict, "region_id names this region; change region_id first");
  }
  if( !ctx.store.delete_region(id) )
  {
    return fail(ErrorCode::NotFound, "region not found");
  }
  audit(ctx, "region deleted", {}, {{"region_id", id}});
  return {200, {{"deleted", id}}};
}

// Lobbies

json channels_json(const std::vector<ChannelSummary>& channels, Clock::time_point now)
{
  json rows = json::array();
  for( const auto& channel : channels )
  {
    auto side = [&](const std::string& user_id, int port)
    {
      json out = {{"user_id", user_id}};
      if( port != 0 )
      {
        out["port"] = port;
      }
      return out;
    };
    const auto left =
        std::chrono::duration_cast<std::chrono::seconds>(channel.expires - now).count();
    rows.push_back({{"id", channel.id},
                    {"protocol", channel.protocol},
                    {"a", side(channel.user_a, channel.port_a)},
                    {"b", side(channel.user_b, channel.port_b)},
                    {"expires_in_sec", std::max<long long>(0, left)},
                    {"closing", channel.closing}});
  }
  return rows;
}

json lobbies_view(AdminContext& ctx)
{
  std::unordered_map<std::string, int> counts;
  for( const auto& channel : ctx.relay.list() )
  {
    ++counts[channel.lobby_id];
  }
  auto rows = ctx.lobbies.admin_list();
  for( auto& row : rows )
  {
    const auto it = counts.find(row.value("lobby_id", ""));
    row["channel_count"] = it == counts.end() ? 0 : it->second;
  }
  return {{"lobbies", rows}};
}

Result<json> lobby_view(AdminContext& ctx, const std::string& id)
{
  auto view = ctx.lobbies.admin_view(id);
  if( !view )
  {
    return Unexpected{view.error()};
  }
  const auto channels = ctx.relay.list(id);
  (*view)["channels"] = channels_json(channels, Clock::now());
  (*view)["channel_count"] = channels.size();
  return *view;
}

// False when |key| is present and neither a string nor null.
bool nullable_string(const json& body, const char* key, std::optional<std::string>& out)
{
  if( !body.contains(key) || body[key].is_null() )
  {
    out.reset();
    return true;
  }
  if( !body[key].is_string() )
  {
    return false;
  }
  out = body[key].get<std::string>();
  return true;
}

AdminReply lobby_op(AdminContext& ctx, const std::string& id, const json& body)
{
  const auto before = ctx.lobbies.admin_view(id);
  if( !before )
  {
    return fail(before.error());
  }
  const auto game_id = before->value("game_id", "");
  const auto op = body.is_object() ? body.value("op", "") : std::string{};
  const auto now = Clock::now();
  json detail = {{"lobby_id", id}};
  Status status = true;
  if( op == "set_phase" )
  {
    if( !body.contains("phase") || !body["phase"].is_string() )
    {
      return fail(ErrorCode::BadRequest, "phase must be a string");
    }
    status = ctx.lobbies.admin_set_phase(id, body["phase"].get<std::string>());
  }
  else if( op == "set_slots" )
  {
    if( !body.contains("slots") || !body["slots"].is_number_integer() )
    {
      return fail(ErrorCode::BadRequest, "slots must be an integer");
    }
    status = ctx.lobbies.admin_set_slots(id, body["slots"].get<int>());
  }
  else if( op == "set_late_join" )
  {
    if( !body.contains("allowed") || !body["allowed"].is_boolean() )
    {
      return fail(ErrorCode::BadRequest, "allowed must be true or false");
    }
    status = ctx.lobbies.admin_set_late_join(id, body["allowed"].get<bool>());
  }
  else if( op == "set_password" )
  {
    std::optional<std::string> password;
    if( !nullable_string(body, "password", password) )
    {
      return fail(ErrorCode::BadRequest, "password must be a string or null");
    }
    status = ctx.lobbies.admin_set_password(id, password);
  }
  else if( op == "set_short_code" )
  {
    std::optional<std::string> code;
    if( !nullable_string(body, "code", code) )
    {
      return fail(ErrorCode::BadRequest, "code must be a string or null");
    }
    status = ctx.lobbies.admin_set_short_code(id, code);
  }
  else if( op == "set_metadata" )
  {
    if( !body.contains("metadata") || !body["metadata"].is_object() )
    {
      return fail(ErrorCode::BadRequest, "metadata must be an object");
    }
    status = ctx.lobbies.admin_set_metadata(id, body["metadata"]);
  }
  else if( op == "kick" )
  {
    if( !body.contains("user_id") || !body["user_id"].is_string() )
    {
      return fail(ErrorCode::BadRequest, "user_id must be a string");
    }
    const auto user_id = body["user_id"].get<std::string>();
    detail["user_id"] = user_id;
    status = ctx.lobbies.admin_kick(id, user_id, now);
  }
  else if( op == "close" )
  {
    status = ctx.lobbies.admin_close(id);
  }
  else if( op == "release_channel" )
  {
    if( !body.contains("channel_id") || !body["channel_id"].is_string() )
    {
      return fail(ErrorCode::BadRequest, "channel_id must be a string");
    }
    const auto channel_id = body["channel_id"].get<std::string>();
    const auto channels = ctx.relay.list(id);
    const bool owned = std::any_of(channels.begin(), channels.end(),
                                   [&](const ChannelSummary& channel)
                                   {
                                     return channel.id == channel_id;
                                   });
    if( !owned )
    {
      return fail(ErrorCode::NotFound, "channel not found in this lobby");
    }
    detail["channel_id"] = channel_id;
    status = ctx.relay.release(channel_id, now);
  }
  else if( op == "release_channels" )
  {
    ctx.relay.release_lobby(id, now);
  }
  else
  {
    return fail(ErrorCode::BadRequest, "unknown op");
  }
  if( !status )
  {
    return fail(status.error());
  }
  audit(ctx, "lobby " + op, game_id, detail);
  auto after = lobby_view(ctx, id);
  return {200, {{"ok", true}, {"lobby", after ? *after : json(nullptr)}}};
}

AdminReply route(AdminContext& ctx, const std::string& method, const std::string& path,
                 const json& body)
{
  const auto parts = split_path(path);
  const bool get = method == "GET";
  const bool post = method == "POST";
  if( parts.size() == 1 && parts[0] == "config" )
  {
    if( get )
    {
      return {200, config_view(ctx)};
    }
    if( post )
    {
      return write_config(ctx, body);
    }
  }
  if( parts.size() == 1 && parts[0] == "reload" && post )
  {
    if( auto problem = ctx.reload() )
    {
      return fail(ErrorCode::Internal, "reload failed: " + *problem);
    }
    audit(ctx, "reload");
    return {200, {{"reloaded", true}}};
  }
  if( !parts.empty() && parts[0] == "games" )
  {
    if( parts.size() == 1 && get )
    {
      return {200, games_view(ctx)};
    }
    if( parts.size() == 1 && post )
    {
      return write_game(ctx, body);
    }
    if( parts.size() == 3 && parts[2] == "delete" && post )
    {
      return delete_game(ctx, parts[1]);
    }
  }
  if( !parts.empty() && parts[0] == "regions" )
  {
    if( parts.size() == 1 && get )
    {
      return {200, regions_view(ctx)};
    }
    if( parts.size() == 1 && post )
    {
      return write_region(ctx, body);
    }
    if( parts.size() == 3 && parts[2] == "delete" && post )
    {
      return delete_region(ctx, parts[1]);
    }
  }
  if( !parts.empty() && parts[0] == "lobbies" )
  {
    if( parts.size() == 1 && get )
    {
      return {200, lobbies_view(ctx)};
    }
    if( parts.size() == 2 && get )
    {
      auto view = lobby_view(ctx, parts[1]);
      if( !view )
      {
        return fail(view.error());
      }
      return {200, *view};
    }
    if( parts.size() == 2 && post )
    {
      return lobby_op(ctx, parts[1], body);
    }
  }
  return fail(ErrorCode::NotFound, "not found");
}

} // namespace

AdminReply handle_admin_api(AdminContext& ctx, const std::string& method, const std::string& path,
                            const nlohmann::json& body)
{
  try
  {
    return route(ctx, method, path, body);
  }
  catch( const std::exception& ex )
  {
    write_log(&ctx.store, LogLevel::Error, "admin", ex.what());
    return fail(ErrorCode::Internal, ex.what());
  }
}

} // namespace matchmaker
