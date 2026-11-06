#pragma once
// Pure logic: PR data, state derivation, formatting, query editing, the local
// query filter, and the plugin protocol fold. No I/O, so tests/model_check.cpp
// covers it without gh, a terminal, or a plugin.

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace anton {

// -- PR ----------------------------------------------------------------------

struct Pr {
  std::string url;      // normalized, see normalize_url
  std::string repo;     // owner/name
  std::string owner;
  int number = 0;
  std::string title;
  std::string author;
  std::string state = "OPEN";     // OPEN | CLOSED | MERGED
  std::string review_decision;    // APPROVED | CHANGES_REQUESTED | REVIEW_REQUIRED | ""
  std::string checks;             // SUCCESS | FAILURE | ERROR | PENDING | EXPECTED | ""
  std::string my_review;          // viewer's latest review state, "" if none
  std::string my_review_at;       // ...and when it was submitted
  bool draft = false;
  long additions = 0, deletions = 0;
  std::time_t created = 0, updated = 0;
  bool partial = false;  // only url/repo/number/state known (see fetch_states)
};

struct Tag {
  std::string glyph, text, hint;
};

struct Status {
  std::string glyph, text;
};

// -- formatting --------------------------------------------------------------

// 999 -> "999", 1999 -> "1k" (truncated: the column means "at least"),
// 10000 -> "10k", 2500000 -> "2M".
inline std::string compact(long n) {
  if (n < 0) n = 0;
  if (n < 1000) return std::to_string(n);
  if (n < 1000000) return std::to_string(n / 1000) + "k";
  return std::to_string(n / 1000000) + "M";
}

// Seconds elapsed -> 5m, 5h, 2d, 3w, 4mo, 1y.
inline std::string age(long secs) {
  if (secs < 0) secs = 0;
  long m = secs / 60, h = m / 60, d = h / 24;
  if (h < 1) return std::to_string(m) + "m";
  if (d < 1) return std::to_string(h) + "h";
  if (d < 7) return std::to_string(d) + "d";
  if (d < 30) return std::to_string(d / 7) + "w";
  if (d < 365) return std::to_string(d / 30) + "mo";
  return std::to_string(d / 365) + "y";
}

enum class Band { Green, Yellow, Red };

inline Band size_band(long total, long green_below, long red_from) {
  if (total >= red_from) return Band::Red;
  if (total < green_below) return Band::Green;
  return Band::Yellow;
}

// "2026-11-05T12:34:56Z" -> epoch seconds; 0 when unparsable.
inline std::time_t parse_time(const std::string& iso) {
  std::tm tm{};
  if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                  &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
    return 0;
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  return timegm(&tm);
}

// -- state -------------------------------------------------------------------

enum class Kind { Done, Custom, Draft, Failing, Changes, Approved, Mine, Review };

struct Shown {
  Kind kind;
  std::string text;
};

// The STATE column. Order is the spec's: merged/closed, plugin status, then
// GitHub's own signals.
inline Shown derive_state(const Pr& pr, const std::optional<Status>& custom) {
  if (pr.state == "MERGED") return {Kind::Done, "merged"};
  if (pr.state == "CLOSED") return {Kind::Done, "closed"};
  if (custom) {
    std::string t = custom->glyph.empty() ? custom->text : custom->glyph + " " + custom->text;
    return {Kind::Custom, t};
  }
  if (pr.draft) return {Kind::Draft, "draft"};
  if (pr.checks == "FAILURE" || pr.checks == "ERROR") return {Kind::Failing, "✗ checks"};
  if (pr.review_decision == "CHANGES_REQUESTED") return {Kind::Changes, "changes"};
  if (pr.review_decision == "APPROVED") return {Kind::Approved, "approved ✓"};
  if (pr.my_review == "APPROVED") return {Kind::Mine, "you ✓"};
  if (pr.my_review == "CHANGES_REQUESTED") return {Kind::Mine, "you ✎"};
  return {Kind::Review, "review"};
}

// -- URLs --------------------------------------------------------------------

inline std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool all_digits(const std::string& s) {
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// https://GitHub.com/O/R/pull/12/files#x -> https://github.com/o/r/pull/12
// o/r#12 -> https://github.com/o/r/pull/12. "" when it is not a PR reference.
// Lowercased whole: GitHub owner/repo names are case-insensitive, and the URL
// is the key that joins plugin lines to fetched PRs.
inline std::string normalize_url(std::string s) {
  s = lower(std::move(s));
  auto cut = s.find_first_of("?#");
  bool short_form = s.find("://") == std::string::npos;
  if (short_form) {
    auto hash = s.find('#');
    auto slash = s.find('/');
    if (hash == std::string::npos || slash == std::string::npos || slash > hash) return "";
    std::string num = s.substr(hash + 1);
    if (!all_digits(num)) return "";
    return "https://github.com/" + s.substr(0, hash) + "/pull/" + num;
  }
  if (cut != std::string::npos) s.resize(cut);
  auto scheme_end = s.find("://") + 3;
  auto host_end = s.find('/', scheme_end);
  if (host_end == std::string::npos) return "";
  auto pull = s.find("/pull/", host_end);
  if (pull == std::string::npos) return "";
  auto num_start = pull + 6;
  auto num_end = num_start;
  while (num_end < s.size() && std::isdigit(static_cast<unsigned char>(s[num_end]))) ++num_end;
  if (num_end == num_start) return "";
  return s.substr(0, num_end);
}

// https://github.com/o/r/pull/12 -> {"o", "r", 12}
struct PrRef {
  std::string owner, name;
  int number = 0;
};

inline std::optional<PrRef> parse_ref(const std::string& url) {
  auto p = url.find("github.com/");
  if (p == std::string::npos) return std::nullopt;
  std::string rest = url.substr(p + 11);  // o/r/pull/12
  auto a = rest.find('/'), b = rest.find("/pull/");
  if (a == std::string::npos || b == std::string::npos || a >= b) return std::nullopt;
  try {
    return PrRef{rest.substr(0, a), rest.substr(a + 1, b - a - 1), std::stoi(rest.substr(b + 6))};
  } catch (...) {
    return std::nullopt;
  }
}

// -- query tokens ------------------------------------------------------------

// Whitespace split that keeps quoted runs together: label:"needs review".
inline std::vector<std::string> tokenize(const std::string& q) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : q) {
    if (c == '"') quoted = !quoted;
    if (std::isspace(static_cast<unsigned char>(c)) && !quoted) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

inline std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& t : v) s += (s.empty() ? "" : " ") + t;
  return s;
}

// "-org:x" and "org:x" both have key "org".
inline std::string qualifier_key(const std::string& tok) {
  auto colon = tok.find(':');
  if (colon == std::string::npos) return "";
  std::string k = tok.substr(0, colon);
  if (!k.empty() && k[0] == '-') k.erase(0, 1);
  return k;
}

inline std::string remove_qualifiers(const std::string& q, const std::set<std::string>& keys) {
  std::vector<std::string> kept;
  for (auto& t : tokenize(q))
    if (!keys.count(qualifier_key(t))) kept.push_back(t);
  return join(kept);
}

// The org/repo scope: o and p replace, x clears.
inline const std::set<std::string> kScopeKeys = {"org", "user", "repo"};

inline std::string with_scope(const std::string& q, const std::string& scope) {
  if (scope.empty()) return q;
  std::string base = remove_qualifiers(q, kScopeKeys);
  return base.empty() ? scope : base + " " + scope;
}

// Search views always search PRs.
inline std::string ensure_is_pr(const std::string& q) {
  for (auto& t : tokenize(q))
    if (t == "is:pr" || t == "type:pr") return q;
  return q.empty() ? "is:pr" : "is:pr " + q;
}

// -- local filter (list views) ----------------------------------------------

struct Pred {
  std::string key, value;
  bool neg = false;
};

// Parses the subset a list view can answer from fetched fields. Returns
// nullopt and sets err for anything else, so it is never a silent no-op.
inline std::optional<std::vector<Pred>> parse_local(const std::string& q, std::string& err) {
  static const std::set<std::string> keys = {"is", "org", "user", "repo", "author", "review"};
  std::vector<Pred> out;
  for (auto t : tokenize(q)) {
    Pred p;
    if (!t.empty() && t[0] == '-') {
      p.neg = true;
      t.erase(0, 1);
    }
    auto colon = t.find(':');
    if (colon == std::string::npos) {
      err = "list views don't support free text: " + t;
      return std::nullopt;
    }
    p.key = t.substr(0, colon);
    p.value = lower(t.substr(colon + 1));
    if (!keys.count(p.key)) {
      err = "list views don't support " + p.key + ":";
      return std::nullopt;
    }
    if (p.key == "is" && p.value != "open" && p.value != "closed" && p.value != "merged" &&
        p.value != "draft" && p.value != "pr") {
      err = "list views don't support is:" + p.value;
      return std::nullopt;
    }
    if (p.key == "review" && p.value != "approved" && p.value != "changes_requested" &&
        p.value != "required" && p.value != "none") {
      err = "review: takes approved, changes_requested, required or none";
      return std::nullopt;
    }
    out.push_back(p);
  }
  return out;
}

inline bool local_match(const Pr& pr, const std::vector<Pred>& preds, const std::string& viewer) {
  for (const auto& p : preds) {
    bool hit = true;
    std::string v = lower(p.value);
    if (p.key == "is") {
      if (v == "open") hit = pr.state == "OPEN";
      else if (v == "closed") hit = pr.state != "OPEN";  // GitHub: merged PRs are closed too
      else if (v == "merged") hit = pr.state == "MERGED";
      else if (v == "draft") hit = pr.draft;
      else hit = true;  // is:pr
    } else if (p.key == "org" || p.key == "user") {
      hit = lower(pr.owner) == v;
    } else if (p.key == "repo") {
      hit = lower(pr.repo) == v;
    } else if (p.key == "author") {
      hit = lower(pr.author) == (v == "@me" ? lower(viewer) : v);
    } else if (p.key == "review") {
      if (v == "approved") hit = pr.review_decision == "APPROVED";
      else if (v == "changes_requested") hit = pr.review_decision == "CHANGES_REQUESTED";
      else if (v == "required") hit = pr.review_decision == "REVIEW_REQUIRED";
      else if (v == "none") hit = pr.review_decision.empty();
      else hit = false;
    }
    if (hit == p.neg) return false;
  }
  return true;
}

// -- plugin protocol ---------------------------------------------------------

struct PrInfo {
  std::set<std::string> lists;
  std::optional<Status> status;
  std::vector<Tag> tags;
  std::optional<double> order;  // list views' default sort: higher first
  bool watch = false;            // re-fetch every watch_seconds (e.g. you're reviewing it)
};

// A key the plugin handles: anton sends {"action": id, "pr": url} on stdin.
struct Action {
  std::string id, key, label;
};

struct PluginState {
  std::map<std::string, PrInfo> prs;  // by normalized URL
  std::vector<Action> actions;        // the latest "actions" line
  std::string health;                 // "" until the plugin says
  std::string health_text;
  std::string last_stderr;
  int bad_lines = 0;
};

inline std::string str(const nlohmann::json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

// Folds one stdout line. Returns the URL it touched, "" otherwise.
inline std::string apply_line(PluginState& st, const std::string& line) {
  auto j = nlohmann::json::parse(line, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    ++st.bad_lines;
    return "";
  }
  if (j.contains("health")) {
    st.health = str(j, "health");
    st.health_text = str(j, "text");
  }
  if (auto it = j.find("actions"); it != j.end() && it->is_array()) {
    st.actions.clear();
    for (auto& a : *it)  // one-character keys only: anton's own keys win
      if (a.is_object() && !str(a, "id").empty() && str(a, "key").size() == 1)
        st.actions.push_back({str(a, "id"), str(a, "key"), str(a, "label")});
  }
  if (!j.contains("pr")) return "";
  std::string url = normalize_url(str(j, "pr"));
  if (url.empty()) {
    ++st.bad_lines;
    return "";
  }
  PrInfo info;  // full state: omitted fields mean "none"
  if (auto it = j.find("lists"); it != j.end() && it->is_array())
    for (auto& l : *it)
      if (l.is_string()) info.lists.insert(l.get<std::string>());
  if (auto it = j.find("status"); it != j.end() && it->is_object() && !str(*it, "text").empty())
    info.status = Status{str(*it, "glyph"), str(*it, "text")};
  if (auto it = j.find("tags"); it != j.end() && it->is_array())
    for (auto& t : *it)
      if (t.is_object() && !str(t, "text").empty())
        info.tags.push_back({str(t, "glyph"), str(t, "text"), str(t, "hint")});
  if (auto it = j.find("order"); it != j.end() && it->is_number()) info.order = it->get<double>();
  if (auto it = j.find("watch"); it != j.end() && it->is_boolean()) info.watch = it->get<bool>();
  if (info.lists.empty() && !info.status && info.tags.empty() && !info.watch)
    st.prs.erase(url);  // {"pr": url} alone: the plugin has nothing to say about it
  else
    st.prs[url] = std::move(info);
  return url;
}

// -- sorting -----------------------------------------------------------------

// List: the plugin's `order` (list views only; their default).
enum class Sort { List, Updated, Created, Size, Repo };

inline const char* sort_name(Sort s) {
  switch (s) {
    case Sort::List: return "list";
    case Sort::Updated: return "updated";
    case Sort::Created: return "created";
    case Sort::Size: return "size";
    case Sort::Repo: return "repo";
  }
  return "";
}

inline void sort_prs(std::vector<Pr>& v, Sort s) {
  std::stable_sort(v.begin(), v.end(), [s](const Pr& a, const Pr& b) {
    switch (s) {
      case Sort::List: return false;  // keeps the order rows() built
      case Sort::Updated: return a.updated > b.updated;
      case Sort::Created: return a.created > b.created;
      case Sort::Size: return a.additions + a.deletions > b.additions + b.deletions;
      case Sort::Repo: return a.repo != b.repo ? a.repo < b.repo : a.number > b.number;
    }
    return false;
  });
}

}  // namespace anton
