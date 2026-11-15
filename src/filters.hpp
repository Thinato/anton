#pragma once
// Pure: the filter bar and the query editor as two views of one query
// string (01-anton.md, "Filter bar"). Chips are read off the query's
// tokens, and every chip or menu edit is an edit of the query, so the two
// never disagree. tests/filters_check.cpp covers it.

#include <algorithm>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "model.hpp"

namespace anton {

inline std::string unquote(const std::string& v) {
  return v.size() >= 2 && v.front() == '"' && v.back() == '"' ? v.substr(1, v.size() - 2) : v;
}

inline std::string quote_if_space(const std::string& v) {
  return v.find(' ') == std::string::npos ? v : '"' + v + '"';
}

// The chip a token belongs to: state, review, author, repo, draft, text
// (free words), pr (is:pr, never shown), or the qualifier's own key.
inline std::string family(const std::string& tok) {
  std::string t = tok.size() > 1 && tok[0] == '-' ? tok.substr(1) : tok;
  auto colon = t.find(':');
  if (colon == std::string::npos || t[0] == '"') return "text";
  std::string key = lower(t.substr(0, colon)), v = lower(t.substr(colon + 1));
  if (key == "is" || key == "type") {
    if (v == "open" || v == "closed" || v == "merged" || v == "unmerged") return "state";
    if (v == "draft") return "draft";
    if (v == "pr" || v == "pull-request") return "pr";
    return "is";
  }
  if (key == "state" || key == "recent") return "state";
  if (key == "review" || key == "reviewed-by" || key == "review-requested" || key == "user-review-requested" ||
      key == "team-review-requested")
    return "review";
  if (key == "repo" || key == "org" || key == "user") return "repo";
  return key;
}

// How a chip shows one of its tokens: "open", "by me", "acme/*", "not bug".
inline std::string chip_value(const std::string& tok) {
  bool neg = tok.size() > 1 && tok[0] == '-';
  std::string t = neg ? tok.substr(1) : tok, fam = family(tok);
  if (fam == "draft") return neg ? "hidden" : "only";
  if (fam == "text") return (neg ? "not " : "") + unquote(t);
  auto colon = t.find(':');
  std::string key = lower(t.substr(0, colon)), v = unquote(t.substr(colon + 1));
  auto me = [](const std::string& s) { return s == "@me" ? std::string("me") : s; };
  std::string s = v;
  if (fam == "state") s = key == "recent" ? "open + merged " + v : v;
  else if (key == "review") s = v == "changes_requested" ? "changes requested" : v == "required" ? "needs review" : v;
  else if (key == "reviewed-by") s = "by " + me(v);
  else if (fam == "review") s = "asked " + me(v);
  else if (fam == "author") s = me(v);
  else if (key == "org" || key == "user") s = v + "/*";
  return neg ? "not " + s : s;
}

// One chip of the filter bar. A set one is removed by taking its tokens out.
struct Chip {
  std::string fam, label, value;
  bool set = false;
  std::vector<std::string> tokens;
};

// state, review, author, repo always (set ones first, so what filters is
// leftmost), then a chip per other token, in query order.
inline std::vector<Chip> chips(const std::string& q) {
  std::vector<Chip> fixed = {{"state", "state", "any", false, {}},
                             {"review", "review", "any", false, {}},
                             {"author", "author", "anyone", false, {}},
                             {"repo", "repo", "all", false, {}}};
  std::vector<Chip> rest;
  for (auto& t : tokenize(q)) {
    std::string f = family(t);
    if (f == "pr") continue;
    auto it = std::find_if(fixed.begin(), fixed.end(), [&](const Chip& c) { return c.fam == f; });
    if (it == fixed.end()) {
      rest.push_back({f, f, chip_value(t), true, {t}});
      continue;
    }
    it->value = it->set ? it->value + ", " + chip_value(t) : chip_value(t);
    it->set = true;
    it->tokens.push_back(t);
  }
  std::stable_partition(fixed.begin(), fixed.end(), [](const Chip& c) { return c.set; });
  fixed.insert(fixed.end(), rest.begin(), rest.end());
  return fixed;
}

// -- edits: each returns the new query --------------------------------------

inline bool has_token(const std::string& q, const std::string& tok) {
  auto ts = tokenize(q);
  return std::find(ts.begin(), ts.end(), tok) != ts.end();
}

// Takes out the first occurrence of each of toks.
inline std::string without(const std::string& q, const std::vector<std::string>& toks) {
  auto ts = tokenize(q);
  for (auto& t : toks)
    if (auto it = std::find(ts.begin(), ts.end(), t); it != ts.end()) ts.erase(it);
  return join(ts);
}

// Appends tok (which may be several tokens) unless it's there already.
inline std::string with_token(const std::string& q, const std::string& tok) {
  if (tok.empty() || has_token(q, tok)) return q;
  return q.empty() ? tok : q + " " + tok;
}

// Replaces every token of a family with tok ("" just removes them).
inline std::string with_family(const std::string& q, const std::string& fam, const std::string& tok) {
  std::vector<std::string> kept;
  for (auto& t : tokenize(q))
    if (family(t) != fam) kept.push_back(t);
  return with_token(join(kept), tok);
}

// The same filter, whatever the order of its tokens.
inline bool same_query(const std::string& a, const std::string& b) {
  auto x = tokenize(a), y = tokenize(b);
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return x == y;
}

// -- menus -------------------------------------------------------------------

// What results have shown this session: menu and completion values.
struct Seen {
  std::map<std::string, int> labels, authors;  // name -> PRs it's on
  std::set<std::string> owners, repos;
};

// local: a list view can answer it; search: GitHub can. A menu greys out
// what the active view can't use, and says why.
struct Option {
  std::string label, token, hint;
  bool local = true, search = true;
};

inline std::vector<std::string> by_count(const std::map<std::string, int>& m, size_t n) {
  std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
  std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  std::vector<std::string> out;
  for (size_t i = 0; i < v.size() && i < n; ++i) out.push_back(v[i].first);
  return out;
}

inline std::string seen_hint(int n) { return n > 0 ? "in " + std::to_string(n) + (n == 1 ? " PR" : " PRs") : ""; }

// A chip's menu: the token each choice puts in place of the family's.
inline std::vector<Option> chip_options(const std::string& fam, const Seen& seen) {
  std::vector<Option> o;
  if (fam == "state") {
    o = {{"any", "", "", true, true},
         {"open", "is:open", "", true, true},
         {"closed", "is:closed", "", true, true},
         {"merged", "is:merged", "", true, true},
         {"open + merged 24h", "recent:24h", "", true, false},
         {"open + merged 7d", "recent:7d", "", true, false}};
  } else if (fam == "review") {
    o = {{"any", "", "", true, true},
         {"approved", "review:approved", "", true, true},
         {"changes requested", "review:changes_requested", "", true, true},
         {"still needs review", "review:required", "", true, true},
         {"reviewed by me", "reviewed-by:@me", "", true, true},
         {"requested from me", "user-review-requested:@me", "", false, true},
         {"requested from me or my teams", "review-requested:@me", "", false, true}};
  } else if (fam == "author") {
    o = {{"anyone", "", "", true, true}, {"me", "author:@me", "", true, true}};
    for (auto& a : by_count(seen.authors, 8)) o.push_back({a, "author:" + a, seen_hint(seen.authors.at(a))});
  } else if (fam == "repo") {
    o = {{"all", "", "", true, true}};
    for (auto& w : seen.owners) o.push_back({w + "/*", "org:" + w, "", true, true});
    for (auto& r : seen.repos) o.push_back({r, "repo:" + r, "", true, true});
  } else if (fam == "draft") {
    o = {{"only drafts", "is:draft", "", true, true}, {"hide drafts", "-is:draft", "", true, true}};
  } else if (fam == "label") {
    for (auto& l : by_count(seen.labels, 30))
      o.push_back({l, "label:" + quote_if_space(l), seen_hint(seen.labels.at(l))});
  }
  return o;
}

// "+ filter": a qualifier to add, then a value for it.
struct AddKind {
  std::string label, prefix;  // prefix + value is the token
  bool local, search;
};

inline const std::vector<AddKind>& add_kinds() {
  static const std::vector<AddKind> k = {
      {"label", "label:", true, true},          {"author", "author:", true, true},
      {"reviewer", "reviewed-by:", true, true}, {"draft", "is:draft", true, true},
      {"checks", "status:", false, true},       {"base branch", "base:", false, true},
      {"updated", "updated:", false, true},     {"involves", "involves:", false, true},
      {"text in title", "in:title", false, true}};
  return k;
}

inline std::string ymd(std::time_t t) {
  std::tm tm{};
  gmtime_r(&t, &tm);
  char b[16];
  std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
  return b;
}

// The values a kind offers; what you type is taken too (typed_token).
inline std::vector<Option> add_values(const AddKind& k, const Seen& seen, std::time_t now) {
  std::vector<Option> o;
  auto people = [&](bool local_others) {
    o.push_back({"me", k.prefix + "@me", "", true, true});
    for (auto& a : by_count(seen.authors, 8))
      o.push_back({a, k.prefix + a, seen_hint(seen.authors.at(a)), local_others, true});
  };
  if (k.prefix == "label:") o = chip_options("label", seen);
  else if (k.prefix == "author:") people(true);
  else if (k.prefix == "reviewed-by:") people(false);  // list views know only your own review
  else if (k.prefix == "involves:") people(false);
  else if (k.prefix == "is:draft") o = chip_options("draft", seen);
  else if (k.prefix == "status:")
    o = {{"passing", "status:success", "", false, true},
         {"failing", "status:failure", "", false, true},
         {"pending", "status:pending", "", false, true}};
  else if (k.prefix == "base:") o = {{"main", "base:main", "", false, true}};
  else if (k.prefix == "updated:")
    for (auto [label, days] : {std::pair{"in the last day", 1}, {"in the last week", 7}, {"in the last month", 30}})
      o.push_back({label, "updated:>=" + ymd(now - days * 86400L), "", false, true});
  for (auto& v : o) v.local = v.local && k.local;
  return o;
}

// What a kind makes of typed text: label:"needs review", "flaky test" in:title.
inline std::string typed_token(const AddKind& k, const std::string& typed) {
  if (typed.empty() || k.prefix == "is:draft") return "";
  if (k.prefix == "in:title") return quote_if_space(typed) + " in:title";
  return k.prefix + quote_if_space(typed);
}

// -- completion --------------------------------------------------------------

// Qualifier names the editor completes; local: list views take it.
struct Qualifier {
  const char* name;
  bool local, search;
};

inline const std::vector<Qualifier>& qualifiers() {
  static const std::vector<Qualifier> q = {
      {"is", true, true},         {"author", true, true},          {"label", true, true},
      {"review", true, true},     {"reviewed-by", true, true},     {"repo", true, true},
      {"org", true, true},        {"user", true, true},            {"recent", true, false},
      {"assignee", false, true},  {"mentions", false, true},       {"involves", false, true},
      {"review-requested", false, true}, {"user-review-requested", false, true},
      {"status", false, true},    {"base", false, true},           {"head", false, true},
      {"draft", false, true},     {"created", false, true},        {"updated", false, true},
      {"closed", false, true},    {"merged", false, true},         {"comments", false, true},
      {"in", false, true},        {"no", false, true},             {"archived", false, true}};
  return q;
}

// Completions for the word being typed: qualifier names ("lab" ->
// "label:"), then values ("label:bu" -> "label:bug"). At most 8; the word
// itself isn't one.
inline std::vector<Option> complete(const std::string& word, const Seen& seen, bool list_view) {
  std::vector<Option> out;
  if (word.empty()) return out;
  bool neg = word[0] == '-';
  std::string w = neg ? word.substr(1) : word, sign = neg ? "-" : "";
  auto colon = w.find(':');
  if (colon == std::string::npos) {
    for (auto& q : qualifiers())
      if (std::string(q.name).starts_with(lower(w)) && (list_view ? q.local : q.search))
        out.push_back({std::string(q.name) + ":", sign + q.name + ":", "", q.local, q.search});
  } else {
    std::string key = lower(w.substr(0, colon)), typed = lower(unquote(w.substr(colon + 1)));
    if (!typed.empty() && typed[0] == '"') typed.erase(0, 1);  // label:"bu, still open
    std::vector<std::pair<std::string, std::string>> vals;    // value, hint
    auto add = [&](std::initializer_list<const char*> vs) {
      for (auto v : vs) vals.push_back({v, ""});
    };
    if (key == "is") add({"open", "closed", "merged", "draft"});
    else if (key == "review") add({"approved", "changes_requested", "required", "none"});
    else if (key == "status") add({"success", "failure", "pending"});
    else if (key == "draft" || key == "archived") add({"true", "false"});
    else if (key == "recent") add({"24h", "7d"});
    else if (key == "label")
      for (auto& l : by_count(seen.labels, 100)) vals.push_back({quote_if_space(l), "seen " + seen_hint(seen.labels.at(l))});
    else if (key == "repo")
      for (auto& r : seen.repos) vals.push_back({r, ""});
    else if (key == "org" || key == "user")
      for (auto& o : seen.owners) vals.push_back({o, ""});
    else if (key == "author" || key == "reviewed-by" || key == "involves" || key == "mentions" ||
             key == "assignee" || key == "review-requested" || key == "user-review-requested") {
      vals.push_back({"@me", ""});
      for (auto& a : by_count(seen.authors, 100)) vals.push_back({a, seen_hint(seen.authors.at(a))});
    }
    for (auto& [v, hint] : vals) {
      std::string bare = lower(unquote(v));
      if (bare.starts_with(typed) && bare != typed) out.push_back({key + ":" + v, sign + key + ":" + v, hint});
    }
  }
  if (out.size() > 8) out.resize(8);
  return out;
}

// The palettes' match (the TUI's `?`, anton-gui's Ctrl K): needle's
// characters in hay in order, ignoring case ("bcore" finds "brick-so/core").
// The byte offsets of hay that matched, to highlight them: a contiguous run
// when needle appears whole ("temoc" in "Run temoc specs"), else the
// earliest subsequence. nullopt when it doesn't match; empty for "".
inline std::optional<std::vector<size_t>> fuzzy_find(const std::string& hay, const std::string& needle) {
  std::vector<size_t> at;
  if (auto whole = lower(hay).find(lower(needle)); whole != std::string::npos) {
    for (size_t i = 0; i < needle.size(); ++i) at.push_back(whole + i);
    return at;
  }
  for (size_t i = 0; i < hay.size() && at.size() < needle.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(hay[i])) == std::tolower(static_cast<unsigned char>(needle[at.size()])))
      at.push_back(i);
  if (at.size() < needle.size()) return std::nullopt;
  return at;
}

// The word of `text` that ends at byte `cursor`: [start, cursor).
inline size_t word_start(const std::string& text, size_t cursor) {
  bool quoted = false;
  size_t start = 0;
  for (size_t i = 0; i < cursor && i < text.size(); ++i) {
    if (text[i] == '"') quoted = !quoted;
    if (text[i] == ' ' && !quoted) start = i + 1;
  }
  return start;
}

}  // namespace anton
