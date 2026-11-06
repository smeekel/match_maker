#include "error.hpp"

namespace matchmaker
{

const char* error_code_string(ErrorCode code)
{
  switch( code )
  {
  case ErrorCode::BadRequest:
    return "bad_request";
  case ErrorCode::Unauthorized:
    return "unauthorized";
  case ErrorCode::UnknownGame:
    return "unknown_game";
  case ErrorCode::NotHost:
    return "not_host";
  case ErrorCode::NotMember:
    return "not_member";
  case ErrorCode::LobbyNotOpen:
    return "lobby_not_open";
  case ErrorCode::LateJoinDisabled:
    return "late_join_disabled";
  case ErrorCode::Full:
    return "full";
  case ErrorCode::BadPassword:
    return "bad_password";
  case ErrorCode::InvalidCode:
    return "invalid_code";
  case ErrorCode::CodeTaken:
    return "code_taken";
  case ErrorCode::Limit:
    return "limit";
  case ErrorCode::LifetimeExceedsLimit:
    return "lifetime_exceeds_limit";
  case ErrorCode::TooLarge:
    return "too_large";
    case ErrorCode::UnknownUser:
      return "unknown_user";
    case ErrorCode::AlreadyInLobby:
      return "already_in_lobby";
  case ErrorCode::NotFound:
    return "not_found";
  case ErrorCode::Internal:
    return "internal";
  }
  return "internal";
}

int http_status(ErrorCode code)
{
  switch( code )
  {
  case ErrorCode::BadRequest:
  case ErrorCode::InvalidCode:
    return 400;
  case ErrorCode::Unauthorized:
  case ErrorCode::UnknownGame:
  case ErrorCode::BadPassword:
    return 401;
  case ErrorCode::NotHost:
  case ErrorCode::NotMember:
    return 403;
  case ErrorCode::NotFound:
    case ErrorCode::UnknownUser:
      return 404;
  case ErrorCode::LobbyNotOpen:
  case ErrorCode::LateJoinDisabled:
  case ErrorCode::Full:
  case ErrorCode::CodeTaken:
  case ErrorCode::Limit:
  case ErrorCode::LifetimeExceedsLimit:
    case ErrorCode::AlreadyInLobby:
      return 409;
  case ErrorCode::TooLarge:
    return 413;
  case ErrorCode::Internal:
    return 500;
  }
  return 500;
}

} // namespace matchmaker
