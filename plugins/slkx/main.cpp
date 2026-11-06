// anton-slkx: Slack -> anton plugin lines. See specs/03-plugin-slkx.md.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "fold.hpp"
#include "proc.hpp"

using namespace anton;
using namespace anton::slkx;

static const char* kUsage =
    "usage: anton-slkx [--review-channel C] [--deploy-channel C] [options]\n"
    "\n"
    "  --review-channel C        posts form the code-review list; reviewing emoji tag them\n"
    "  --review-emoji a,b        default eyes,zoiando\n"
    "  --deploy-channel C        posts mean queued for deploy\n"
    "  --deploy-clear-emoji a,b  default p2r2-rejected-pr\n"
    "  --deploy-merged-emoji e   default p2r2-pr-merged: re-check the PR on GitHub at once\n"
    "  --deploy-state e=glyph:text  repeatable, in pipeline order; replaces the defaults\n"
    "  --backfill N              messages read per channel at startup (default 1000)\n"
    "  --slkx PATH               default slkx\n";

static std::string slkx_bin = "slkx";
static std::atomic<pid_t> watch_pid = -1;  // read by the signal handler

static void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[noreturn]] static void fail(const std::string& why) {
  if (pid_t w = watch_pid; w > 0) ::kill(w, SIGTERM);  // or it outlives us
  emit(json({{"health", "error"}, {"text", why}}).dump(-1, ' ', false, json::error_handler_t::replace));
  std::fprintf(stderr, "anton-slkx: %s\n", why.c_str());
  std::fflush(nullptr);
  ::_exit(1);  // not exit(): the stdin thread may still be using statics
}

static json slkx_json(std::vector<std::string> args) {
  args.insert(args.begin(), slkx_bin);
  Result r = run(args, "", false);  // in our group: anton's stop reaches it
  json j = json::parse(r.out, nullptr, false);
  if (r.code != 0 || j.is_discarded()) {
    std::string err = r.err.substr(0, r.err.find('\n'));
    fail(args[1] + ": " + (err.empty() ? "exit " + std::to_string(r.code) : err));
  }
  return j;
}

static std::set<std::string> split(const std::string& s) {
  std::set<std::string> out;
  std::stringstream ss(s);
  for (std::string item; std::getline(ss, item, ',');)
    if (!item.empty()) out.insert(item);
  return out;
}

// '#name', name, or id -> id, via slkx's cache.
static std::string resolve(const json& cache, std::string ch) {
  if (ch.empty()) return "";
  if (ch[0] == '#') ch.erase(0, 1);
  auto names = cache.value("channels", json::object());
  if (names.contains(ch)) return names[ch].get<std::string>();
  if (cache.value("ids", json::object()).contains(ch)) return ch;
  fail("unknown channel " + ch + " (try: slkx channels --refresh)");
}

// anton -> us: {"action":"review","pr":url} when you press v, and
// {"event":"approved","pr":url} when GitHub says you approved. Each becomes
// slkx reactions, shown at once and undone if slkx fails.
static void serve_one(Fold& fold, std::mutex& mu, const std::string& review_name, const std::string& line);

static void serve_stdin(Fold& fold, std::mutex& mu, const std::string& review_name) {
  for (std::string line; std::getline(std::cin, line);) {
    try {
      serve_one(fold, mu, review_name, line);
    } catch (const std::exception& e) {  // one bad request is skipped, not fatal
      std::fprintf(stderr, "anton-slkx: %s\n", e.what());
    }
  }
}

static void serve_one(Fold& fold, std::mutex& mu, const std::string& review_name, const std::string& line) {
  json in = json::parse(line, nullptr, false);
  if (!in.is_object() || !in.contains("pr") || !in["pr"].is_string()) return;
  std::string url = normalize_url(in["pr"].get<std::string>());
  bool toggle = in.value("action", json()) == "review";
  bool approved = in.value("event", json()) == "approved";
  if (url.empty() || (!toggle && !approved)) return;
  std::vector<React> todo;
  {
    std::lock_guard lk(mu);
    todo = toggle ? fold.toggle_review(url) : fold.approved(url);
    if (todo.empty()) {
      if (toggle) std::fprintf(stderr, "anton-slkx: %s is not posted in %s\n", url.c_str(), review_name.c_str());
      return;
    }
    std::set<std::string> touched;
    for (auto& r : todo) {
      auto t = fold.event(fold.as_event(r));
      touched.insert(t.begin(), t.end());
    }
    for (auto& l : fold.changed(touched)) emit(l);
  }
  for (size_t i = 0; i < todo.size(); ++i) {
    const React& r = todo[i];
    std::vector<std::string> args = {slkx_bin, "react", "--message", r.ts, "--channel", r.ch, "--emoji", r.emoji};
    if (r.remove) args.push_back("--remove");
    Result res = run(args, "", false);
    if (res.code == 0) continue;
    // Stop here: undo this one and the ones never sent, so the fold matches Slack.
    std::lock_guard lk(mu);
    std::set<std::string> touched;
    for (size_t j = i; j < todo.size(); ++j) {
      auto t = fold.event(fold.as_event(todo[j], true));
      touched.insert(t.begin(), t.end());
    }
    for (auto& l : fold.changed(touched)) emit(l);
    std::string err = res.err.substr(0, res.err.find('\n'));
    if (err.empty()) err = "exit " + std::to_string(res.code);
    std::fprintf(stderr, "anton-slkx: :%s: %s failed: %s\n", r.emoji.c_str(), r.remove ? "removal" : "reaction",
                 err.c_str());
    return;
  }
}

static int run_plugin(int argc, char** argv) {
  FoldConfig c;
  std::string review, deploy;
  long backfill = 1000;
  bool custom_steps = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "anton-slkx: %s needs a value\n%s", a.c_str(), kUsage);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--review-channel") review = val();
    else if (a == "--deploy-channel") deploy = val();
    else if (a == "--review-emoji") {
      std::string v = val();
      c.review_emoji = split(v);
      c.review_add = v.substr(0, v.find(','));
      if (c.review_add.empty() && !c.review_emoji.empty()) c.review_add = *c.review_emoji.begin();
    }
    else if (a == "--deploy-clear-emoji") c.clear_emoji = split(val());
    else if (a == "--deploy-merged-emoji") c.merged_emoji = val();
    else if (a == "--backfill") {
      // A bad value is a warning, not a failure: the plugin still runs.
      std::string v = val();
      size_t nz = v.find_first_not_of('0');
      if (!all_digits(v) || nz == std::string::npos) {
        std::fprintf(stderr, "anton-slkx: --backfill %s is not a positive number, using 1000\n", v.c_str());
      } else if (v.size() - nz > 4 || std::stol(v.substr(nz)) > 1000) {
        std::fprintf(stderr, "anton-slkx: --backfill %s is over Slack's limit, using 1000\n", v.c_str());
      } else {
        backfill = std::stol(v.substr(nz));
      }
    }
    else if (a == "--slkx") slkx_bin = val();
    else if (a == "--deploy-state") {
      std::string v = val();
      auto eq = v.find('='), colon = v.find(':', eq);
      if (eq == std::string::npos || colon == std::string::npos) {
        std::fprintf(stderr, "anton-slkx: --deploy-state wants emoji=glyph:text, got %s\n", v.c_str());
        return 2;
      }
      if (!custom_steps) c.steps.clear();
      custom_steps = true;
      c.steps.push_back({v.substr(0, eq), v.substr(eq + 1, colon - eq - 1), v.substr(colon + 1)});
    } else if (a == "-h" || a == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else {
      std::fprintf(stderr, "anton-slkx: unknown argument %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
  if (review.empty() && deploy.empty()) {
    std::fprintf(stderr, "anton-slkx: give --review-channel and/or --deploy-channel\n%s", kUsage);
    return 2;
  }

  json cache = slkx_json({"channels", "--json"});
  c.review_ch = resolve(cache, review);
  c.deploy_ch = resolve(cache, deploy);
  json auth = slkx_json({"auth", "status", "--json"});
  if (auth.is_object() && auth.contains("user_id") && auth["user_id"].is_string())
    c.me = auth["user_id"].get<std::string>();
  json user_names = cache.value("user_names", json::object());  // .items() on a temporary dangles
  for (auto& [id, handle] : user_names.items())
    if (handle.is_string()) c.names[id] = handle.get<std::string>();
  Fold fold(c);

  // Watch starts first and waits for the backfill, so nothing that happens
  // in between is lost. Replaying an event the history already shows is a
  // no-op: the fold is set-based.
  std::mutex mu;
  std::condition_variable cv;
  bool ready = false;
  std::string last_err;
  Spawned w = spawn({slkx_bin, "watch"}, {}, "", false);  // stays in our group: anton's kill reaches it
  if (w.pid == -1) fail(w.error);
  watch_pid = w.pid;
  std::thread watcher([&] {
    int code = stream(
        w,
        [&](const std::string& line) {
          std::unique_lock lk(mu);
          cv.wait(lk, [&] { return ready; });
          try {
            json ev = json::parse(line, nullptr, false);
            for (auto& l : fold.changed(fold.event(ev))) emit(l);
          } catch (const std::exception&) {
            // one odd event (unexpected shape, bad UTF-8) is skipped, not fatal
          }
        },
        [&](const std::string& line) {
          std::lock_guard lk(mu);
          last_err = line;
        });
    watch_pid = -1;  // reaped: the pid may be reused
    std::lock_guard lk(mu);
    fail("slkx watch exited (" + std::to_string(code) + ")" +
         (last_err.empty() ? "" : ": " + last_err));
  });

  {
    std::lock_guard lk(mu);
    std::set<std::string> seen;
    for (const std::string& ch : {c.review_ch, c.deploy_ch}) {
      if (ch.empty()) continue;
      json msgs = slkx_json({"read", "--channel", ch, "--limit", std::to_string(backfill), "--json"});
      if (!msgs.is_array()) fail("slkx read: expected a list of messages");
      for (auto& m : msgs) {
        try {
          auto urls = fold.add_message(ch, m);
          seen.insert(urls.begin(), urls.end());
        } catch (const std::exception&) {
        }
      }
    }
    for (auto& l : fold.changed(seen)) emit(l);
    emit(R"({"health":"ok"})");
    if (!c.review_ch.empty())
      emit(R"({"actions":[{"id":"review","key":"v","label":"toggle 👀 reviewing in Slack"}]})");
    ready = true;
  }
  cv.notify_all();
  if (!c.review_ch.empty()) std::thread([&] { serve_stdin(fold, mu, review); }).detach();
  watcher.join();  // never returns: the watcher fail()s when slkx watch exits
  return 1;
}

int main(int argc, char** argv) {
  // Killed on its own (not as a group, as anton does): take slkx watch along.
  for (int sig : {SIGTERM, SIGHUP, SIGINT})
    ::signal(sig, [](int s) {
      if (pid_t w = watch_pid; w > 0) ::kill(w, SIGTERM);
      ::_exit(128 + s);
    });
  try {
    return run_plugin(argc, argv);
  } catch (const std::exception& e) {
    fail(e.what());
  }
}
