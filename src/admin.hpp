#pragma once

#include "catalog.hpp"
#include "lobby.hpp"
#include "relay.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>

namespace matchmaker
{

// Everything the operator API reads or changes. Every call must run on the app strand.
struct AdminContext
{
  Store& store;
  const Catalog& catalog;
  // The config the process bound at startup; restart-only keys report this as live.
  const SystemConfig& startup;
  LobbyService& lobbies;
  RelayService& relay;
  // Reloads config, regions, and games. Returns an error message on failure.
  std::function<std::optional<std::string>()> reload;
};

struct AdminReply
{
  int status = 200;
  nlohmann::json body = nlohmann::json::object();
};

// |path| is the part after /admin/api/v1, for example "/config" or "/lobbies/<id>".
AdminReply handle_admin_api(AdminContext& ctx, const std::string& method, const std::string& path,
                            const nlohmann::json& body);

} // namespace matchmaker
