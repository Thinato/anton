#pragma once
// anton-temoc's pure core: temoc.json -> Config, which changed files are
// specs, temoc's JSONL events -> counts, note and marks, and the PR's plugin
// line. No I/O, so tests/temoc_check.cpp drives it.

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../llm/llm.hpp"  // fit, trim, tilde, config_path, kMaxLine
#include "config.hpp"      // expand_home
#include "model.hpp"       // lower, str

namespace anton::temoc {

using nlohmann::json;
using llm::fit;
using llm::kMaxLine;
using llm::trim;

struct Repo {
  std::string dir;     // the clone
  std::string config;  // temoc's config, relative to the checkout unless absolute
  std::string checkout = "pr";  // "pr" (a worktree of the head) or "as-is"
  std::map<std::string, std::string> env;
};

struct Config {
  std::string key = "T";
  int timeout = 900;  // seconds
  std::map<std::string, Repo> repos;  // by "owner/name", lowercase as in a normalized URL
};

// Parses temoc.json; returns "" or the first problem found.
inline std::string parse_config(const std::string& text, Config& c) {
  json j = json::parse(text, nullptr, false, true);  // allow comments
  if (j.is_discarded() || !j.is_object()) return "not a JSON object";
  if (j.contains("key")) {
    c.key = str(j, "key");
    if (c.key.size() != 1) return "key must be one character";
  }
  if (auto t = j.find("timeout"); t != j.end()) {
    if (!t->is_number_integer() || t->get<long>() <= 0 || t->get<long>() > 86400)
      return "timeout must be 1-86400 seconds";
    c.timeout = t->get<int>();
  }
  auto rp = j.find("repos");
  if (rp == j.end()) return "";
  if (!rp->is_object()) return "repos: want {\"owner/name\": {\"dir\": \"~/path/to/clone\", \"config\": \"Config.toml\"}}";
  for (auto& [k, v] : rp->items()) {
    std::string where = "repos " + k;
    if (std::count(k.begin(), k.end(), '/') != 1) return where + ": want owner/name";
    if (!v.is_object()) return where + ": want {\"dir\": \"~/path/to/clone\", \"config\": \"Config.toml\"}";
    Repo r;
    r.dir = expand_home(str(v, "dir"));
    r.config = expand_home(str(v, "config"));
    if (r.dir.empty()) return where + ": dir is required, the path to a clone";
    if (r.config.empty()) return where + ": config is required, temoc's config file";
    if (std::string ch = str(v, "checkout"); !ch.empty()) r.checkout = ch;
    if (r.checkout != "pr" && r.checkout != "as-is") return where + ": checkout must be \"pr\" or \"as-is\"";
    if (auto e = v.find("env"); e != v.end()) {
      if (!e->is_object()) return where + ": env must be an object of strings";
      for (auto& [ek, ev] : e->items()) {
        if (!ev.is_string()) return where + ": env must be an object of strings";
        r.env[ek] = expand_home(ev.get<std::string>());
      }
    }
    c.repos[lower(k)] = std::move(r);
  }
  return "";
}

inline bool is_markdown(const std::string& path) { return lower(path).ends_with(".md"); }

// True when markdown text holds a temoc test: a `[//]: # "decisionTable …"`
// line (or `(…)`, `'…'`), as temoc's parser reads it.
// Not inside a ``` or ~~~ fenced block, where temoc sees no directive.
inline bool has_directive(const std::string& text) {
  static const std::regex re(R"(^ {0,3}\[//\]:[ \t]*#[ \t]+["'(]decisionTable )");
  std::istringstream in(text);
  std::string fence;  // the open fence's marker
  for (std::string l; std::getline(in, l);) {
    size_t i = l.find_first_not_of(' ');
    if (i <= 3 && (l.compare(i, 3, "```") == 0 || l.compare(i, 3, "~~~") == 0)) {
      if (fence.empty()) fence = l.substr(i, 3);
      else if (fence == l.substr(i, 3)) fence.clear();
    } else if (fence.empty() && std::regex_search(l, re)) {
      return true;
    }
  }
  return false;
}

// A note on one line of a file in the PR's head (02-plugins.md, "marks").
struct Mark {
  std::string file;
  int line = 0;  // 0: the file as a whole
  std::string level, glyph, text;
};

struct RunError {
  std::string kind, message, file, stderr_tail;
};

// One press of the key: what was run and what temoc said.
struct Outcome {
  std::string error;    // temoc couldn't be run: why (markdown, first line = the gist)
  std::string nothing;  // nothing to run: why
  std::string ref;      // the head sha the run used
  std::vector<std::string> files;  // what temoc was asked to run
  // temoc's events, folded
  bool finished = false;  // saw run_finished
  std::string status;     // run_finished's
  std::map<std::string, int> counts;  // rows by status, from run_finished
  std::map<std::string, std::string> file_status;
  std::map<std::string, std::vector<std::string>> problems;  // by file: markdown list items
  std::vector<RunError> run_errors;
  std::vector<Mark> marks;
  // the process
  int exit_code = 0;
  bool timed_out = false;
  int timeout = 0;
  std::string err;  // the end of temoc's own stderr
};

// A value temoc got back, as text: {"kind":"value","value":"4"} -> 4.
inline std::string render(const json& a) {
  std::string kind = str(a, "kind");
  if (kind == "value") return str(a, "value");
  if (kind == "void") return "nothing";
  if (kind == "exception") return str(a, "message");
  if (kind == "missing_symbol") return "no symbol $" + str(a, "symbol");
  if (kind == "protocol_error") return "protocol error: " + str(a, "message");
  if (kind == "list" && a.contains("value") && a["value"].is_array()) {
    std::string out = "[";
    for (auto& v : a["value"]) out += (out.size() > 1 ? ", " : "") + render(v);
    return out + "]";
  }
  return a.is_null() ? "?" : a.dump(-1, ' ', false, json::error_handler_t::replace);
}

// A check that didn't pass, in a line: `sum: expected 4, got 3`.
inline std::string describe(const json& c) {
  std::string who = str(c, "method");
  if (who.empty()) who = str(c, "role");
  const json& a = c.contains("actual") ? c["actual"] : json();
  std::string kind = str(a, "kind");
  if (c.contains("expected") && (kind == "value" || kind == "void" || kind == "list"))
    return who + ": expected " + str(c, "expected") + ", got " + render(a);
  return who + ": " + render(a);
}

// Rows by status, worst first: `1 failed, 11 passed, 4 snoozed`.
inline std::string counts_text(const std::map<std::string, int>& counts) {
  std::string out;
  for (const char* s : {"error", "failed", "passed", "snoozed", "skipped"})
    if (auto it = counts.find(s); it != counts.end() && it->second > 0)
      out += (out.empty() ? "" : ", ") + std::to_string(it->second) + " " + s;
  return out.empty() ? "no rows" : out;
}

// A status's mark: failed and error are both errors.
inline std::pair<std::string, std::string> look(const std::string& status) {
  if (status == "passed") return {"ok", "✓"};
  if (status == "snoozed") return {"warn", "💤"};
  if (status == "skipped") return {"dim", "–"};
  return {"error", "✗"};
}

inline std::map<std::string, int> counts_of(const json& ev) {
  std::map<std::string, int> out;
  if (auto c = ev.find("counts"); c != ev.end() && c->is_object())
    for (auto& [k, v] : c->items())
      if (v.is_number_integer()) out[k] = v.get<int>();
  return out;
}

// Folds one of temoc's --output jsonl events into o. Unknown events and
// fields are ignored, as temoc asks.
inline void fold(Outcome& o, const json& ev) {
  if (!ev.is_object()) return;
  std::string type = str(ev, "type"), file = str(ev, "file"), status = str(ev, "status");
  auto l = ev.find("line");  // not ev.value(): a wrong type would throw out of the run's reader
  int line = l != ev.end() && l->is_number_integer() ? l->get<int>() : 0;
  auto mark = [&](int at, const std::string& st, const std::string& text) {
    auto [level, glyph] = look(st);
    o.marks.push_back({file, at, level, glyph, text});
  };
  if (type == "row_finished") {
    std::string detail, until;
    if (auto cs = ev.find("checks"); cs != ev.end() && cs->is_array())
      for (auto& c : *cs) {
        std::string s = str(c, "status");
        if (s == "passed" || s == "skipped") continue;
        detail += (detail.empty() ? "" : "; ") + describe(c);
        if (until.empty()) until = str(c, "snoozed_until");
      }
    if (std::string m = str(ev, "message"); !m.empty()) detail += (detail.empty() ? "" : "; ") + m;
    if (status == "snoozed") detail = "snoozed until " + until + (detail.empty() ? "" : ": " + detail);
    else if (status == "skipped") detail = "skipped";
    if (status == "failed" || status == "error")
      o.problems[file].push_back("`" + file + ":" + std::to_string(line) + "` " + detail);
    mark(line, status, detail);
  } else if (type == "table_finished") {
    mark(line, status, counts_text(counts_of(ev)));
  } else if (type == "file_finished") {
    o.file_status[file] = status;
    mark(0, status, counts_text(counts_of(ev)));
  } else if (type == "setup_failed") {
    std::string what = str(ev, "phase") + " failed: " + render(ev.contains("actual") ? ev["actual"] : json());
    if (status == "snoozed") what += " (snoozed until " + str(ev, "snoozed_until") + ")";
    else o.problems[file].push_back("`" + file + ":" + std::to_string(line) + "` " + what);
    mark(line, status == "snoozed" ? status : "error", what);
  } else if (type == "run_error") {
    o.run_errors.push_back({str(ev, "kind"), str(ev, "message"), file, str(ev, "stderr")});
    if (!file.empty()) mark(0, "error", str(ev, "kind") + ": " + str(ev, "message"));
  } else if (type == "run_finished") {
    o.finished = true;
    o.status = status;
    o.counts = counts_of(ev);
  }
}

// Why the run as a whole failed, or "": temoc couldn't run, didn't finish,
// reported a run_error, or exited with something other than 0 or 1.
inline std::string failure(const Outcome& o) {
  if (!o.error.empty()) return o.error;
  if (o.timed_out) return "timed out after " + std::to_string(o.timeout) + "s";
  if (!o.run_errors.empty()) {
    auto& e = o.run_errors.front();
    return e.kind + (e.file.empty() ? "" : " in " + e.file) + ": " + e.message;
  }
  if (!o.finished) return "temoc stopped without finishing (exit " + std::to_string(o.exit_code) + ")";
  if (o.status == "cancelled") return "temoc was cancelled";
  if (o.exit_code != 0 && o.exit_code != 1) return "temoc exited with " + std::to_string(o.exit_code);
  return "";
}

// The TAGS text for finished rows: `14 ✓`, `2 ✗ 12 ✓ 💤 4`. Failed and error
// rows are ✗; snoozed ones don't fail, as in temoc.
inline std::string tally(const std::map<std::string, int>& c) {
  auto n = [&](const char* s) { auto it = c.find(s); return it == c.end() ? 0 : it->second; };
  int bad = n("failed") + n("error"), ok = n("passed"), zz = n("snoozed"), skip = n("skipped");
  std::string out;
  auto add = [&](const std::string& s) { out += (out.empty() ? "" : " ") + s; };
  if (bad) add(std::to_string(bad) + " ✗");
  if (ok || !(bad || zz || skip)) add(std::to_string(ok) + " ✓");
  if (zz) add("💤 " + std::to_string(zz));
  if (skip) add(std::to_string(skip) + " skipped");
  return out;
}

inline std::string short_sha(const std::string& ref) { return ref.substr(0, 7); }

inline std::string note(const Outcome& o) {
  if (!o.nothing.empty()) return o.nothing;
  std::string out, why = failure(o);
  if (!why.empty()) out += "**temoc failed**: " + why + "\n\n";
  if (!o.error.empty()) return trim(out);
  out += "temoc at `" + short_sha(o.ref) + "`: " + counts_text(o.counts) + "\n\n";
  std::vector<std::string> files = o.files;
  for (auto& [f, s] : o.file_status)
    if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
  for (auto& f : files) {
    auto s = o.file_status.find(f);
    out += "**" + f + "**: " + (s == o.file_status.end() ? "not run" : s->second) + "\n";
    if (auto p = o.problems.find(f); p != o.problems.end())
      for (auto& item : p->second) out += "- " + item + "\n";
    out += "\n";
  }
  for (auto& e : o.run_errors) {
    out += "**run error** " + e.kind + (e.file.empty() ? "" : " in " + e.file) + ": " + e.message + "\n";
    if (std::string t = trim(e.stderr_tail); !t.empty())
      out += "\n```\n" + (t.size() > 2000 ? "…" + t.substr(t.size() - 2000) : t) + "\n```\n";
    out += "\n";
  }
  if (!why.empty() && !trim(o.err).empty()) out += "temoc's stderr:\n\n```\n" + trim(o.err) + "\n```\n";
  return trim(out);
}

inline json marks_json(const std::string& ref, const std::vector<Mark>& ms) {
  json lines = json::array();
  for (auto& m : ms)
    lines.push_back({{"file", m.file}, {"line", m.line}, {"level", m.level}, {"glyph", m.glyph}, {"text", m.text}});
  return {{"ref", ref}, {"lines", lines}};
}

// What this plugin knows about one PR.
struct PrState {
  bool running = false;
  std::optional<Outcome> last;  // stays while a rerun goes, replaced when it ends
};

// The full `pr` line for url: a 🧪 tag, the note and the marks. Fits kMaxLine:
// when the marks are too many, the ok and skipped ones go first, then the
// rest from the end; then the note is cut.
inline std::string pr_line(const std::string& url, const PrState& st) {
  json j = {{"pr", url}};
  if (!st.running && !st.last) return j.dump();
  json t = {{"glyph", "🧪"}};
  const Outcome* o = st.last ? &*st.last : nullptr;
  std::string why = o ? failure(*o) : "";
  if (st.running) t["text"] = "temoc…", t["hint"] = "temoc: running";
  else if (!o->nothing.empty()) t["text"] = "no specs", t["hint"] = "temoc: no spec files changed";
  else if (!why.empty()) {
    std::string h = why.substr(0, why.find('\n'));
    if (h.size() > 300) {  // the hint isn't cut to fit the line below: keep it short
      size_t n = 300;
      while (n > 0 && (static_cast<unsigned char>(h[n]) & 0xC0) == 0x80) --n;
      h = h.substr(0, n) + "…";
    }
    t["text"] = "failed", t["hint"] = "temoc: " + h;
  }
  else t["text"] = tally(o->counts), t["hint"] = "temoc at " + short_sha(o->ref) + ": " + counts_text(o->counts);
  j["tags"] = json::array({t});
  auto dump = [&] { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
  if (!o) return dump();
  std::string n = note(*o);
  if (!o->marks.empty()) {
    size_t reserve = std::min<size_t>(llm::escaped_size(n), 8192) + 10;  // + ,"note":
    std::vector<Mark> ms = o->marks;
    j["marks"] = marks_json(o->ref, ms);
    if (dump().size() + reserve > kMaxLine) {
      std::erase_if(ms, [](const Mark& m) { return m.level == "ok" || m.level == "dim"; });
      j["marks"] = marks_json(o->ref, ms);
    }
    while (!ms.empty() && dump().size() + reserve > kMaxLine) {
      ms.resize(ms.size() * 9 / 10);  // ponytail: re-dumps per step, fine for a few thousand rows
      j["marks"] = marks_json(o->ref, ms);
    }
  }
  if (!n.empty()) {
    size_t rest = dump().size() + 10;
    j["note"] = fit(n, rest < kMaxLine ? kMaxLine - rest : 0);
  }
  return dump();
}

}  // namespace anton::temoc
