// anton-llm: a key runs your agent on a PR; its answer becomes the PR's note.
// See specs/04-plugin-llm.md.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "llm.hpp"
#include "proc.hpp"

using namespace anton;
using namespace anton::llm;

static const char* kUsage =
    "usage: anton-llm [--config PATH]\n"
    "\n"
    "  --config PATH  default $XDG_CONFIG_HOME/anton/llm.json, or ~/.config/anton/llm.json\n";

static constexpr size_t kMaxRuns = 64;            // at once, across PRs and reviewers
static constexpr size_t kMaxDiff = 200 * 1024;    // piped to "stdin": "diff"
static constexpr size_t kMaxOut = 1024 * 1024;    // stdout kept from one run
static constexpr size_t kMaxNote = 60 * 1024;     // a stored output, so {note} stays a sane argv

static std::mutex mu;  // guards everything below and stdout
static std::vector<Reviewer> reviewers;
static std::map<std::string, PrState> prs;  // by normalized URL
static size_t running = 0;
// Running agents' process groups, fixed slots so the signal handler can read them.
static std::atomic<pid_t> live[kMaxRuns] = {};

static void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

static void kill_live() {
  for (auto& p : live)
    if (pid_t g = p; g > 0) kill_group(g);
}

// Agents run in their own process groups (so a timeout reaches whatever they
// spawn), which anton's kill of ours doesn't reach: take them along.
[[noreturn]] static void quit(int code) {
  kill_live();
  ::_exit(code);  // not exit(): run threads may still be using statics
}

static std::string tail(const std::string& s, size_t n) {
  std::string t = trim(s);
  if (t.size() <= n) return t;
  size_t at = t.size() - n;
  while (at < t.size() && (static_cast<unsigned char>(t[at]) & 0xC0) == 0x80) ++at;
  return "…" + t.substr(at);
}

// Runs r on url. On success `output` is the note-to-be.
static Run execute(const Reviewer& r, const std::string& url, const std::string& note, std::string& output) {
  std::string input;
  if (r.stdin_from == "diff") {
    Result d = run({"gh", "pr", "diff", url}, "", false);
    if (d.code != 0) {
      std::string e = trim(d.err);
      return {Run::kFailed, "gh pr diff: " + (e.empty() ? "exit " + std::to_string(d.code) : e.substr(0, e.find('\n')))};
    }
    input = std::move(d.out);
    if (input.size() > kMaxDiff) {
      input.resize(kMaxDiff);
      input += "\n[diff truncated at 200 KB]\n";
    }
  }
  std::error_code ec;
  if (!r.cwd.empty() && !std::filesystem::is_directory(r.cwd, ec))
    return {Run::kFailed, "cwd " + r.cwd + " is not a directory"};

  Spawned s = spawn(argv(r, builtins(url, note)), {}, input, true, false, r.cwd);
  if (s.pid == -1) return {Run::kFailed, s.error};
  std::atomic<pid_t>* slot = nullptr;
  for (auto& p : live) {
    pid_t free = 0;
    if (p.compare_exchange_strong(free, s.pid)) {
      slot = &p;
      break;
    }
  }

  // The watchdog: SIGTERM to the group at the timeout, SIGKILL 5s later.
  std::mutex wm;
  std::condition_variable wcv;
  bool done = false, timed_out = false;
  std::thread dog([&] {
    std::unique_lock lk(wm);
    if (wcv.wait_for(lk, std::chrono::seconds(r.timeout), [&] { return done; })) return;
    timed_out = true;
    kill_group(s.pid);
    if (!wcv.wait_for(lk, std::chrono::seconds(5), [&] { return done; })) ::kill(-s.pid, SIGKILL);
  });
  std::string out, err;
  bool cut = false;
  int code = stream(
      s,
      [&](const std::string& l) {
        if (out.size() < kMaxOut) out += l + "\n";
        else cut = true;
      },
      [&](const std::string& l) {
        err += l + "\n";
        if (err.size() > 8192) err.erase(0, err.size() - 4096);
      });
  {
    std::lock_guard lk(wm);
    done = true;
  }
  wcv.notify_all();
  dog.join();
  if (slot) *slot = 0;

  std::string why = timed_out ? "timed out after " + std::to_string(r.timeout) + "s"
                    : code != 0 ? "exit " + std::to_string(code)
                                : "";
  if (!why.empty()) {
    std::string t = tail(err.empty() ? out : err, 2000);
    return {Run::kFailed, t.empty() ? why : why + "\n\n```\n" + t + "\n```"};
  }
  output = trim(out);
  if (output.empty()) output = "(" + r.tag + " printed nothing)";
  if (cut) output += "\n\n[truncated]";
  output = fit(output, kMaxNote);
  return {Run::kOk, ""};
}

// anton -> us: {"action": key, "pr": url} when you press a reviewer's key.
static void serve_one(const std::string& line) {
  json in = json::parse(line, nullptr, false);
  if (!in.is_object()) return;
  std::string id = str(in, "action"), url = normalize_url(str(in, "pr"));
  size_t i = 0;
  while (i < reviewers.size() && reviewers[i].key != id) ++i;
  if (i == reviewers.size() || url.empty()) return;
  const Reviewer& r = reviewers[i];

  std::lock_guard lk(mu);
  PrState& st = prs[url];
  if (auto it = st.runs.find(i); it != st.runs.end() && it->second.state == Run::kRunning) {
    std::fprintf(stderr, "anton-llm: %s is already running on this PR\n", r.tag.c_str());
    return;
  }
  if (running >= kMaxRuns) {
    std::fprintf(stderr, "anton-llm: %zu runs going already, try again later\n", running);
    return;
  }
  ++running;
  st.runs[i] = Run{};
  emit(pr_line(url, st, reviewers));
  std::thread([i, url, note = st.output] {
    std::string output;
    Run res = execute(reviewers[i], url, note, output);
    std::lock_guard lk(mu);
    --running;
    PrState& st = prs[url];
    if (res.state == Run::kOk) st.output = output;
    st.runs[i] = res;
    emit(pr_line(url, st, reviewers));
  }).detach();
}

int main(int argc, char** argv) {
  for (int sig : {SIGTERM, SIGHUP, SIGINT}) ::signal(sig, [](int s) { quit(128 + s); });
  std::string path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) path = argv[++i];
    else if (a == "-h" || a == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      std::fprintf(stderr, "anton-llm: bad argument %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
  if (path.empty()) path = llm::config_path();

  // A missing or bad config is reported once, in the footer, and then we
  // idle: exiting would have anton restart us and say it again and again.
  std::string problem;
  std::ifstream f(path);
  if (!f) {
    problem = "no " + tilde(path);
  } else {
    std::stringstream ss;
    ss << f.rdbuf();
    if (std::string e = llm::parse_config(ss.str(), reviewers); !e.empty()) {
      problem = tilde(path) + ": " + e;
      std::fprintf(stderr, "anton-llm: %s\n", problem.c_str());
    }
  }
  {
    std::lock_guard lk(mu);
    if (!problem.empty()) {
      emit(json({{"health", "error"}, {"text", problem}}).dump(-1, ' ', false, json::error_handler_t::replace));
    } else {
      json actions = json::array();
      for (auto& r : reviewers) actions.push_back({{"id", r.key}, {"key", r.key}, {"label", r.label}});
      emit(R"({"health":"ok"})");
      emit(json({{"actions", actions}}).dump(-1, ' ', false, json::error_handler_t::replace));
    }
  }
  for (std::string line; std::getline(std::cin, line);) {
    if (!problem.empty()) continue;
    try {
      serve_one(line);
    } catch (const std::exception& e) {  // one bad request is skipped, not fatal
      std::fprintf(stderr, "anton-llm: %s\n", e.what());
    }
  }
  quit(0);  // anton is gone: so are its runs
}
