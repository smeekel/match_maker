#pragma once

#include "result.hpp"

#include <cstdint>
#include <string>

namespace matchmaker {

struct JwtClaims {
  std::string sub;
  std::string game_id;
  std::string jti;
  std::string boot_id;
  std::int64_t iat = 0;
  std::int64_t exp = 0;
};

std::string jwt_sign(const JwtClaims& claims, std::string_view secret);
Result<JwtClaims> jwt_verify(std::string_view token, std::string_view secret, std::int64_t now_sec);

}  // namespace matchmaker
