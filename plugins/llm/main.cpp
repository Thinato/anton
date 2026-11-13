// anton-llm: a key runs your agent on a PR; its answer becomes the PR's note.
// See specs/04-plugin-llm.md.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    "       anton-llm --check-review [--allow-approve]   (a Claude Code PreToolUse hook)\n"
    "\n"
    "  --config PATH   default $XDG_CONFIG_HOME/anton/llm.json, or ~/.config/anton/llm.json\n"
    "  --check-review  read a hook's JSON on stdin; exit 2 for a call that could approve,\n"
    "                  request changes, merge or close, 0 for anything else\n"
    "  --allow-approve  ...except a review POST whose \"event\" is \"APPROVE\"\n";

static constexpr size_t kMaxRuns = 64;            // at once, across PRs and reviewers
static constexpr size_t kMaxDiff = 200 * 1024;    // piped to "stdin": "diff"
static constexpr size_t kMaxOut = 1024 * 1024;    // stdout kept from one run
static constexpr size_t kMaxNote = 60 * 1024;     // a stored output, so {note} stays a sane argv

static std::mutex mu;  // guards everything below and stdout
static std::vector<Reviewer> reviewers;
static Vars repos;  // "owner/name" -> local clone
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

static std::string first_line(const Result& res) {
  std::string e = trim(res.err);
  return e.empty() ? "exit " + std::to_string(res.code) : e.substr(0, e.find('\n'));
}

// fetch writes the clone's one FETCH_HEAD, so a fetch and the worktree add
// that reads it go together; runs on other PRs of that clone wait their turn.
static std::mutex git_mu;

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
  Scratch sc;
  std::string tmpl = (std::filesystem::temp_directory_path(ec) / "anton-llm-XXXXXX").string();
  if (!::mkdtemp(tmpl.data())) return {Run::kFailed, std::string("mkdtemp: ") + std::strerror(errno)};
  sc.tmp = std::filesystem::canonical(tmpl, ec).string();  // macOS: /var/folders/… is /private/var/folders/…
  if (ec) sc.tmp = tmpl;

  // The PR's repo has a clone: run in it, or in a worktree of the PR's head.
  Vars b = builtins(url, note, sc.tmp);
  std::string cwd = r.cwd;
  if (auto c = repos.find(b["repo"]); c != repos.end()) {
    if (!std::filesystem::is_directory(c->second, ec)) return {Run::kFailed, "repos: " + c->second + " is not a directory"};
    cwd = c->second;
    if (r.checkout == "pr") {
      std::lock_guard lk(git_mu);
      std::string ref = "pull/" + b["number"] + "/head";
      Result f = run({"git", "-C", c->second, "fetch", "origin", ref});
      if (f.code != 0) return {Run::kFailed, "git fetch origin " + ref + " in " + c->second + ": " + first_line(f)};
      sc.clone = c->second, sc.tree = cwd = sc.tmp + "/pr";
      // No hooks: a core.hooksPath inside the repo would be the PR's own post-checkout.
      Result w = run({"git", "-c", "core.hooksPath=/dev/null", "-C", c->second, "worktree", "add", "--detach", cwd,
                      "FETCH_HEAD"});
      if (w.code != 0) return {Run::kFailed, "git worktree add: " + first_line(w)};
    }
    b["repo_dir"] = cwd;
  }
  if (!cwd.empty() && !std::filesystem::is_directory(cwd, ec)) return {Run::kFailed, "cwd " + cwd + " is not a directory"};

  Spawned s = spawn(argv(r, b), {}, input, true, false, cwd);
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
  ::signal(SIGPIPE, SIG_IGN);  // an agent that exits before reading its stdin must not kill us
  for (int sig : {SIGTERM, SIGHUP, SIGINT}) ::signal(sig, [](int s) { quit(128 + s); });
  std::string path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) path = argv[++i];
    else if (a == "--check-review") {
      bool approve = i + 1 < argc && std::string(argv[i + 1]) == "--allow-approve";
      std::stringstream in;
      in << std::cin.rdbuf();
      std::string why = review_block(json::parse(in.str(), nullptr, false), [](const std::string& p) {
        std::ifstream f(p);
        std::stringstream ss;
        ss << f.rdbuf();
        return f ? std::optional(ss.str()) : std::nullopt;
      }, approve);
      if (why.empty()) return 0;
      std::fprintf(stderr, "anton-llm: %s\n", why.c_str());  // exit 2: Claude Code blocks the call, Claude reads this
      return 2;
    } else if (a == "-h" || a == "--help") {
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
    if (std::string e = llm::parse_config(ss.str(), reviewers, &repos); !e.empty()) {
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
