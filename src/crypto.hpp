#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace matchmaker
{

std::string sha256_hex(std::string_view data);
std::string hmac_sha256(std::string_view key, std::string_view data);
bool ct_equal(std::string_view a, std::string_view b);
void random_bytes(std::span<std::uint8_t> out);
std::string random_hex(std::size_t bytes);
std::string uuid_v4();
std::string uuid_string(std::span<const std::uint8_t, 16> bytes);
std::optional<std::array<std::uint8_t, 16>> uuid_bytes(std::string_view text);

} // namespace matchmaker
