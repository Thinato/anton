#include "plugins.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "proc.hpp"

namespace anton {

Plugins::Plugins(std::vector<PluginConfig> cfg, OnLine on_line, OnExit on_exit)
    : cfg_(std::move(cfg)), on_line_(std::move(on_line)), on_exit_(std::move(on_exit)) {
  std::fill(std::begin(in_), std::end(in_), -1);
}

bool Plugins::send(size_t i, const std::string& line) {
  std::lock_guard lk(mu_);
  if (i >= cfg_.size() || in_[i] == -1) return false;
  std::string l = line + "\n";
  // ponytail: a short write (pipe nearly full) drops the rest of the line;
  // lines are ~100 bytes, so it only happens to a plugin that never reads.
  return write(in_[i], l.data(), l.size()) == static_cast<ssize_t>(l.size());
}

void Plugins::kill_all_from_signal() {
  for (auto& p : pids_)
    if (pid_t pid = p.load(); pid > 0) ::kill(-pid, SIGTERM);
}

void Plugins::start() {
  // ponytail: detached threads; anton _Exits after stop(), so nothing joins.
  for (size_t i = 0; i < cfg_.size(); ++i) std::thread([this, i] { loop(i); }).detach();
}

void Plugins::refresh() {
  std::lock_guard lk(mu_);
  ++refresh_gen_;
  cv_.notify_all();
}

void Plugins::stop() {
  std::lock_guard lk(mu_);
  stopped_ = true;
  for (auto& p : pids_) kill_group(p.load());
  cv_.notify_all();
}

// True when stopped during the wait.
bool Plugins::wait_for(std::unique_lock<std::mutex>& lk, int seconds) {
  return cv_.wait_for(lk, std::chrono::seconds(seconds), [this] { return stopped_; });
}

void Plugins::loop(size_t i) {
  const PluginConfig& c = cfg_[i];
  unsigned long seen_gen = 0;
  int delay = 5;
  for (;;) {
    {
      std::unique_lock lk(mu_);
      if (stopped_) return;
      if (c.rerun == "refresh") {  // anton's first refresh is the first run
        cv_.wait(lk, [&] { return stopped_ || refresh_gen_ != seen_gen; });
        if (stopped_) return;
      }
      seen_gen = refresh_gen_;
    }

    auto t0 = std::chrono::steady_clock::now();
    // Long-running plugins get a stdin for actions and events (02-plugins.md).
    Spawned s = spawn(c.command, c.env, "", true, c.rerun == "exit");
    int code = -1;
    if (s.pid == -1) {
      on_line_(i, s.error, true);
    } else {
      {
        std::lock_guard lk(mu_);
        pids_[i] = s.pid;
        if (s.in != -1) {
          fcntl(s.in, F_SETFL, O_NONBLOCK);
          in_[i] = s.in;
        }
        if (stopped_) kill_group(s.pid);
      }
      code = stream(
          s,
          [&](const std::string& l) {
            if (l.size() <= 65536) on_line_(i, l, false);  // longer: dropped, per spec
          },
          [&](const std::string& l) { on_line_(i, l, true); }, 65536);
      std::lock_guard lk(mu_);
      pids_[i] = -1;
      if (in_[i] != -1) close(in_[i]);
      in_[i] = -1;
    }
    on_exit_(i, code);

    if (c.rerun == "never") return;
    if (c.rerun == "exit") {
      // 5s after a long run; doubling to 5 min while it keeps dying quickly.
      if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(30)) delay = 5;
      std::unique_lock lk(mu_);
      if (wait_for(lk, delay)) return;
      delay = std::min(delay * 2, 300);
    }
  }
}

}  // namespace anton
