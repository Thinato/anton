#pragma once
// anton-llm's pure core: llm.json -> reviewers, a reviewer + PR -> argv, run
// results -> the PR's plugin line. No I/O, so tests/llm_check.cpp drives it.

#include <cctype>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"  // expand_home
#include "model.hpp"   // normalize_url, parse_ref

namespace anton::llm {

using nlohmann::json;
using Vars = std::map<std::string, std::string>;

// One configured key: a command that runs an agent on a PR.
struct Reviewer {
  std::string key, label, tag;
  std::string stdin_from;  // "" or "diff"
  std::string cwd;         // "" = the plugin's cwd
  int timeout = 600;       // seconds
  std::vector<std::string> command;
  Vars fields;  // every string field, for {name}
};

inline const std::set<std::string> kBuiltins = {"pr_url", "repo", "number", "note"};

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
inline std::string subst(const std::string& s, const Vars& builtins, const Vars& fields, int depth = 0,
                         std::set<std::string>* unknown = nullptr) {
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
    else if (auto f = fields.find(name); f != fields.end())
      out += depth == 0 ? subst(f->second, builtins, fields, 1) : f->second;
    else {
      out += s.substr(i, end - i + 1);
      if (unknown && depth == 0) unknown->insert(name);
    }
    i = end + 1;
  }
  return out;
}

inline Vars builtins(const std::string& url, const std::string& note) {
  auto ref = parse_ref(url);
  return {{"pr_url", url},
          {"repo", ref ? ref->owner + "/" + ref->name : ""},
          {"number", ref ? std::to_string(ref->number) : ""},
          {"note", note}};
}

inline std::vector<std::string> argv(const Reviewer& r, const Vars& b) {
  std::vector<std::string> out;
  for (auto& a : r.command) out.push_back(subst(a, b, r.fields));
  return out;
}

// Parses llm.json; returns "" or the first problem found.
inline std::string parse_config(const std::string& text, std::vector<Reviewer>& out) {
  json j = json::parse(text, nullptr, false, true);  // allow comments
  if (j.is_discarded() || !j.is_object()) return "not a JSON object";
  auto rs = j.find("reviewers");
  if (rs == j.end() || !rs->is_array() || rs->empty()) return "reviewers: want a non-empty list";
  std::set<std::string> keys;
  for (size_t i = 0; i < rs->size(); ++i) {
    const json& v = (*rs)[i];
    std::string where = "reviewers[" + std::to_string(i) + "]";
    if (!v.is_object()) return where + ": not an object";
    Reviewer r;
    for (auto& [k, val] : v.items()) {
      if (!val.is_string()) continue;
      if (kBuiltins.count(k)) return where + ": \"" + k + "\" is a built-in, pick another name";
      r.fields[k] = val.get<std::string>();
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

}  // namespace anton::llm
