#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace matchmaker {

std::string sha256_hex(std::string_view data);
std::string hmac_sha256(std::string_view key, std::string_view data);
bool ct_equal(std::string_view a, std::string_view b);
void random_bytes(std::span<std::uint8_t> out);
std::string random_hex(std::size_t bytes);
std::string uuid_v4();

}  // namespace matchmaker
