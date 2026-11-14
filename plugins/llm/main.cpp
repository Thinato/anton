// anton-llm: a key runs your agent on a PR; its answer becomes the PR's note.
// See specs/04-plugin-llm.md.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../run.hpp"
#include "llm.hpp"

using namespace anton;
using namespace anton::llm;
using namespace anton::plugin;

static const char* kUsage =
    "usage: anton-llm [--config PATH]\n"
    "       anton-llm --check-review [--allow-approve]   (a Claude Code PreToolUse hook)\n"
    "\n"
    "  --config PATH   default $XDG_CONFIG_HOME/anton/llm.json, or ~/.config/anton/llm.json\n"
    "  --check-review  read a hook's JSON on stdin; exit 2 for a call that could approve,\n"
    "                  request changes, merge or close, 0 for anything else\n"
    "  --allow-approve  ...except a review POST whose \"event\" is \"APPROVE\"\n";

static constexpr size_t kMaxDiff = 200 * 1024;    // piped to "stdin": "diff"
static constexpr size_t kMaxOut = 1024 * 1024;    // stdout kept from one run
static constexpr size_t kMaxNote = 60 * 1024;     // a stored output, so {note} stays a sane argv

static std::mutex mu;  // guards everything below and stdout
static std::vector<Reviewer> reviewers;
static Vars repos;  // "owner/name" -> local clone
static std::map<std::string, PrState> prs;  // by normalized URL
static size_t running = 0;  // at most kMaxRuns, across PRs and reviewers

static void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[noreturn]] static void quit(int code) {
  kill_live();
  ::_exit(code);  // not exit(): run threads may still be using statics
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
  Scratch sc;
  if (std::string e = make_tmp(sc, "anton-llm"); !e.empty()) return {Run::kFailed, e};

  // The PR's repo has a clone: run in it, or in a worktree of the PR's head.
  Vars b = builtins(url, note, sc.tmp);
  std::string cwd = r.cwd;
  if (auto c = repos.find(b["repo"]); c != repos.end()) {
    if (!std::filesystem::is_directory(c->second, ec)) return {Run::kFailed, "repos: " + c->second + " is not a directory"};
    cwd = c->second;
    if (r.checkout == "pr") {
      if (std::string e = add_worktree(sc, c->second, b["number"]); !e.empty()) return {Run::kFailed, e};
      cwd = sc.tree;
    }
    b["repo_dir"] = cwd;
  }
  if (!cwd.empty() && !std::filesystem::is_directory(cwd, ec)) return {Run::kFailed, "cwd " + cwd + " is not a directory"};

  std::string out, err;
  bool cut = false;
  Watched w = watch(
      argv(r, b), {}, input, cwd, r.timeout,
      [&](const std::string& l) {
        if (out.size() < kMaxOut) out += l + "\n";
        else cut = true;
      },
      [&](const std::string& l) {
        err += l + "\n";
        if (err.size() > 8192) err.erase(0, err.size() - 4096);
      });
  if (!w.error.empty()) return {Run::kFailed, w.error};

  std::string why = w.timed_out ? "timed out after " + std::to_string(r.timeout) + "s"
                    : w.code != 0 ? "exit " + std::to_string(w.code)
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
