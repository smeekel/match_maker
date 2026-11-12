#pragma once

#include "error.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace matchmaker
{

// C++20 stand-in for std::expected<T, Error>. operator bool is true when a value is held.
struct Unexpected
{
  Error error;
};

template <class T> class Result
{
public:
  Result(T value) : data_(std::move(value))
  {
  }

  Result(Unexpected unexpected) : data_(std::move(unexpected.error))
  {
  }

  explicit operator bool() const noexcept
  {
    return std::holds_alternative<T>(data_);
  }

  bool has_value() const noexcept
  {
    return std::holds_alternative<T>(data_);
  }

  T& operator*()
  {
    return std::get<T>(data_);
  }

  const T& operator*() const
  {
    return std::get<T>(data_);
  }

  T* operator->()
  {
    return &std::get<T>(data_);
  }

  const T* operator->() const
  {
    return &std::get<T>(data_);
  }

  Error& error()
  {
    return std::get<Error>(data_);
  }

  const Error& error() const
  {
    return std::get<Error>(data_);
  }

private:
  std::variant<T, Error> data_;
};

using Status = Result<bool>;

inline Unexpected Err(ErrorCode code, std::string message,
                      std::optional<std::int64_t> max_lifetime_sec = std::nullopt)
{
  return Unexpected{Error{code, std::move(message), max_lifetime_sec}};
}

} // namespace matchmaker
