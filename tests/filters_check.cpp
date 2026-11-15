// Assert-based self-check for src/filters.hpp and src/config_write.hpp.
#include <cassert>
#include <cstdio>

#include "config_write.hpp"
#include "filters.hpp"

using namespace anton;

static const Chip& chip(const std::vector<Chip>& cs, const std::string& fam) {
  for (auto& c : cs)
    if (c.fam == fam) return c;
  assert(!"no such chip");
  return cs[0];
}

int main() {
  // -- chips -------------------------------------------------------------------
  assert(family("is:open") == "state" && family("-is:merged") == "state" && family("recent:24h") == "state");
  assert(family("is:draft") == "draft" && family("is:pr") == "pr" && family("review-requested:@me") == "review");
  assert(family("org:acme") == "repo" && family("label:bug") == "label" && family("flaky") == "text");
  assert(family("\"a:b c\"") == "text" && family("base:main") == "base");
  assert(chip_value("recent:24h") == "open + merged 24h");
  assert(chip_value("author:@me") == "me" && chip_value("org:acme") == "acme/*");
  assert(chip_value("-label:\"needs review\"") == "not needs review");
  assert(chip_value("review:changes_requested") == "changes requested");
  assert(chip_value("reviewed-by:@me") == "by me" && chip_value("-is:draft") == "hidden");

  // Set fixed chips first, then one per other token; is:pr never shows.
  auto cs = chips("is:pr is:open author:@me label:bug label:\"needs review\" base:main");
  assert(cs.size() == 7);
  assert(cs[0].fam == "state" && cs[0].set && cs[0].value == "open");
  assert(cs[1].fam == "author" && cs[1].value == "me");
  assert(cs[2].fam == "review" && !cs[2].set && cs[2].value == "any");
  assert(cs[3].fam == "repo" && cs[3].value == "all");
  assert(cs[4].label == "label" && cs[4].value == "bug" && cs[5].value == "needs review");
  assert(cs[6].label == "base" && cs[6].tokens == std::vector<std::string>{"base:main"});
  assert(chips("").size() == 4 && !chips("")[0].set);
  // Two tokens of one family share its chip, and removing it takes both.
  auto two = chips("is:open recent:24h");
  assert(chip(two, "state").value == "open, open + merged 24h");
  assert(without("is:open recent:24h label:x", chip(two, "state").tokens) == "label:x");

  // -- edits -------------------------------------------------------------------
  assert(with_family("is:open author:@me", "state", "is:merged") == "author:@me is:merged");
  assert(with_family("is:open author:@me", "author", "") == "is:open");
  assert(with_family("is:open is:draft", "state", "") == "is:draft");  // is:draft isn't a state
  assert(with_token("is:open", "label:bug") == "is:open label:bug");
  assert(with_token("is:open label:bug", "label:bug") == "is:open label:bug");
  assert(with_token("", "label:bug") == "label:bug");
  assert(without("a:1 b:2 a:1", {"a:1"}) == "b:2 a:1");
  assert(same_query("is:open author:@me", "author:@me  is:open"));
  assert(!same_query("is:open", "is:open is:open"));

  // -- menus -------------------------------------------------------------------
  Seen seen;
  seen.labels = {{"bug", 2}, {"billing", 1}, {"build system", 1}};
  seen.authors = {{"ren", 3}, {"mona", 1}};
  seen.owners = {"acme"};
  seen.repos = {"acme/core", "acme/web"};
  auto review = chip_options("review", seen);
  assert(review[0].token.empty() && review[4].token == "reviewed-by:@me" && review[4].local);
  assert(!review[5].local && review[5].search);  // "requested" needs GitHub
  auto state = chip_options("state", seen);
  assert(state[4].token == "recent:24h" && state[4].local && !state[4].search);
  auto authors = chip_options("author", seen);
  assert(authors[2].label == "ren" && authors[2].hint == "in 3 PRs");  // most seen first
  auto repo = chip_options("repo", seen);
  assert(repo[1].token == "org:acme" && repo[2].token == "repo:acme/core");
  auto labels = chip_options("label", seen);
  assert(labels[0].label == "bug" && labels.back().token == "label:\"build system\"");

  const AddKind& reviewer = add_kinds()[2];
  assert(reviewer.prefix == "reviewed-by:");
  auto rv = add_values(reviewer, seen, 0);
  assert(rv[0].token == "reviewed-by:@me" && rv[0].local && !rv[1].local);  // only @me is local
  const AddKind& updated = add_kinds()[6];
  auto up = add_values(updated, seen, 86400 * 8);
  assert(up[0].token == "updated:>=1970-01-08" && !up[0].local && up[1].token == "updated:>=1970-01-02");
  assert(typed_token(add_kinds()[0], "needs review") == "label:\"needs review\"");
  assert(typed_token(add_kinds().back(), "flaky") == "flaky in:title");
  assert(typed_token(add_kinds()[3], "x").empty());

  // -- completion ---------------------------------------------------------------
  auto c = complete("lab", seen, false);
  assert(c.size() == 1 && c[0].token == "label:");
  c = complete("label:b", seen, false);
  assert(c.size() == 3 && c[0].token == "label:bug" && c[0].hint == "seen in 2 PRs");
  assert(c[2].token == "label:\"build system\"");
  assert(complete("label:\"bui", seen, false)[0].token == "label:\"build system\"");
  assert(complete("label:bug", seen, false).empty());  // complete already
  assert(complete("-is:dr", seen, false)[0].token == "-is:draft");
  assert(complete("a", seen, true).size() == 1);   // list views: author only (assignee is search-only)
  assert(complete("a", seen, false).size() == 3);  // author, assignee, archived
  assert(complete("rec", seen, false).empty());    // recent: is list views' own
  assert(complete("author:", seen, false)[0].token == "author:@me");
  assert(complete("", seen, false).empty());
  assert(word_start("is:open lab", 11) == 8);
  assert(word_start("label:\"a b", 10) == 0);
  assert(word_start("x ", 2) == 2);

  // -- writing config.json --------------------------------------------------------
  std::string file = R"({
  "zeta": 1,
  "refresh_seconds": 120,
  "people": {"ren": {"name": "ren", "hex": "#a2a5d8"}},
  "views": [
    {"name": "Mine", "query": "is:open author:@me", "pinned": true},
    {"name": "Code review", "plugin": "slack", "list": "code-review", "query": "is:open"}
  ],
  "columns": ["repo", {"name": "CI", "width": 8, "command": ["sh", "-c", "echo $HOME"]}, "title"],
  "plugins": [{"name": "slack", "command": ["anton-slkx", "--review-channel", "#code-review"]}]
})";
  Config cfg;
  assert(parse_config(file, cfg).empty());
  assert(cfg.columns[1].command[2] == "echo " + std::string(std::getenv("HOME") ? std::getenv("HOME") : ""));
  cfg.refresh_seconds = 300;
  cfg.views[0].query = "is:open author:@me label:bug";
  cfg.views[1].sort = Sort::Size;
  cfg.views.push_back({"New", "is:open", "", "", Sort::Updated});
  cfg.columns[1].hidden = true;  // off, still in the file
  cfg.columns[0].hidden = true;
  std::swap(cfg.columns[0], cfg.columns[2]);
  std::string err, out = merged_config(file, cfg, err);
  assert(err.empty());
  auto j = nlohmann::ordered_json::parse(out);
  std::vector<std::string> keys;
  for (auto& [k, v] : j.items()) keys.push_back(k);
  assert((keys == std::vector<std::string>{"zeta", "refresh_seconds", "people", "views", "columns", "plugins"}));
  assert(j["zeta"] == 1 && j["refresh_seconds"] == 300);
  assert(j["people"]["ren"]["hex"] == "#a2a5d8");
  assert(j["plugins"][0]["command"][2] == "#code-review");
  assert(j["views"][0]["pinned"] == true && j["views"][0]["query"] == "is:open author:@me label:bug");
  assert(j["views"][1]["sort"] == "size" && j["views"][1]["plugin"] == "slack");
  assert(j["views"][2] == nlohmann::ordered_json::parse(R"({"name": "New", "query": "is:open"})"));  // updated: default
  assert(j["columns"][0] == "title");
  assert(j["columns"][1]["command"][2] == "echo $HOME");  // as written, not expanded
  assert(j["columns"][1]["hidden"] == true && j["columns"][1]["width"] == 8);
  assert(j["columns"][2] == nlohmann::ordered_json::parse(R"({"name": "repo", "hidden": true})"));
  assert(out.find("\n  \"zeta\": 1,\n") != std::string::npos);  // 2-space indent
  assert(!j.contains("watch_seconds") && !j.contains("mouse") && !j.contains("size"));  // defaults stay out
  // What it wrote reads back as what was set.
  Config back;
  assert(parse_config(out, back).empty());
  cfg.views[2].sort.reset();  // written as the default it is
  assert(back.views == cfg.views && back.refresh_seconds == 300);
  assert(back.columns.size() == 3 && back.columns[2].hidden && back.columns[1].hidden && !back.columns[0].hidden);

  // Turning a column back on drops "hidden"; a default view list isn't added.
  cfg.columns[2].hidden = false;
  out = merged_config(out, cfg, err);
  assert(nlohmann::ordered_json::parse(out)["columns"][2] == "repo");
  Config fresh;
  fresh.mouse = false;
  fresh.green_below = 300;
  out = merged_config("", fresh, err);
  assert(err.empty());
  j = nlohmann::ordered_json::parse(out);
  assert(j["mouse"] == false && j["size"]["green_below"] == 300 && !j.contains("views") && !j.contains("columns"));
  fresh.start_view = "Assigned";
  assert(nlohmann::ordered_json::parse(merged_config(out, fresh, err))["start_view"] == "Assigned");

  // A file anton can't rewrite faithfully is left alone, with the reason.
  assert(merged_config("{ \"a\": ", fresh, err).empty() && err.find("valid JSON") != std::string::npos);
  err.clear();
  assert(merged_config("{ // mine\n \"a\": 1 }", fresh, err).empty() && err.find("comments") != std::string::npos);
  err.clear();
  assert(merged_config("[1]", fresh, err).empty() && !err.empty());

  // Parsing: hidden built-ins, per-view sort.
  Config p;
  assert(parse_config(R"({"columns": [{"name": "repo", "hidden": true}, "title"]})", p).empty());
  assert(p.columns[0].hidden && p.columns[0].command.empty());
  assert(!parse_config(R"({"columns": [{"name": "repo", "hidden": true}]})", p).empty());  // nothing left
  assert(!parse_config(R"({"columns": [{"name": "nope", "hidden": true}]})", p).empty());
  Config q;
  assert(!parse_config(R"({"views": [{"name": "a", "sort": "list"}]})", q).empty());  // search views have no list
  assert(!parse_config(R"({"views": [{"name": "a", "sort": "age"}]})", q).empty());
  std::puts("filters_check: ok");
}
