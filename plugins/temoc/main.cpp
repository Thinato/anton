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

// A changed file of the PR, if it is a regular file of at most 4 MB: the PR
// could add a link to /dev/zero or a FIFO, which would never finish reading.
static std::optional<std::string> read_spec(const std::string& p) {
  std::error_code ec;
  auto st = std::filesystem::symlink_status(p, ec);
  if (ec || st.type() != std::filesystem::file_type::regular || std::filesystem::file_size(p, ec) > (4 << 20) || ec)
    return std::nullopt;
  return read_file(p);
}

// Results moved: the ticker sends the PR's line within a quarter second.
static void touch(const std::string& url) { prs[url].dirty = true; }

// The last n lines of s.
static std::string last_lines(const std::string& s, int n) {
  std::string t = trim(s);
  size_t at = t.size();
  for (int i = 0; i < n && at != std::string::npos && at > 0; ++i) at = t.rfind('\n', at - 1);
  return at == std::string::npos || at == 0 ? t : t.substr(at + 1);
}

// Checks out the PR, picks its changed spec files (or the one asked for),
// runs the repo's setup command, then temoc, folding its events into the
// PR's results as they come.
static void execute(const std::string& url, const Target& target, Outcome& o) {
  auto fail = [&](const std::string& why) {
    std::lock_guard lk(mu);
    fail_run(o, prs[url].results, why);
  };
  o.timeout = cfg.timeout;
  auto t0 = std::chrono::steady_clock::now();
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

  o.ref = trim(run({"git", "-C", cwd, "rev-parse", "HEAD"}).out);

  // The changed .md files that hold a test, as they are in the checkout
  // (a removed file isn't there).
  Result f = run({"gh", "api", "--paginate", "repos/" + repo + "/pulls/" + number + "/files", "--jq", ".[].filename"}, "",
                 true, 120);
  if (f.code != 0) return fail("gh api repos/" + repo + "/pulls/" + number + "/files: " + first_line(f));
  std::istringstream names(f.out);
  std::map<std::string, std::vector<Table>> specs;
  for (std::string name; std::getline(names, name);)
    if (is_markdown(name) && (target.file.empty() || name == target.file))
      if (auto text = read_spec(cwd + "/" + name); text && has_directive(*text)) specs[name] = tables_of(*text);
  if (specs.empty()) {
    if (!target.file.empty()) return fail(target.file + " is not one of this PR's changed spec files");
    o.nothing = "No spec files changed: none of this PR's changed `.md` files has a `decisionTable` directive.";
    return;
  }
  for (auto& [name, _] : specs) o.files.push_back(name);

  std::string config = r.config[0] == '/' ? r.config : cwd + "/" + r.config;
  std::vector<std::string> args = {"temoc", "-c", config, "--output", "jsonl", "--"};
  std::string shown = "temoc -c " + r.config + " --output jsonl --";
  for (auto& name : o.files) {
    std::string a = target.line ? where(target) : name;
    args.push_back(a);
    shown += " " + a;
  }
  {
    std::lock_guard lk(mu);
    Results& res = prs[url].results;
    if (res.ref != o.ref) {  // results of another commit don't mix with these
      res.files.clear();
      res.ref = o.ref;
    }
    for (auto& [name, tables] : specs) {
      set_tables(res.files[name], tables);
      queue(res.files[name], target.line);
    }
    res.run_files = o.files;
    res.command = shown;
    touch(url);
  }
  if (!std::filesystem::is_regular_file(config, ec))
    return fail("temoc config " + r.config + " is not in " + (r.checkout == "pr" ? "the PR's checkout" : r.dir));

  // The setup command and temoc share the timeout.
  auto left = [&] {
    auto spent = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
    return std::max<int>(1, cfg.timeout - static_cast<int>(spent));
  };
  if (!r.setup.empty()) {
    {
      std::lock_guard lk(mu);
      prs[url].results.phase = "setup";
      touch(url);
    }
    std::string out;
    auto keep = [&](const std::string& l) {
      out += l + "\n";
      if (out.size() > 65536) out.erase(0, out.size() - 32768);
    };
    Watched s = watch(r.setup, r.env, "", cwd, left(), keep, keep);
    std::string end = last_lines(out, 20);
    std::string shown_out = end.empty() ? "" : "\n\n```\n" + end + "\n```";
    if (!s.error.empty()) return fail("can't start the setup command " + r.setup[0] + ": " + s.error);
    if (s.timed_out) return fail("the setup command timed out after " + std::to_string(cfg.timeout) + "s" + shown_out);
    if (s.code != 0) return fail("setup failed (exit " + std::to_string(s.code) + ")" + shown_out);
  }

  {
    std::lock_guard lk(mu);
    prs[url].results.phase = "starting";  // until its first table starts
    touch(url);
  }
  Watched w = watch(
      args, r.env, "", cwd, left(),
      [&](const std::string& l) {
        std::lock_guard lk(mu);
        fold(o, prs[url].results, json::parse(l, nullptr, false));
        touch(url);
      },
      [&](const std::string& l) {
        o.err += l + "\n";
        if (o.err.size() > 8192) o.err.erase(0, o.err.size() - 4096);
      });
  if (!w.error.empty())
    return fail("can't start temoc: " + w.error +
                (w.error.find("No such file") != std::string::npos ? " (temoc isn't on the PATH anton-temoc got)" : ""));
  o.exit_code = w.code;
  o.timed_out = w.timed_out;
  o.err = tail(o.err, 2000);
}

// anton -> us: {"action": "run", "pr": url} when you press the key, with
// "file" (and "line": a table's directive or a row) to run less.
static void serve_one(const std::string& line) {
  json in = json::parse(line, nullptr, false);
  if (!in.is_object() || str(in, "action") != "run") return;
  std::string url = normalize_url(str(in, "pr"));
  if (url.empty()) return;
  Target target{str(in, "file"), 0};
  if (auto l = in.find("line"); l != in.end() && l->is_number_integer() && l->get<long>() > 0 && l->get<long>() < (1 << 24))
    target.line = l->get<int>();
  if (target.file.empty()) target.line = 0;

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
  st.target = target;
  begin(st.results, target, std::time(nullptr));
  emit(pr_line(url, st, std::time(nullptr)));
  st.dirty = false;
  std::thread([url, target] {
    Outcome o;
    execute(url, target, o);
    std::lock_guard lk(mu);
    --running;
    PrState& st = prs[url];
    st.running = false;
    end(st.results, o);
    st.last = std::move(o);
    emit(pr_line(url, st, std::time(nullptr)));
    st.dirty = false;
  }).detach();
}

// Sends the line of every PR whose results moved, at most four times a
// second: rows fill in as temoc finishes them. A run's line also goes out
// every second, so its clock ticks through a long setup with nothing else
// to say.
[[noreturn]] static void ticker() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::lock_guard lk(mu);
    std::time_t now = std::time(nullptr);
    for (auto& [url, st] : prs)
      if (st.dirty || (st.running && st.sent != now)) {
        emit(pr_line(url, st, now));
        st.dirty = false;
        st.sent = now;
      }
  }
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

  // A missing or bad config is reported in the footer (health), and the key
  // still works: it answers with the problem as the PR's tag and a banner
  // in the reader. Exiting would have anton restart us and say it again and
  // again.
  std::string problem;
  if (auto text = read_file(cfg_path); !text) {
    problem = "no " + llm::tilde(cfg_path);
  } else if (std::string e = parse_config(*text, cfg); !e.empty()) {
    problem = llm::tilde(cfg_path) + ": " + e;
    std::fprintf(stderr, "anton-temoc: %s\n", problem.c_str());
  }
  {
    std::lock_guard lk(mu);
    if (!problem.empty()) emit(json({{"health", "error"}, {"text", problem}}).dump(-1, ' ', false, json::error_handler_t::replace));
    else emit(R"({"health":"ok"})");
    if (cfg.key.size() != 1) cfg.key = "T";  // the bad one it read
    emit(json({{"actions", {{{"id", "run"}, {"key", cfg.key}, {"label", "run the PR's temoc specs"}, {"button", "Run"}, {"line", true}}}}})
             .dump(-1, ' ', false, json::error_handler_t::replace));
  }
  std::thread(ticker).detach();
  for (std::string line; std::getline(std::cin, line);) {
    if (!problem.empty()) {
      json in = json::parse(line, nullptr, false);
      if (std::string url = in.is_object() ? normalize_url(str(in, "pr")) : ""; !url.empty()) {
        std::lock_guard lk(mu);
        emit(problem_line(url, problem));
      }
      continue;
    }
    try {
      serve_one(line);
    } catch (const std::exception& e) {  // one bad request is skipped, not fatal
      std::fprintf(stderr, "anton-temoc: %s\n", e.what());
    }
  }
  quit(0);  // anton is gone: so are its runs
}
