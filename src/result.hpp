#pragma once

#include "error.hpp"

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace matchmaker
{

template <class T> using Result = std::expected<T, Error>;

using Status = Result<bool>;

inline std::unexpected<Error> Err(ErrorCode code, std::string message,
                                  std::optional<std::int64_t> max_lifetime_sec = std::nullopt)
{
  return std::unexpected(Error{code, std::move(message), max_lifetime_sec});
}

} // namespace matchmaker
