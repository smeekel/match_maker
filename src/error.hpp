#pragma once

#include <optional>
#include <string>
#include <cstdint>

namespace matchmaker {

enum class ErrorCode {
  BadRequest,
  Unauthorized,
  UnknownGame,
  NotHost,
  NotMember,
  LobbyNotOpen,
  LateJoinDisabled,
  Full,
  BadPassword,
  InvalidCode,
  CodeTaken,
  Limit,
  LifetimeExceedsLimit,
  TooLarge,
  UnknownUser,
  ModeNotConfigured,
  AlreadyInLobby,
  AlreadyQueued,
  NotFound,
  Internal
};

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string message;
  std::optional<std::int64_t> max_lifetime_sec;
};

const char* error_code_string(ErrorCode code);
int http_status(ErrorCode code);

}  // namespace matchmaker
