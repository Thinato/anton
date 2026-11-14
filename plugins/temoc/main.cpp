// anton-temoc: a key runs a PR's temoc acceptance specs; the results become
// the PR's tag, note and marks. See specs/05-plugin-temoc.md.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "../run.hpp"
#include "temoc.hpp"

using namespace anton;
using namespace anton::temoc;
using namespace anton::plugin;

static const char* kUsage =
    "usage: anton-temoc [--config PATH]\n"
    "\n"
    "  --config PATH   default $XDG_CONFIG_HOME/anton/temoc.json, or ~/.config/anton/temoc.json\n";

static std::mutex mu;  // guards everything below and stdout
static temoc::Config cfg;
static std::string cfg_path;
static std::map<std::string, PrState> prs;  // by normalized URL
static size_t running = 0;

static void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[noreturn]] static void quit(int code) {
  kill_live();  // temoc forwards the SIGTERM to its fixture server
  ::_exit(code);  // not exit(): run threads may still be using statics
}

static std::optional<std::string> read_file(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return f ? std::optional(ss.str()) : std::nullopt;
}

// Checks out the PR, picks its changed spec files and runs temoc on them.
static void execute(const std::string& url, Outcome& o) {
  auto fail = [&](const std::string& why) { o.error = why; };
  o.timeout = cfg.timeout;
  auto ref = parse_ref(url);
  if (!ref) return fail("not a PR URL: " + url);
  std::string repo = ref->owner + "/" + ref->name, number = std::to_string(ref->number);
  auto it = cfg.repos.find(repo);
  if (it == cfg.repos.end())
    return fail(repo + " is not in " + llm::tilde(cfg_path) + ". Add it:\n\n```json\n\"repos\": {\"" + repo +
                "\": {\"dir\": \"~/path/to/clone\", \"config\": \"Config.toml\"}}\n```");
  const Repo& r = it->second;
  std::error_code ec;
  if (!std::filesystem::is_directory(r.dir, ec)) return fail("repos: " + r.dir + " is not a directory");

  Scratch sc;
  if (std::string e = make_tmp(sc, "anton-temoc"); !e.empty()) return fail(e);
  std::string cwd = r.dir;
  if (r.checkout == "pr") {
    if (std::string e = add_worktree(sc, r.dir, number); !e.empty()) return fail(e);
    cwd = sc.tree;
  }

  // The changed .md files that hold a test, as they are in the checkout
  // (a removed file isn't there).
  Result f = run({"gh", "api", "--paginate", "repos/" + repo + "/pulls/" + number + "/files", "--jq", ".[].filename"});
  if (f.code != 0) return fail("gh api repos/" + repo + "/pulls/" + number + "/files: " + first_line(f));
  std::istringstream names(f.out);
  for (std::string name; std::getline(names, name);)
    if (is_markdown(name))
      if (auto text = read_file(cwd + "/" + name); text && has_directive(*text)) o.files.push_back(name);
  if (o.files.empty()) {
    o.nothing = "No spec files changed: none of this PR's changed `.md` files has a `decisionTable` directive.";
    return;
  }

  o.ref = trim(run({"git", "-C", cwd, "rev-parse", "HEAD"}).out);
  std::string config = r.config[0] == '/' ? r.config : cwd + "/" + r.config;
  if (!std::filesystem::is_regular_file(config, ec))
    return fail("temoc config " + r.config + " is not in " + (r.checkout == "pr" ? "the PR's checkout" : r.dir));

  std::vector<std::string> argv = {"temoc", "-c", config, "--output", "jsonl"};
  argv.insert(argv.end(), o.files.begin(), o.files.end());
  Watched w = watch(
      argv, r.env, "", cwd, cfg.timeout, [&](const std::string& l) { fold(o, json::parse(l, nullptr, false)); },
      [&](const std::string& l) {
        o.err += l + "\n";
        if (o.err.size() > 8192) o.err.erase(0, o.err.size() - 4096);
      });
  if (!w.error.empty()) return fail("can't start temoc: " + w.error);
  o.exit_code = w.code;
  o.timed_out = w.timed_out;
  o.err = tail(o.err, 2000);
}

// anton -> us: {"action": "run", "pr": url} when you press the key.
static void serve_one(const std::string& line) {
  json in = json::parse(line, nullptr, false);
  if (!in.is_object() || str(in, "action") != "run") return;
  std::string url = normalize_url(str(in, "pr"));
  if (url.empty()) return;

  std::lock_guard lk(mu);
  PrState& st = prs[url];
  if (st.running) {
    std::fputs("anton-temoc: temoc is already running on this PR\n", stderr);
    return;
  }
  if (running >= kMaxRuns) {
    std::fprintf(stderr, "anton-temoc: %zu runs going already, try again later\n", running);
    return;
  }
  ++running;
  st.running = true;
  emit(pr_line(url, st));
  std::thread([url] {
    Outcome o;
    execute(url, o);
    std::lock_guard lk(mu);
    --running;
    PrState& st = prs[url];
    st.running = false;
    st.last = std::move(o);
    emit(pr_line(url, st));
  }).detach();
}

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);  // a child that exits before reading its stdin must not kill us
  for (int sig : {SIGTERM, SIGHUP, SIGINT}) ::signal(sig, [](int s) { quit(128 + s); });
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
    else if (a == "-h" || a == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      std::fprintf(stderr, "anton-temoc: bad argument %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
  if (cfg_path.empty()) cfg_path = llm::config_path("temoc.json");

  // A missing or bad config is reported once, in the footer, and then we
  // idle: exiting would have anton restart us and say it again and again.
  std::string problem;
  if (auto text = read_file(cfg_path); !text) {
    problem = "no " + llm::tilde(cfg_path);
  } else if (std::string e = parse_config(*text, cfg); !e.empty()) {
    problem = llm::tilde(cfg_path) + ": " + e;
    std::fprintf(stderr, "anton-temoc: %s\n", problem.c_str());
  }
  {
    std::lock_guard lk(mu);
    if (!problem.empty()) {
      emit(json({{"health", "error"}, {"text", problem}}).dump(-1, ' ', false, json::error_handler_t::replace));
    } else {
      emit(R"({"health":"ok"})");
      emit(json({{"actions", {{{"id", "run"}, {"key", cfg.key}, {"label", "run the PR's temoc specs"}}}}})
               .dump(-1, ' ', false, json::error_handler_t::replace));
    }
  }
  for (std::string line; std::getline(std::cin, line);) {
    if (!problem.empty()) continue;
    try {
      serve_one(line);
    } catch (const std::exception& e) {  // one bad request is skipped, not fatal
      std::fprintf(stderr, "anton-temoc: %s\n", e.what());
    }
  }
  quit(0);  // anton is gone: so are its runs
}
