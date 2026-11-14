#pragma once
// What anton-llm and anton-temoc share to run a command on a PR: a temp
// directory, a detached worktree of the PR's head in it, and a run in its own
// process group under a watchdog. I/O, so not in the pure headers.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "proc.hpp"

namespace anton::plugin {

inline constexpr size_t kMaxRuns = 64;  // at once, across PRs
// Running commands' process groups, fixed slots so a signal handler can read them.
inline std::atomic<pid_t> live[kMaxRuns] = {};

// Runs go in their own process groups (so a timeout reaches whatever they
// spawn), which anton's kill of the plugin doesn't reach: take them along.
inline void kill_live() {
  for (auto& p : live)
    if (pid_t g = p; g > 0) kill_group(g);
}

inline std::string trim_ws(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The last n bytes of s, trimmed, cut at a UTF-8 boundary.
inline std::string tail(const std::string& s, size_t n) {
  std::string t = trim_ws(s);
  if (t.size() <= n) return t;
  size_t at = t.size() - n;
  while (at < t.size() && (static_cast<unsigned char>(t[at]) & 0xC0) == 0x80) ++at;
  return "…" + t.substr(at);
}

inline std::string first_line(const Result& res) {
  std::string e = trim_ws(res.err);
  return e.empty() ? "exit " + std::to_string(res.code) : e.substr(0, e.find('\n'));
}

// fetch writes the clone's one FETCH_HEAD, so a fetch and the worktree add
// that reads it go together; runs on other PRs of that clone wait their turn.
inline std::mutex git_mu;

// A run's {tmp} and the PR's worktree inside it, removed with the object.
// ponytail: a plugin killed mid-run leaves them; the next run's prune drops the worktree's entry.
struct Scratch {
  std::string tmp, clone, tree;
  ~Scratch() {
    if (!tree.empty()) {
      std::lock_guard lk(git_mu);
      run({"git", "-C", clone, "worktree", "remove", "--force", tree});
      run({"git", "-C", clone, "worktree", "prune"});
    }
    std::error_code ec;
    if (!tmp.empty()) std::filesystem::remove_all(tmp, ec);
  }
};

// Makes sc.tmp, absolute and real. Returns "" or the problem.
inline std::string make_tmp(Scratch& sc, const std::string& prefix) {
  std::error_code ec;
  std::string tmpl = (std::filesystem::temp_directory_path(ec) / (prefix + "-XXXXXX")).string();
  if (!::mkdtemp(tmpl.data())) return std::string("mkdtemp: ") + std::strerror(errno);
  sc.tmp = std::filesystem::canonical(tmpl, ec).string();  // macOS: /var/folders/… is /private/var/folders/…
  if (ec) sc.tmp = tmpl;
  return "";
}

// Checks out PR `number` of `clone` as a detached worktree, sc.tmp/pr, in
// sc.tree. Returns "" or the problem.
inline std::string add_worktree(Scratch& sc, const std::string& clone, const std::string& number) {
  std::lock_guard lk(git_mu);
  std::string ref = "pull/" + number + "/head";
  Result f = run({"git", "-C", clone, "fetch", "origin", ref});
  if (f.code != 0) return "git fetch origin " + ref + " in " + clone + ": " + first_line(f);
  sc.clone = clone, sc.tree = sc.tmp + "/pr";
  // No hooks: a core.hooksPath inside the repo would be the PR's own post-checkout.
  Result w = run({"git", "-c", "core.hooksPath=/dev/null", "-C", clone, "worktree", "add", "--detach", sc.tree,
                  "FETCH_HEAD"});
  if (w.code != 0) return "git worktree add: " + first_line(w);
  return "";
}

struct Watched {
  int code = -1;
  bool timed_out = false;
  std::string error;  // couldn't start: strerror
};

// Runs argv in its own process group, lines to the callbacks, with a
// watchdog: SIGTERM to the group after `timeout` seconds, SIGKILL 5s later.
// The group is in `live` meanwhile, so kill_live() reaches it.
inline Watched watch(const std::vector<std::string>& argv, const std::map<std::string, std::string>& env,
                     const std::string& input, const std::string& cwd, int timeout,
                     const std::function<void(const std::string&)>& on_out,
                     const std::function<void(const std::string&)>& on_err) {
  Watched res;
  Spawned s = spawn(argv, env, input, true, false, cwd);
  if (s.pid == -1) {
    res.error = s.error;
    return res;
  }
  std::atomic<pid_t>* slot = nullptr;
  for (auto& p : live) {
    pid_t free = 0;
    if (p.compare_exchange_strong(free, s.pid)) {
      slot = &p;
      break;
    }
  }
  std::mutex wm;
  std::condition_variable wcv;
  bool done = false;
  std::thread dog([&] {
    std::unique_lock lk(wm);
    if (wcv.wait_for(lk, std::chrono::seconds(timeout), [&] { return done; })) return;
    res.timed_out = true;
    kill_group(s.pid);
    if (!wcv.wait_for(lk, std::chrono::seconds(5), [&] { return done; })) ::kill(-s.pid, SIGKILL);
  });
  res.code = stream(s, on_out, on_err);
  {
    std::lock_guard lk(wm);
    done = true;
  }
  wcv.notify_all();
  dog.join();
  if (slot) *slot = 0;
  return res;
}

}  // namespace anton::plugin
