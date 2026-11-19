// Assert-based self-check for src/core.cpp's notify views: told once a
// session, never for the baseline, a PR re-entering or a plugin restart.
#include <cassert>
#include <cstdio>

#include "config_write.hpp"
#include "core.hpp"

using namespace anton;
using nlohmann::json;

struct TestCore : Core {
  using Core::Core;
  void post(std::function<void()> fn) override { fn(); }
};

static std::string url(int n) { return "https://github.com/acme/api/pull/" + std::to_string(n); }
static Pr pr(int n, const std::string& decision = "REVIEW_REQUIRED", bool draft = false) {
  Pr p;
  p.url = url(n);
  p.repo = "acme/api";
  p.owner = "acme";
  p.number = n;
  p.title = "t" + std::to_string(n);
  p.review_decision = decision;
  p.draft = draft;
  return p;
}

int main() {
  const std::string file = R"({
    "views": [{"name": "My turn", "plugin": "slack", "list": "review-turn",
               "query": "is:open -is:draft review:required", "notify": true}],
    "plugins": [{"name": "slack", "command": ["anton-slkx"]}]
  })";
  Config cfg;
  assert(parse_config(file, cfg).empty());
  assert(cfg.views[0].notify);
  // written back as it was; a search view never carries it
  std::string err;
  assert(json::parse(merged_config(file, cfg, err))["views"][0]["notify"] == true);
  Config search = cfg;
  search.views[0].plugin = search.views[0].list = "";
  assert(!json::parse(merged_config(file, search, err))["views"][0].contains("notify"));

  TestCore c(cfg);
  std::vector<std::string> told;
  c.notifier = [&](const std::string& title, const std::string& body) { told.push_back(title + ": " + body); };
  auto line = [&](const json& j) { c.on_plugin_line(0, j.dump(), false); };
  auto turn = [&](int n) { line({{"pr", url(n)}, {"lists", {"code-review", "review-turn"}}}); };
  auto off = [&](int n) { line({{"pr", url(n)}, {"lists", {"code-review"}}}); };
  auto fetched = [&](const std::vector<Pr>& prs) {
    Fetch f;
    f.prs = prs;
    c.on_fetched("", f, false);
  };
  auto refetching = [&](int n) {
    std::lock_guard lk(c.mu);
    return c.want_full.count(url(n)) > 0;
  };

  // the backfill, before health ok: not news, even once fetched
  turn(1);
  turn(5);
  fetched({pr(1)});
  line({{"health", "ok"}});
  fetched({pr(1), pr(5, "REVIEW_REQUIRED", true)});
  assert(told.empty());

  // a PR joining is re-fetched first, then told once
  turn(2);
  assert(told.empty() && refetching(2));
  fetched({pr(2)});
  assert((told == std::vector<std::string>{"My turn: acme/api#2 t2"}));
  assert(c.notice == "My turn: acme/api#2 t2");
  fetched({pr(2)});
  assert(told.size() == 1);

  // leaving and coming back: not told again
  off(2);
  turn(2);
  fetched({pr(2)});
  assert(told.size() == 1);

  // joins approved: not told; told when it needs a review again
  turn(3);
  fetched({pr(3, "APPROVED")});
  assert(told.size() == 1);
  fetched({pr(3)});
  assert(told.size() == 2 && told[1] == "My turn: acme/api#3 t3");

  // judged on the re-fetch, not on what the cache had
  fetched({pr(4)});
  turn(4);
  assert(told.size() == 2);
  fetched({pr(4, "APPROVED")});
  assert(told.size() == 2);

  // in the baseline as a draft: news once it's ready
  fetched({pr(5)});
  fetched({pr(5)});  // the re-fetch asked for when it failed the filter has landed either way
  assert(told.size() == 3 && told[2] == "My turn: acme/api#5 t5");

  // a plugin restart backfills again: nothing is told twice
  c.on_plugin_exit(0, 1);
  for (int n : {1, 2, 3, 4, 5}) turn(n);
  line({{"health", "ok"}});
  fetched({pr(1), pr(2), pr(3), pr(4, "APPROVED"), pr(5)});
  fetched({pr(1), pr(2), pr(3), pr(4, "APPROVED"), pr(5)});
  assert(told.size() == 3);

  // a view whose filter changes starts from what it holds then
  c.tabs[0].saved->query = "is:open";
  fetched({pr(4, "APPROVED")});
  assert(told.size() == 3);
  turn(6);
  fetched({pr(6, "APPROVED")});
  assert(told.size() == 4 && told[3] == "My turn: acme/api#6 t6");

  // views without notify never tell
  c.tabs[0].saved->notify = false;
  turn(7);
  fetched({pr(7)});
  fetched({pr(7)});
  assert(told.size() == 4);

  std::puts("core_check: all checks passed");
}
