// Assert-based self-check for src/model.hpp.
#include <cassert>
#include <cstdio>

#include "model.hpp"

using namespace anton;

int main() {
  // compact: truncated, no decimals
  assert(compact(0) == "0");
  assert(compact(999) == "999");
  assert(compact(1000) == "1k");
  assert(compact(1999) == "1k");
  assert(compact(10000) == "10k");
  assert(compact(999999) == "999k");
  assert(compact(2500000) == "2M");

  // age
  assert(age(59) == "0m");
  assert(age(5 * 60) == "5m");
  assert(age(5 * 3600) == "5h");
  assert(age(2 * 86400) == "2d");
  assert(age(21 * 86400) == "3w");
  assert(age(120 * 86400) == "4mo");
  assert(age(400 * 86400) == "1y");

  // size bands with configurable thresholds
  assert(size_band(449, 450, 1000) == Band::Green);
  assert(size_band(450, 450, 1000) == Band::Yellow);
  assert(size_band(1000, 450, 1000) == Band::Red);

  assert(parse_time("1970-01-02T00:00:00Z") == 86400);
  assert(parse_time("nope") == 0);

  // state ranking
  Pr pr;
  assert(derive_state(pr, std::nullopt).text == "review");
  pr.my_review = "APPROVED";
  assert(derive_state(pr, std::nullopt).text == "you ✓");
  pr.review_decision = "APPROVED";
  assert(derive_state(pr, std::nullopt).text == "approved ✓");
  pr.checks = "FAILURE";
  assert(derive_state(pr, std::nullopt).kind == Kind::Failing);
  pr.draft = true;
  assert(derive_state(pr, std::nullopt).text == "draft");
  Status queued{"⚙", "queued"};
  assert(derive_state(pr, queued).text == "⚙ queued");  // custom beats GitHub signals
  pr.state = "MERGED";
  assert(derive_state(pr, queued).text == "merged");     // but never a finished PR

  // urls
  assert(normalize_url("https://GitHub.com/o/r/pull/12/files#diff") == "https://github.com/o/r/pull/12");
  assert(normalize_url("https://github.com/o/r/pull/12?x=1") == "https://github.com/o/r/pull/12");
  assert(normalize_url("o/r#12") == "https://github.com/o/r/pull/12");
  assert(normalize_url("https://github.com/Brick-So/Core/pull/7") == "https://github.com/brick-so/core/pull/7");
  assert(normalize_url("https://github.com/o/r/issues/12").empty());
  assert(normalize_url("o/r#x").empty());
  auto ref = parse_ref("https://github.com/brick-so/core/pull/20387");
  assert(ref && ref->owner == "brick-so" && ref->name == "core" && ref->number == 20387);

  // query editing
  assert(tokenize(R"(is:open label:"needs review" foo)").size() == 3);
  assert(with_scope("is:open org:a repo:b/c", "org:z") == "is:open org:z");
  assert(with_scope("is:open", "") == "is:open");
  assert(remove_qualifiers("is:open -org:a user:b x", kScopeKeys) == "is:open x");
  assert(ensure_is_pr("is:open") == "is:pr is:open");
  assert(ensure_is_pr("type:pr is:open") == "type:pr is:open");

  // local filter
  std::string err;
  assert(!parse_local("is:open sort:updated", err) && err.find("sort") != std::string::npos);
  assert(!parse_local("is:open hello", err));
  assert(!parse_local("is:locked", err));
  assert(!parse_local("review:approve", err));  // typo is an error, not a silent no-match
  assert(parse_local("is:Open review:APPROVED", err));
  auto preds = parse_local("is:open org:Brick-So -is:draft author:@me", err);
  assert(preds);
  Pr p2;
  p2.owner = "brick-so";
  p2.repo = "brick-so/core";
  p2.author = "paulo";
  assert(local_match(p2, *preds, "Paulo"));
  p2.draft = true;
  assert(!local_match(p2, *preds, "paulo"));
  p2.draft = false;
  assert(!local_match(p2, *preds, "someone"));
  p2.state = "MERGED";
  assert(local_match(p2, *parse_local("is:closed", err), ""));

  // plugin protocol: full state per line
  PluginState st;
  std::string url = "https://github.com/o/r/pull/1";
  assert(apply_line(st, R"({"pr":"o/r#1","lists":["code-review"],)"
                        R"("status":{"glyph":"⚙","text":"queued"},)"
                        R"("tags":[{"glyph":"👀","text":"alice"}]})") == url);
  assert(st.prs[url].lists.count("code-review"));
  assert(st.prs[url].status->text == "queued");
  assert(st.prs[url].tags.size() == 1);
  apply_line(st, R"({"pr":"https://github.com/o/r/pull/1","lists":["code-review"]})");
  assert(!st.prs[url].status && st.prs[url].tags.empty());  // omitted = none
  apply_line(st, R"({"health":"error","text":"expired"})");
  assert(st.health == "error" && st.health_text == "expired");
  apply_line(st, "not json");
  apply_line(st, R"({"pr":"nope"})");
  assert(st.bad_lines == 2);
  assert(apply_line(st, R"({"other":1})").empty());
  apply_line(st, R"({"pr":"o/r#1"})");
  assert(!st.prs.count(url));  // a clearing line forgets the PR

  // sorting
  std::vector<Pr> v(3);
  v[0].additions = 1;
  v[1].additions = 100;
  v[2].additions = 10;
  sort_prs(v, Sort::Size);
  assert(v[0].additions == 100 && v[2].additions == 1);

  std::puts("model_check: all checks passed");
}
