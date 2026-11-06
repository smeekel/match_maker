#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <filesystem>

namespace matchmaker {

std::int64_t wall_ms();
std::string hex_encode(std::string_view bytes);
std::string hex_encode(const std::uint8_t* data, std::size_t n);
std::optional<std::string> hex_decode(std::string_view hex);
std::string b64url_encode(std::string_view data);
std::optional<std::string> b64url_decode(std::string_view data);
std::string url_decode(std::string_view in);
std::optional<std::pair<std::string, std::uint16_t>> parse_host_port(std::string_view text);
std::filesystem::path executable_dir();
std::filesystem::path migrations_dir();
std::string gzip_compress(std::string_view input);
std::optional<std::string> gzip_decompress(std::string_view input);
int env_int(const char* key, int fallback);

}  // namespace matchmaker
