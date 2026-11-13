#pragma once
// anton-llm's pure core: llm.json -> reviewers, a reviewer + PR -> argv, run
// results -> the PR's plugin line. No I/O, so tests/llm_check.cpp drives it.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"  // expand_home
#include "model.hpp"   // normalize_url, parse_ref

namespace anton::llm {

using nlohmann::json;
using Vars = std::map<std::string, std::string>;
using Fields = std::map<std::string, json>;  // a string or an object

// One configured key: a command that runs an agent on a PR.
struct Reviewer {
  std::string key, label, tag;
  std::string stdin_from;  // "" or "diff"
  std::string cwd;         // "" = the plugin's cwd
  std::string checkout = "pr";  // with a clone in "repos": "pr" (a worktree of the head) or "as-is"
  int timeout = 600;       // seconds
  std::vector<std::string> command;
  Fields fields;  // every string or object field, for {name}
};

inline const std::set<std::string> kBuiltins = {"pr_url", "repo", "number", "note", "tmp", "repo_dir"};

inline std::string config_path() {
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/anton/llm.json";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.config/anton/llm.json";
}

// /Users/me/.config/anton/llm.json -> ~/.config/anton/llm.json, for messages.
inline std::string tilde(const std::string& path) {
  const char* h = std::getenv("HOME");
  std::string home = h ? h : "";
  if (!home.empty() && path.rfind(home + "/", 0) == 0) return "~" + path.substr(home.size());
  return path;
}

inline bool is_ident(const std::string& s) {
  if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (unsigned char c : s)
    if (!std::isalnum(c) && c != '_') return false;
  return true;
}

// Replaces {name} and $PR_URL / ${PR_URL} in s. {name} is a built-in, else
// one of the reviewer's fields. At depth 0 (a command arg) a field's text is
// itself expanded once (depth 1); at depth 1 fields go in as written, so
// nothing recurses. Built-in values (a note is agent output) are never
// expanded. An unknown {name} stays as written and, at depth 0, is added to
// `unknown`; text that isn't {identifier} (JSON in a prompt) is left alone.
// An object field goes in as compact JSON; at depth 0 its strings are
// expanded first (depth 1), so a value can't break out of its string.
inline std::string subst(const std::string& s, const Vars& builtins, const Fields& fields, int depth = 0,
                         std::set<std::string>* unknown = nullptr) {
  std::function<json(const json&)> fill = [&](const json& j) {
    if (j.is_string()) return json(subst(j.get<std::string>(), builtins, fields, 1));
    json o = j;
    if (o.is_structured())  // iterating a number yields the number itself
      for (auto& v : o) v = fill(v);
    return o;
  };
  std::string out;
  for (size_t i = 0; i < s.size();) {
    bool braced = s.compare(i, 9, "${PR_URL}") == 0;
    if (braced || s.compare(i, 7, "$PR_URL") == 0) {
      out += builtins.at("pr_url");
      i += braced ? 9 : 7;
      continue;
    }
    size_t end = s[i] == '{' ? s.find('}', i) : std::string::npos;
    std::string name = end == std::string::npos ? "" : s.substr(i + 1, end - i - 1);
    if (!is_ident(name)) {
      out += s[i++];
      continue;
    }
    if (auto b = builtins.find(name); b != builtins.end()) out += b->second;
    else if (auto f = fields.find(name); f != fields.end()) {
      if (f->second.is_string()) out += (depth == 0 ? fill(f->second) : f->second).get<std::string>();
      else out += (depth == 0 ? fill(f->second) : f->second).dump(-1, ' ', false, json::error_handler_t::replace);
    } else {
      out += s.substr(i, end - i + 1);
      if (unknown && depth == 0) unknown->insert(name);
    }
    i = end + 1;
  }
  return out;
}

// tmp: the run's own temp directory; repo_dir: the PR's checkout, or "".
inline Vars builtins(const std::string& url, const std::string& note, const std::string& tmp = "",
                     const std::string& repo_dir = "") {
  auto ref = parse_ref(url);
  return {{"pr_url", url},
          {"repo", ref ? ref->owner + "/" + ref->name : ""},
          {"number", ref ? std::to_string(ref->number) : ""},
          {"note", note},
          {"tmp", tmp},
          {"repo_dir", repo_dir}};
}

inline std::vector<std::string> argv(const Reviewer& r, const Vars& b) {
  std::vector<std::string> out;
  for (auto& a : r.command) out.push_back(subst(a, b, r.fields));
  return out;
}

// Parses llm.json; returns "" or the first problem found. `repos` gets
// "owner/name" (lowercase, as in a normalized URL) -> local clone.
inline std::string parse_config(const std::string& text, std::vector<Reviewer>& out, Vars* repos = nullptr) {
  json j = json::parse(text, nullptr, false, true);  // allow comments
  if (j.is_discarded() || !j.is_object()) return "not a JSON object";
  if (auto rp = j.find("repos"); rp != j.end()) {
    if (!rp->is_object()) return "repos: want {\"owner/name\": \"path to a clone\"}";
    for (auto& [k, v] : rp->items()) {
      if (!v.is_string() || std::count(k.begin(), k.end(), '/') != 1)
        return "repos: want {\"owner/name\": \"path to a clone\"}, not \"" + k + "\"";
      if (repos) (*repos)[lower(k)] = expand_home(v.get<std::string>());
    }
  }
  auto rs = j.find("reviewers");
  if (rs == j.end() || !rs->is_array() || rs->empty()) return "reviewers: want a non-empty list";
  std::set<std::string> keys;
  for (size_t i = 0; i < rs->size(); ++i) {
    const json& v = (*rs)[i];
    std::string where = "reviewers[" + std::to_string(i) + "]";
    if (!v.is_object()) return where + ": not an object";
    Reviewer r;
    for (auto& [k, val] : v.items()) {
      if (!val.is_string() && !val.is_object()) continue;
      if (kBuiltins.count(k)) return where + ": \"" + k + "\" is a built-in, pick another name";
      r.fields[k] = val;
    }
    r.key = str(v, "key");
    if (r.key.size() != 1) return where + ": key must be one character";
    where = "reviewer " + r.key;
    if (!keys.insert(r.key).second) return where + ": key used twice";
    r.tag = str(v, "tag");
    if (r.tag.empty()) r.tag = r.key;
    r.label = str(v, "label");
    if (r.label.empty()) r.label = "review with " + r.tag;
    r.stdin_from = str(v, "stdin");
    if (!r.stdin_from.empty() && r.stdin_from != "diff") return where + ": stdin must be \"diff\" or absent";
    r.cwd = expand_home(str(v, "cwd"));
    if (std::string c = str(v, "checkout"); !c.empty()) r.checkout = c;
    if (r.checkout != "pr" && r.checkout != "as-is") return where + ": checkout must be \"pr\" or \"as-is\"";
    if (auto t = v.find("timeout"); t != v.end()) {
      if (!t->is_number_integer() || t->get<long>() <= 0 || t->get<long>() > 86400)
        return where + ": timeout must be 1-86400 seconds";
      r.timeout = t->get<int>();
    }
    auto c = v.find("command");
    if (c == v.end() || !c->is_array() || c->empty()) return where + ": command must be a non-empty list";
    for (auto& a : *c) {
      if (!a.is_string()) return where + ": command must be a list of strings";
      r.command.push_back(expand_home(a.get<std::string>()));
    }
    std::set<std::string> unknown;
    Vars probe = builtins("https://github.com/o/r/pull/1", "");
    for (auto& a : r.command) subst(a, probe, r.fields, 0, &unknown);
    if (!unknown.empty()) return where + ": command uses {" + *unknown.begin() + "}, which is not a field or built-in";
    out.push_back(std::move(r));
  }
  return "";
}

inline std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

inline size_t escaped_size(const std::string& s) {
  return json(s).dump(-1, ' ', false, json::error_handler_t::replace).size();
}

// s, cut so that it is at most `budget` bytes as a JSON string, with a marker
// saying so. Cuts at a UTF-8 boundary.
inline std::string fit(const std::string& s, size_t budget) {
  static const std::string mark = "\n\n[truncated]";
  if (escaped_size(s) <= budget) return s;
  for (size_t n = s.size(); n > 0;) {
    n = n * 9 / 10;  // ponytail: 10% per step re-escapes each time; fine for 1 MiB
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    std::string cut = s.substr(0, n) + mark;
    if (escaped_size(cut) <= budget) return cut;
  }
  return mark;
}

inline constexpr size_t kMaxLine = 65536;  // anton drops longer lines (02-plugins.md)

// What this plugin knows about one PR, across reviewers.
struct Run {
  enum State { kRunning, kOk, kFailed } state = kRunning;
  std::string error;  // kFailed: markdown
};
struct PrState {
  std::map<size_t, Run> runs;  // by reviewer index: config order
  std::string output;          // the last successful run's stdout: {note}
};

// The full `pr` line for url: a tag per reviewer that ran, and a note made
// of the failures, then the last good output. Fits kMaxLine.
inline std::string pr_line(const std::string& url, const PrState& st, const std::vector<Reviewer>& rs) {
  json j = {{"pr", url}};
  std::string note;
  for (auto& [i, run] : st.runs) {
    const Reviewer& r = rs[i];
    json t = {{"glyph", "🤖"}};
    if (run.state == Run::kRunning) t["text"] = r.tag + "…", t["hint"] = r.label + ": running";
    else if (run.state == Run::kOk) t["text"] = r.tag, t["hint"] = r.label;
    else {
      t["text"] = r.tag + " failed";
      t["hint"] = r.label + ": " + run.error.substr(0, run.error.find('\n'));
      note += "**" + r.tag + " failed**: " + run.error + "\n\n";
    }
    j["tags"].push_back(t);
  }
  note += st.output;
  note = trim(note);
  auto dump = [&] { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
  if (!note.empty()) {
    size_t rest = dump().size() + 10;  // + ,"note":
    j["note"] = fit(note, rest < kMaxLine ? kMaxLine - rest : 0);
  }
  return dump();
}

// -- --check-review: a Claude Code PreToolUse hook on Bash ---------------------

// cmd's words, split into simple commands at unquoted ; & | ( ) ` and
// newlines. Quotes and backslashes are honoured; nothing is expanded.
inline std::vector<std::vector<std::string>> shell_split(const std::string& cmd) {
  std::vector<std::vector<std::string>> out(1);
  std::string w;
  bool word = false;
  auto end = [&] {
    if (word) out.back().push_back(w);
    w.clear();
    word = false;
  };
  for (size_t i = 0; i < cmd.size(); ++i) {
    char c = cmd[i];
    if (c == '\'') {
      size_t e = std::min(cmd.find('\'', i + 1), cmd.size());
      w += cmd.substr(i + 1, e - i - 1), i = e, word = true;
    } else if (c == '"') {
      for (++i; i < cmd.size() && cmd[i] != '"'; ++i) {
        if (cmd[i] == '\\' && i + 1 < cmd.size() && std::string_view("\"\\$`\n").find(cmd[i + 1]) != std::string_view::npos) ++i;
        w += cmd[i];
      }
      word = true;
    } else if (c == '\\' && i + 1 < cmd.size()) {
      w += cmd[++i], word = true;
    } else if (c == ' ' || c == '\t') {
      end();
    } else if (std::string_view(";&|()`\n").find(c) != std::string_view::npos) {
      end();
      if (!out.back().empty()) out.emplace_back();
    } else {
      w += c, word = true;
    }
  }
  end();
  if (out.back().empty()) out.pop_back();
  return out;
}

using ReadFile = std::function<std::optional<std::string>(const std::string&)>;

// Why the Bash command cmd (run in cwd) must not run, or "". Blocks what can
// approve, request changes, merge or close: `gh pr review` with those flags,
// `gh pr merge|close`, `gh api graphql`, `gh api` writes to pulls/N/merge, and
// a review POST (pulls/N/reviews, or .../reviews/ID/events) unless it is one
// plain command whose --input file has "event": "COMMENT".
inline std::string command_block(const std::string& cmd, const std::string& cwd, const ReadFile& read,
                                 int depth = 0) {
  for (auto& seg : shell_split(cmd)) {
    for (auto& w : seg)  // sh -c '…', "$(…)": check what's inside too
      if (depth < 3 && w.find_first_of(" \t\n;&|`") != std::string::npos)
        if (std::string why = command_block(w, cwd, read, depth + 1); !why.empty()) return why;
    size_t g = 0;
    while (g < seg.size() && seg[g] != "gh" && !seg[g].ends_with("/gh")) ++g;
    if (g == seg.size()) continue;
    std::vector<std::string> a(seg.begin() + g + 1, seg.end()), pos;  // pos: subcommand words
    for (size_t i = 0; i < a.size() && pos.size() < 2; ++i) {
      if (a[i] == "-R" || a[i] == "--repo") ++i;
      else if (!a[i].starts_with("-")) pos.push_back(a[i]);
    }
    pos.resize(2);
    if (pos[0] == "pr" && (pos[1] == "merge" || pos[1] == "close")) return "gh pr " + pos[1] + " is not allowed";
    if (pos[0] == "pr" && pos[1] == "review")
      for (auto& w : a)
        if (w.starts_with("--approve") || w.starts_with("--request-changes") ||
            (w.size() > 1 && w[0] == '-' && w[1] != '-' && w.find_first_of("ar") != std::string::npos))
          return "gh pr review may only comment, not approve or request changes";
    if (pos[0] != "api") continue;

    static const std::set<std::string> takes = {"-X", "--method", "-H", "--header", "-f", "--raw-field",
                                                "-F", "--field", "--input", "-q", "--jq", "-t", "--template",
                                                "--hostname", "--cache", "-p", "--preview"};
    std::string method, endpoint, input;
    bool fields = false, has_input = false;
    for (size_t i = std::find(a.begin(), a.end(), "api") - a.begin() + 1; i < a.size(); ++i) {
      std::string f = a[i], v;
      if (size_t eq = f.find('='); f.starts_with("--") && eq != std::string::npos) v = f.substr(eq + 1), f.resize(eq);
      else if (f.size() > 2 && f[1] != '-' && takes.count(f.substr(0, 2))) v = f.substr(2), f.resize(2);  // -XPOST
      else if (takes.count(f) && i + 1 < a.size()) v = a[++i];
      if (f == "-X" || f == "--method") {
        method = v;
        for (auto& ch : method) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
      else if (f == "--input") input = v, has_input = true;
      else if (f == "-f" || f == "-F" || f == "--field" || f == "--raw-field") fields = true;
      else if (!f.starts_with("-") && endpoint.empty()) endpoint = f;
    }
    if (method.empty()) method = fields || has_input ? "POST" : "GET";  // what gh does
    endpoint = endpoint.substr(0, endpoint.find('?'));
    auto is = [&](const char* re) { return std::regex_search(endpoint, std::regex(re, std::regex::icase)); };
    if (is(R"((^|/)graphql/?$)")) return "gh api graphql is not allowed (it can approve and merge)";
    if (method != "GET" && is(R"((^|/)repos/[^/]+/[^/]+/pulls/[^/]+/merge/?$)")) return "merging is not allowed";
    if (method != "POST" || !is(R"((^|/)repos/[^/]+/[^/]+/pulls/[^/]+/reviews(/[^/]+/events)?/?$)")) continue;
    if (depth > 0 || cmd.find_first_of(";&|<>()`$\n") != std::string::npos)
      return "post a review as one plain gh api command, nothing around it";
    if (fields || input.empty() || input == "-") return "post a review with --input FILE and no -f/-F fields";
    std::string path = input[0] == '/' ? input : cwd + "/" + input;
    auto text = read(path);
    if (!text) return "can't read " + path;
    int events = 0;  // {"event": "COMMENT", "event": "APPROVE"}: which one wins is the server's call
    json body = json::parse(*text, [&](int d, json::parse_event_t e, json& k) {
      if (e == json::parse_event_t::key && d == 1 && k == "event") ++events;
      return true;
    }, false);
    std::string event = body.is_object() ? str(body, "event") : "";
    if (events != 1 || event != "COMMENT")
      return "a review's \"event\" must be \"COMMENT\" once, not " +
             (events > 1 ? "twice" : event.empty() ? "missing" : "\"" + event + "\"");
  }
  return "";
}

// The verdict on a PreToolUse hook's stdin: "" lets the call run.
inline std::string review_block(const json& in, const ReadFile& read) {
  if (!in.is_object()) return "can't parse the hook input";
  if (str(in, "tool_name") != "Bash") return "";
  auto ti = in.find("tool_input");
  return command_block(ti != in.end() && ti->is_object() ? str(*ti, "command") : "", str(in, "cwd"), read);
}

}  // namespace anton::llm
