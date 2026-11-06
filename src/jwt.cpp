#include "jwt.hpp"

#include "crypto.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

namespace matchmaker {
namespace {

std::string b64_text(std::string_view raw) { return b64url_encode(raw); }

}  // namespace

std::string jwt_sign(const JwtClaims& claims, std::string_view secret) {
  nlohmann::json header = {{"alg", "HS256"}, {"typ", "JWT"}};
  nlohmann::json payload = {{"sub", claims.sub},     {"game_id", claims.game_id}, {"jti", claims.jti},
                            {"boot_id", claims.boot_id}, {"iat", claims.iat},     {"exp", claims.exp}};
  const std::string h = b64_text(header.dump());
  const std::string p = b64_text(payload.dump());
  const std::string signing = h + "." + p;
  const std::string mac = hmac_sha256(secret, signing);
  return signing + "." + b64url_encode(mac);
}

Result<JwtClaims> jwt_verify(std::string_view token, std::string_view secret, std::int64_t now_sec) {
  const auto first = token.find('.');
  const auto second = token.find('.', first == std::string_view::npos ? 0 : first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos) {
    return Err(ErrorCode::Unauthorized, "malformed token");
  }
  const auto signing = token.substr(0, second);
  const auto sig = token.substr(second + 1);
  const std::string mac = hmac_sha256(secret, signing);
  const std::string expect = b64url_encode(mac);
  if (!ct_equal(expect, sig)) {
    return Err(ErrorCode::Unauthorized, "bad signature");
  }
  auto payload_raw = b64url_decode(token.substr(first + 1, second - first - 1));
  if (!payload_raw) {
    return Err(ErrorCode::Unauthorized, "bad payload");
  }
  auto payload = nlohmann::json::parse(*payload_raw, nullptr, false);
  if (!payload.is_object()) {
    return Err(ErrorCode::Unauthorized, "bad payload");
  }
  JwtClaims claims;
  claims.sub = payload.value("sub", "");
  claims.game_id = payload.value("game_id", "");
  claims.jti = payload.value("jti", "");
  claims.boot_id = payload.value("boot_id", "");
  claims.iat = payload.value("iat", static_cast<std::int64_t>(0));
  claims.exp = payload.value("exp", static_cast<std::int64_t>(0));
  if (claims.sub.empty() || claims.game_id.empty() || claims.jti.empty() || claims.boot_id.empty()) {
    return Err(ErrorCode::Unauthorized, "missing claims");
  }
  if (now_sec >= claims.exp) {
    return Err(ErrorCode::Unauthorized, "token expired");
  }
  return claims;
}

}  // namespace matchmaker
