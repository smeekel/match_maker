#include "util.hpp"

#include <zlib.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace matchmaker {

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string hex_encode(const std::uint8_t* data, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(n * 2, '\0');
  for (std::size_t i = 0; i < n; ++i) {
    out[i * 2] = kHex[data[i] >> 4];
    out[i * 2 + 1] = kHex[data[i] & 0x0f];
  }
  return out;
}

std::string hex_encode(std::string_view bytes) {
  return hex_encode(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

std::optional<std::string> hex_decode(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    return std::nullopt;
  }
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out(hex.size() / 2, '\0');
  for (std::size_t i = 0; i < out.size(); ++i) {
    const int hi = val(hex[i * 2]);
    const int lo = val(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out[i] = static_cast<char>((hi << 4) | lo);
  }
  return out;
}

std::string b64url_encode(std::string_view data) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::uint32_t buffer = 0;
  int bits = 0;
  for (unsigned char c : data) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(kTable[(buffer >> bits) & 0x3f]);
    }
  }
  if (bits > 0) {
    out.push_back(kTable[(buffer << (6 - bits)) & 0x3f]);
  }
  return out;
}

std::optional<std::string> b64url_decode(std::string_view data) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
  };
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (char c : data) {
    if (c == '=') {
      break;
    }
    const int v = val(c);
    if (v < 0) {
      return std::nullopt;
    }
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

std::string url_decode(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%' && i + 2 < in.size()) {
      auto hex = hex_decode(in.substr(i + 1, 2));
      if (hex && hex->size() == 1) {
        out.push_back((*hex)[0]);
        i += 2;
        continue;
      }
    }
    if (in[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

std::optional<std::pair<std::string, std::uint16_t>> parse_host_port(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::string host;
  std::string port_text;
  if (text.front() == '[') {
    const auto end = text.find(']');
    if (end == std::string_view::npos || end + 1 >= text.size() || text[end + 1] != ':') {
      return std::nullopt;
    }
    host = std::string(text.substr(1, end - 1));
    port_text = std::string(text.substr(end + 2));
  } else {
    const auto pos = text.rfind(':');
    if (pos == std::string_view::npos) {
      return std::nullopt;
    }
    host = std::string(text.substr(0, pos));
    port_text = std::string(text.substr(pos + 1));
  }
  if (host.empty() || port_text.empty()) {
    return std::nullopt;
  }
  int port = 0;
  try {
    port = std::stoi(port_text);
  } catch (...) {
    return std::nullopt;
  }
  if (port < 0 || port > 65535) {
    return std::nullopt;
  }
  return std::make_pair(host, static_cast<std::uint16_t>(port));
}

std::filesystem::path executable_dir() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return std::filesystem::current_path();
  }
  return std::filesystem::path(buf).parent_path();
#else
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return std::filesystem::current_path();
  }
  buf[n] = '\0';
  return std::filesystem::path(buf).parent_path();
#endif
}

std::filesystem::path migrations_dir() {
  if (const char* env = std::getenv("MATCHMAKER_MIGRATIONS")) {
    if (env[0] != '\0') {
      return std::filesystem::path(env);
    }
  }
  const auto beside = executable_dir() / "migrations" / "sqlite";
  if (std::filesystem::exists(beside)) {
    return beside;
  }
#ifdef MATCHMAKER_SOURCE_DIR
  return std::filesystem::path(MATCHMAKER_SOURCE_DIR) / "migrations" / "sqlite";
#else
  return beside;
#endif
}

std::string gzip_compress(std::string_view input) {
  z_stream stream{};
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return {};
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  std::string out;
  char buf[4096];
  int ret = Z_OK;
  while (ret == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buf);
    stream.avail_out = sizeof(buf);
    ret = deflate(&stream, Z_FINISH);
    out.append(buf, sizeof(buf) - stream.avail_out);
  }
  deflateEnd(&stream);
  if (ret != Z_STREAM_END) {
    return {};
  }
  return out;
}

std::optional<std::string> gzip_decompress(std::string_view input) {
  z_stream stream{};
  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return std::nullopt;
  }
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  std::string out;
  char buf[4096];
  int ret = Z_OK;
  while (ret == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buf);
    stream.avail_out = sizeof(buf);
    ret = inflate(&stream, Z_NO_FLUSH);
    out.append(buf, sizeof(buf) - stream.avail_out);
    if (out.size() > 16 * 1024 * 1024) {
      inflateEnd(&stream);
      return std::nullopt;
    }
  }
  inflateEnd(&stream);
  if (ret != Z_STREAM_END) {
    return std::nullopt;
  }
  return out;
}

int env_int(const char* key, int fallback) {
  if (const char* v = std::getenv(key)) {
    try {
      return std::stoi(v);
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

}  // namespace matchmaker
