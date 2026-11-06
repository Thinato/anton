#pragma once
// Runs the configured plugins, one thread each, and hands their output lines
// to callbacks. The callbacks run on plugin threads: the UI posts them to its
// own loop before touching any state.

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace anton {

struct PluginConfig {
  std::string name;
  std::vector<std::string> command;
  std::string rerun = "exit";  // exit | refresh | never
  std::map<std::string, std::string> env;
};

class Plugins {
 public:
  static constexpr size_t kMaxPlugins = 32;  // config.hpp rejects more
  // on_line(index, line, from_stderr); on_exit(index, exit code).
  using OnLine = std::function<void(size_t, const std::string&, bool)>;
  using OnExit = std::function<void(size_t, int)>;

  Plugins(std::vector<PluginConfig> cfg, OnLine on_line, OnExit on_exit);
  void start();
  void refresh();  // reruns the "refresh" plugins
  void stop();     // kills every plugin's process group
  // One line to a long-running plugin's stdin. False if it isn't running or
  // its pipe is full (it doesn't read stdin): never blocks the UI.
  bool send(size_t i, const std::string& line);

  // Async-signal-safe: SIGTERM to every running plugin's group. For fatal
  // signal handlers, where stop()'s mutex can't be taken.
  static void kill_all_from_signal();

 private:
  void loop(size_t i);
  bool wait_for(std::unique_lock<std::mutex>& lk, int seconds);

  std::vector<PluginConfig> cfg_;
  OnLine on_line_;
  OnExit on_exit_;
  std::mutex mu_;
  std::condition_variable cv_;
  // Fixed slots so a signal handler can read them lock-free.
  static inline std::atomic<pid_t> pids_[kMaxPlugins] = {};
  int in_[kMaxPlugins];  // stdin write ends, -1 when not running; under mu_
  unsigned long refresh_gen_ = 0;
  bool stopped_ = false;
};

}  // namespace anton
