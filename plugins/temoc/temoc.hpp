#pragma once
// anton-temoc's pure core: temoc.json -> Config, which changed files are
// specs and where their tables are, temoc's JSONL events -> per-row results
// kept across runs, and the PR's plugin line (tag, note, marks). No I/O, so
// tests/temoc_check.cpp drives it.

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
  std::vector<std::string> setup;  // argv run in the checkout before temoc, if any
};

struct Config {
  std::string key = "T";
  int timeout = 900;  // seconds, for the setup command and temoc together
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
    if (auto s = v.find("setup"); s != v.end()) {
      const std::string want = where + ": setup must be a command, like [\"sh\", \"-c\", \"make deps\"]";
      if (!s->is_array() || s->empty()) return want;
      for (auto& a : *s) {
        if (!a.is_string() || (r.setup.empty() && a.get<std::string>().empty())) return want;
        r.setup.push_back(expand_home(a.get<std::string>()));
      }
    }
    c.repos[lower(k)] = std::move(r);
  }
  return "";
}

inline bool is_markdown(const std::string& path) { return lower(path).ends_with(".md"); }

// A row's, a table's or a file's result, temoc-ui's words: best to worst,
// so the worst of several is the greatest. None: no result (not run).
enum class St { None, Passed, Skipped, Snoozed, Partial, Failed, Error, Running };

inline const char* name(St s) {
  static const char* n[] = {"not run", "passed", "skipped", "snoozed", "partial", "failed", "error", "running"};
  return n[static_cast<int>(s)];
}
inline const char* glyph(St s) {
  static const char* g[] = {"", "✓", "-", "~", "·", "✗", "!", "⋯"};
  return g[static_cast<int>(s)];
}
// The mark's level (02-plugins.md): failed and error are errors, snoozed a warning.
inline const char* level(St s) {
  return s == St::Passed ? "ok" : s == St::Failed || s == St::Error ? "error" : s == St::Snoozed ? "warn" : "dim";
}
inline St parse_st(const std::string& s) {
  if (s == "passed") return St::Passed;
  if (s == "failed") return St::Failed;
  if (s == "snoozed") return St::Snoozed;
  if (s == "skipped") return St::Skipped;
  return St::Error;  // error, and anything temoc adds later
}

// A test table: its directive's line and its rows' lines, then what runs
// said about it.
struct Table {
  int line = 0;
  std::vector<int> rows;
  bool running = false, finished = false;
  std::vector<std::pair<St, std::string>> failures;  // outside its rows: a failed make
};

// The test tables of markdown text: a `[//]: # "decisionTable …"` line (or
// `(…)`, `'…'`), as temoc's parser reads it, then the table after it (blank
// lines between are fine): a header, its |---| line, then rows. Not inside a
// ``` or ~~~ fenced block, where temoc sees no directive. A directive with
// no table after it is a table with no rows.
inline std::vector<Table> tables_of(const std::string& text) {
  static const std::regex re(R"(^ {0,3}\[//\]:[ \t]*#[ \t]+["'(]decisionTable )");
  std::vector<Table> out;
  std::istringstream in(text);
  std::string fence;  // the open fence's marker
  int n = 0, state = 0;  // 1: after a directive, 2: after its header, 3: in its rows
  for (std::string l; std::getline(in, l);) {
    ++n;
    size_t i = l.find_first_not_of(' ');
    bool pipe = i != std::string::npos && i <= 3 && l[i] == '|';
    if (state == 1 && i == std::string::npos) continue;
    if ((state == 1 || state == 3) && pipe) {
      if (state == 3) out.back().rows.push_back(n);
      else state = 2;
      continue;
    }
    if (state == 2) {
      state = pipe ? 3 : 0;
      continue;
    }
    state = 0;
    if (i <= 3 && (l.compare(i, 3, "```") == 0 || l.compare(i, 3, "~~~") == 0)) {
      if (fence.empty()) fence = l.substr(i, 3);
      else if (fence == l.substr(i, 3)) fence.clear();
    } else if (fence.empty() && std::regex_search(l, re)) {
      out.emplace_back().line = n;
      state = 1;
    }
  }
  return out;
}

// True when markdown text holds a temoc test.
inline bool has_directive(const std::string& text) { return !tables_of(text).empty(); }

// A note on one line of a file in the PR's head (02-plugins.md, "marks").
struct Mark {
  std::string file;
  int line = 0;  // 0: the file as a whole
  std::string level, glyph, text;
  bool banner = false;
};

struct Row {
  St st = St::None;
  std::vector<std::string> notes;  // a line per check that didn't pass
};

// What runs said about one spec file. Kept across runs: a run replaces what
// it ran and nothing else.
struct FileRes {
  std::vector<Table> tables;  // in source order
  std::map<int, Row> rows;    // by line
  bool running = false;
  std::vector<std::string> banner;  // why temoc couldn't run the file, outside every table
};

struct Results {
  std::string ref;      // the commit they're for
  std::string command;  // the last temoc command line, as shown
  std::map<std::string, FileRes> files;
};

// What a run is asked to run: every changed spec (no file), a file (no
// line), or a table (its directive's line) or row of it.
struct Target {
  std::string file;
  int line = 0;
};

inline std::string where(const Target& t) {
  return t.file.empty() ? "" : t.file + (t.line ? ":" + std::to_string(t.line) : "");
}

struct RunError {
  std::string kind, message, file, stderr_tail;
};

// One press of the key: what was run and how the run went.
struct Outcome {
  std::string error;    // temoc couldn't be run: why (markdown, first line = the gist)
  std::string nothing;  // nothing to run: why
  std::string ref;      // the head sha the run used
  std::vector<std::string> files;  // what temoc was asked to run
  // temoc's events, folded
  bool finished = false;  // saw run_finished
  std::string status;     // run_finished's
  std::vector<std::string> setup_failures;  // a file's hooks failed: nothing in it ran
  std::vector<RunError> run_errors;
  // the process
  int exit_code = 0;
  bool timed_out = false;
  int timeout = 0;
  std::string err;  // the end of temoc's own stderr
};

// The first line of s that has anything on it, trimmed.
inline std::string first_line(const std::string& s) {
  std::istringstream in(s);
  for (std::string l; std::getline(in, l);)
    if (std::string t = trim(l); !t.empty()) return t;
  return "";
}

// The deepest `Caused by:` of an exception's detail, without the exception's
// type: `… Caused by: SlimError: NO_CLASS Hooks` -> `NO_CLASS Hooks`. "" for none.
inline std::string cause(const std::string& detail) {
  size_t at = detail.rfind("Caused by:");
  if (at == std::string::npos) return "";
  std::string c = trim(detail.substr(at + 10, detail.find('\n', at) - at - 10));
  static const std::regex type(R"(^[A-Za-z_$][\w$.]*(Error|Exception):\s*)");
  return std::regex_replace(c, type, "", std::regex_constants::format_first_only);
}

// message, then the cause in parentheses unless message already says it.
inline std::string with_cause(const std::string& message, const std::string& detail) {
  std::string c = cause(detail);
  return c.empty() || message.find(c) != std::string::npos ? message : message + " (" + c + ")";
}

// What the server answered, as temoc-ui says it: `4`, `no value`,
// `exception division by zero`, `unknown symbol $NOPE`.
inline std::string got(const json& a) {
  std::string kind = str(a, "kind");
  if (kind == "value") return first_line(str(a, "value"));
  if (kind == "void") return "no value";
  if (kind == "exception") return "exception " + with_cause(first_line(str(a, "message")), str(a, "detail"));
  if (kind == "missing_symbol") return "unknown symbol $" + str(a, "symbol");
  if (kind == "protocol_error") return "protocol error " + first_line(str(a, "message"));
  if (kind == "list" && a.contains("value") && a["value"].is_array()) {
    std::string out = "[";
    for (auto& v : a["value"]) out += (out.size() > 1 ? ", " : "") + got(v);
    return out + "]";
  }
  return "an answer anton-temoc doesn't know";
}

// A check that didn't pass, in temoc-ui's words: `mul expected 5, got 4`,
// `setA got unknown symbol $NOPE`, `… (snoozed until 2099-12-31)`.
inline std::string describe(const json& c) {
  std::string out = str(c, "method");
  if (out.empty()) out = str(c, "role");
  bool expected = c.contains("expected") && c["expected"].is_string();
  if (expected) out += " expected " + first_line(str(c, "expected"));
  if (c.contains("actual") && c["actual"].is_object()) out += (expected ? ", got " : " got ") + got(c["actual"]);
  if (str(c, "status") == "snoozed" && !str(c, "snoozed_until").empty())
    out += " (snoozed until " + str(c, "snoozed_until") + ")";
  return out;
}

inline Table* table_at(FileRes& f, int line) {
  for (auto& t : f.tables)
    if (t.line == line) return &t;
  return nullptr;
}

// A table's result, as temoc-ui works it out: the worst of its rows (one
// without a result is partial) and its own failures. None until something
// about it is known.
inline St status(const Table& t, const FileRes& f) {
  if (t.running) return St::Running;
  bool any = t.finished || !t.failures.empty();
  St worst = St::None;
  for (int r : t.rows) {
    auto it = f.rows.find(r);
    any = any || it != f.rows.end();
    worst = std::max(worst, it == f.rows.end() ? St::Partial : it->second.st);
  }
  for (auto& [s, _] : t.failures) worst = std::max(worst, s);
  return !any ? St::None : worst == St::None ? St::Passed : worst;
}

// The file's: running, an error when temoc couldn't run it, else the worst
// of its tables (one without a result is partial), not run when none has one.
inline St status(const FileRes& f) {
  if (f.running) return St::Running;
  if (!f.banner.empty()) return St::Error;
  St worst = St::None;
  bool any = false;
  for (auto& t : f.tables) {
    St s = status(t, f);
    any = any || s != St::None;
    worst = std::max(worst, s == St::None ? St::Partial : s);
  }
  return any ? worst : St::None;
}

// `failed · 1 of 8`: how many of the rows got the result (for running and
// partial, how many have one), out of all of them.
inline std::string result_text(St st, const std::vector<int>& rows, const FileRes& f) {
  int k = 0;
  for (int r : rows) {
    auto it = f.rows.find(r);
    St s = it == f.rows.end() ? St::None : it->second.st;
    bool bad = s == St::Failed || s == St::Error;
    if (st == St::Failed || st == St::Error ? bad
        : st == St::Running || st == St::Partial ? s != St::None && s != St::Running
                                                 : s == st)
      ++k;
  }
  std::string out = name(st);
  bool own = k == 0 && (st == St::Failed || st == St::Error);  // the table's own failure, not its rows'
  if (!rows.empty() && !own) out += " · " + std::to_string(k) + " of " + std::to_string(rows.size());
  return out;
}

inline std::vector<int> all_rows(const FileRes& f) {
  std::vector<int> out;
  for (auto& t : f.tables) out.insert(out.end(), t.rows.begin(), t.rows.end());
  return out;
}

// Clears what a run of t is about to replace, so an old result is never
// taken for a new one: every file, one file, or a table or row of it.
inline void begin(Results& r, const Target& t) {
  if (t.file.empty()) return r.files.clear();
  auto it = r.files.find(t.file);
  if (it == r.files.end()) return;
  if (!t.line) return void(r.files.erase(it));
  FileRes& f = it->second;
  f.banner.clear();
  for (auto& tb : f.tables)
    if (tb.line == t.line || std::find(tb.rows.begin(), tb.rows.end(), t.line) != tb.rows.end()) {
      tb.failures.clear();
      tb.finished = false;
      if (tb.line == t.line)
        for (int row : tb.rows) f.rows.erase(row);
    }
  f.rows.erase(t.line);
}

// The tables a checkout's spec text has, keeping what's known about each.
inline void set_tables(FileRes& f, std::vector<Table> tables) {
  for (auto& t : tables)
    if (Table* old = table_at(f, t.line)) {
      t.running = old->running, t.finished = old->finished, t.failures = old->failures;
    }
  f.tables = std::move(tables);
}

inline std::string failure(const Outcome& o);

// The run is over: whatever it left running wasn't finished, and when it
// failed (a timeout, a crash) each file it ran without a reason yet gets
// that one as a banner.
inline void end(Results& r, const Outcome& o) {
  for (auto& [_, f] : r.files) {
    f.running = false;
    for (auto& t : f.tables) t.running = false;
    std::erase_if(f.rows, [](const auto& kv) { return kv.second.st == St::Running; });
  }
  if (std::string why = failure(o); o.error.empty() && !why.empty())
    for (auto& file : o.files)
      if (FileRes& f = r.files[file]; f.banner.empty()) f.banner.push_back(why.substr(0, why.find('\n')));
}

// The run failed before temoc could say anything: why, and a banner on each
// file it was going to run.
inline void fail_run(Outcome& o, Results& r, const std::string& why) {
  o.error = why;
  for (auto& f : o.files) r.files[f].banner.push_back(why.substr(0, why.find('\n')));
}

// Folds one of temoc's --output jsonl events into the run and the results.
// Unknown events and fields are ignored, as temoc asks.
inline void fold(Outcome& o, Results& r, const json& ev) {
  if (!ev.is_object()) return;
  std::string type = str(ev, "type"), file = str(ev, "file");
  St st = parse_st(str(ev, "status"));
  auto num = [&](const char* k) {  // not ev.value(): a wrong type would throw out of the run's reader
    auto l = ev.find(k);
    return l != ev.end() && l->is_number_integer() ? l->get<int>() : 0;
  };
  int line = num("line");
  if (type == "run_finished") {
    o.finished = true;
    o.status = str(ev, "status");
    return;
  }
  if (type == "run_error") {
    o.run_errors.push_back({str(ev, "kind"), str(ev, "message"), file, str(ev, "stderr")});
    std::string msg = with_cause(first_line(str(ev, "message")), str(ev, "stderr"));
    if (!file.empty()) r.files[file].banner.push_back(msg);
    else
      for (auto& f : o.files) r.files[f].banner.push_back(msg);
    return;
  }
  if (file.empty() || (line <= 0 && type.starts_with("row_"))) return;  // a row on line 0 would read as the file
  FileRes& f = r.files[file];
  if (type == "file_started") {  // what it lists is about to run: clear it
    f.running = true;
    if (auto ts = ev.find("tables"); ts != ev.end() && ts->is_array())
      for (auto& tj : *ts) {
        if (!tj.is_object() || !tj.contains("line") || !tj["line"].is_number_integer()) continue;
        int at = tj["line"].get<int>();
        Table* t = table_at(f, at);
        if (!t) {  // not in the checkout's text: as temoc says
          f.tables.emplace_back().line = at;
          std::sort(f.tables.begin(), f.tables.end(), [](const Table& a, const Table& b) { return a.line < b.line; });
          t = table_at(f, at);
        }
        t->failures.clear();
        t->finished = false;
        if (auto rs = tj.find("rows"); rs != tj.end() && rs->is_array())
          for (auto& rj : *rs)
            if (rj.is_number_integer()) {
              f.rows.erase(rj.get<int>());
              if (std::find(t->rows.begin(), t->rows.end(), rj.get<int>()) == t->rows.end()) {
                t->rows.push_back(rj.get<int>());
                std::sort(t->rows.begin(), t->rows.end());
              }
            }
      }
  } else if (type == "table_started") {
    if (Table* t = table_at(f, line)) t->running = true;
  } else if (type == "row_started") {
    f.rows[line] = {St::Running, {}};
  } else if (type == "row_finished") {
    Row row{st, {}};
    if (auto cs = ev.find("checks"); cs != ev.end() && cs->is_array())
      for (auto& c : *cs)
        if (std::string s = str(c, "status"); c.is_object() && s != "passed" && s != "skipped")
          row.notes.push_back(describe(c));
    if (std::string m = first_line(str(ev, "message")); !m.empty()) row.notes.push_back(m);
    f.rows[line] = std::move(row);
  } else if (type == "table_finished") {
    if (Table* t = table_at(f, line)) t->running = false, t->finished = true;
  } else if (type == "file_finished") {
    f.running = false;
  } else if (type == "setup_failed") {
    std::string what = str(ev, "phase") + " got " + (ev.contains("actual") ? got(ev["actual"]) : "nothing");
    if (st == St::Snoozed) what += " (snoozed until " + str(ev, "snoozed_until") + ")";
    // A failed import is on its directive's line, above every table: it
    // belongs to the first table after it. Line 0 is the file's hooks.
    Table* owner = nullptr;
    for (auto& t : f.tables)
      if (line > 0 && t.line >= line) {
        owner = &t;
        break;
      }
    if (owner) {
      owner->failures.push_back({st, what});
    } else if (st != St::Snoozed) {
      f.banner.push_back(what);
      o.setup_failures.push_back(file + ": " + what);
    }
  }
}

// Why the run as a whole failed, or "": temoc couldn't run, didn't finish,
// a file's hooks failed, it reported a run_error, or exited with something
// other than 0 or 1.
inline std::string failure(const Outcome& o) {
  if (!o.error.empty()) return o.error;
  if (o.timed_out) return "timed out after " + std::to_string(o.timeout) + "s";
  if (!o.setup_failures.empty()) return o.setup_failures.front();
  if (!o.run_errors.empty()) {
    auto& e = o.run_errors.front();
    return e.kind + (e.file.empty() ? "" : " in " + e.file) + ": " + with_cause(first_line(e.message), e.stderr_tail);
  }
  if (!o.finished) return "temoc stopped without finishing (exit " + std::to_string(o.exit_code) + ")";
  if (o.status == "cancelled") return "temoc was cancelled";
  if (o.exit_code != 0 && o.exit_code != 1) return "temoc exited with " + std::to_string(o.exit_code);
  return "";
}

// Rows with a result, by status, across every file.
inline std::map<std::string, int> counts_of(const Results& r) {
  std::map<std::string, int> out;
  for (auto& [_, f] : r.files)
    for (auto& [line, row] : f.rows)
      if (row.st != St::None && row.st != St::Running) ++out[name(row.st)];
  return out;
}

// Rows by status, worst first: `1 failed, 11 passed, 4 snoozed`.
inline std::string counts_text(const std::map<std::string, int>& counts) {
  std::string out;
  for (const char* s : {"error", "failed", "passed", "snoozed", "skipped"})
    if (auto it = counts.find(s); it != counts.end() && it->second > 0)
      out += (out.empty() ? "" : ", ") + std::to_string(it->second) + " " + s;
  return out.empty() ? "no rows" : out;
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

// One mark per file (its result), per table (its result, then its own
// failures), per row (its result, then a line per check that didn't pass),
// and a banner per reason temoc couldn't run a file. Rows last: they're the
// first to go when the line gets too long.
inline std::vector<Mark> marks_of(const Results& r) {
  std::vector<Mark> out, rows;
  for (auto& [file, f] : r.files) {
    for (auto& b : f.banner) out.push_back({file, 0, "error", "!", "temoc failed: " + b, true});
    if (St s = status(f); s != St::None) out.push_back({file, 0, level(s), glyph(s), result_text(s, all_rows(f), f)});
    for (auto& t : f.tables) {
      St s = status(t, f);
      if (s == St::None) continue;
      std::string text = result_text(s, t.rows, f);
      for (auto& [_, why] : t.failures) text += "\n" + why;
      out.push_back({file, t.line, level(s), glyph(s), text});
    }
    for (auto& [line, row] : f.rows) {
      std::string text;
      for (auto& n : row.notes) text += (text.empty() ? "" : "\n") + n;
      rows.push_back({file, line, level(row.st), glyph(row.st), text});
    }
  }
  out.insert(out.end(), rows.begin(), rows.end());
  return out;
}

inline std::string note(const Outcome& o, const Results& r) {
  if (!o.nothing.empty()) return o.nothing;
  std::string out, why = failure(o);
  if (!why.empty()) out += "**temoc failed**: " + why + "\n\n";
  if (!o.error.empty() && r.files.empty()) return trim(out);
  out += "temoc at `" + short_sha(r.ref) + "`: " + counts_text(counts_of(r)) + "\n\n";
  std::vector<std::string> files = o.files;
  for (auto& [f, _] : r.files)
    if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
  for (auto& file : files) {
    auto it = r.files.find(file);
    out += "**" + file + "**: " + (it == r.files.end() ? "not run" : name(status(it->second))) + "\n";
    if (it != r.files.end()) {
      const FileRes& f = it->second;
      for (auto& b : f.banner)
        if (b != why.substr(0, why.find('\n'))) out += "- temoc failed: " + b + "\n";  // said once, on top
      std::map<int, std::vector<std::string>> items;  // by line: rows and tables, in source order
      for (auto& t : f.tables)
        for (auto& [_, w] : t.failures) items[t.line].push_back(w);
      for (auto& [line, row] : f.rows)
        if (row.st == St::Failed || row.st == St::Error)
          for (auto& n : row.notes) items[line].push_back(n);
      for (auto& [line, ws] : items)
        for (auto& w : ws) out += "- `" + file + ":" + std::to_string(line) + "` " + w + "\n";
    }
    out += "\n";
  }
  for (auto& e : o.run_errors) {
    out += "**run error** " + e.kind + (e.file.empty() ? "" : " in " + e.file) + ": " + e.message + "\n";
    if (std::string t = trim(e.stderr_tail); !t.empty())
      out += "\n```\n" + (t.size() > 2000 ? "…" + t.substr(t.size() - 2000) : t) + "\n```\n";
    out += "\n";
  }
  if (o.error.empty() && !why.empty() && !trim(o.err).empty()) out += "temoc's stderr:\n\n```\n" + trim(o.err) + "\n```\n";
  return trim(out);
}

inline json marks_json(const Results& r, const std::vector<Mark>& ms) {
  json lines = json::array();
  for (auto& m : ms) {
    json l = {{"file", m.file}, {"line", m.line}, {"level", m.level}, {"glyph", m.glyph}, {"text", m.text}};
    if (m.banner) l["banner"] = true;
    lines.push_back(l);
  }
  json j = {{"ref", r.ref}, {"lines", lines}};
  if (!r.command.empty()) j["command"] = r.command;
  return j;
}

// What this plugin knows about one PR.
struct PrState {
  bool running = false;
  std::string phase;  // while running: "setup", or "" for checkout and temoc
  Target target;      // while running: what
  Results results;
  std::optional<Outcome> last;  // the last run that ended
  bool dirty = false;           // results moved since the last line went out
};

// The full `pr` line for url: a 🧪 tag, the note and the marks. Fits kMaxLine:
// when the marks are too many, the ok and dim ones go first, then the rest
// from the end; then the note is cut.
inline std::string pr_line(const std::string& url, const PrState& st) {
  json j = {{"pr", url}};
  if (!st.running && !st.last) return j.dump();
  json t = {{"glyph", "🧪"}};
  const Outcome* o = st.last ? &*st.last : nullptr;
  std::string why = o ? failure(*o) : "";
  auto counts = counts_of(st.results);
  if (st.running && st.phase == "setup") {
    t["text"] = "setup…", t["hint"] = "temoc: running the repo's setup command";
  } else if (st.running) {
    std::string w = where(st.target);
    t["text"] = w.empty() ? "temoc…" : "temoc " + w.substr(w.rfind('/') + 1) + "…";
    t["hint"] = "temoc: running " + (w.empty() ? "the PR's specs" : w);
  } else if (!o->nothing.empty()) {
    t["text"] = "no specs", t["hint"] = "temoc: no spec files changed";
  } else if (!why.empty()) {
    std::string h = why.substr(0, why.find('\n'));
    if (h.size() > 300) {  // the hint isn't cut to fit the line below: keep it short
      size_t n = 300;
      while (n > 0 && (static_cast<unsigned char>(h[n]) & 0xC0) == 0x80) --n;
      h = h.substr(0, n) + "…";
    }
    t["text"] = "failed", t["hint"] = "temoc: " + h;
  } else {
    t["text"] = tally(counts), t["hint"] = "temoc at " + short_sha(st.results.ref) + ": " + counts_text(counts);
  }
  j["tags"] = json::array({t});
  auto dump = [&] { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
  std::string n = o ? note(*o, st.results) : "";
  std::vector<Mark> ms = marks_of(st.results);
  if (!ms.empty() || !st.results.command.empty()) {
    size_t reserve = std::min<size_t>(llm::escaped_size(n), 8192) + 10;  // + ,"note":
    j["marks"] = marks_json(st.results, ms);
    if (dump().size() + reserve > kMaxLine) {
      std::erase_if(ms, [](const Mark& m) { return m.level == "ok" || m.level == "dim"; });
      j["marks"] = marks_json(st.results, ms);
    }
    while (!ms.empty() && dump().size() + reserve > kMaxLine) {
      ms.resize(ms.size() * 9 / 10);  // ponytail: re-dumps per step, fine for a few thousand rows
      j["marks"] = marks_json(st.results, ms);
    }
  }
  if (!n.empty()) {
    size_t rest = dump().size() + 10;
    j["note"] = fit(n, rest < kMaxLine ? kMaxLine - rest : 0);
  }
  return dump();
}

}  // namespace anton::temoc
