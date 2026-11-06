#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace matchmaker
{

class HttpSession;
class StatsSession;

class App
{
  friend class HttpSession;
  friend class StatsSession;

public:
  explicit App(std::string database_path);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  void start();
  void stop();
  void wait();
  void install_signals();
  std::uint16_t https_port() const;
  std::uint16_t stats_port() const;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace matchmaker
