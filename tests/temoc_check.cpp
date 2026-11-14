// Assert-based self-check for plugins/temoc/temoc.hpp.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "temoc.hpp"

using namespace anton::temoc;

static const std::string PR = "https://github.com/acme/core/pull/12";

static std::string err_of(const std::string& text) {
  Config c;
  return parse_config(text, c);
}

static Outcome fold_all(const std::string& jsonl) {
  Outcome o;
  std::istringstream in(jsonl);
  for (std::string l; std::getline(in, l);) fold(o, json::parse(l, nullptr, false));
  return o;
}

static bool has_mark(const Outcome& o, const std::string& file, int line, const std::string& level,
                     const std::string& text) {
  for (auto& m : o.marks)
    if (m.file == file && m.line == line && m.level == level && m.text == text) return true;
  return false;
}

// Recorded from temoc 0.11 on its own examples (see temoc_calculator.jsonl):
// a failing row, a fixture that doesn't exist, and a server that won't start.
static const char* kFailing = R"J({"type":"run_started","schema":1,"temoc_version":"0.11.0"}
{"type":"file_started","file":"bad.md","tables":[{"line":2,"class":"CalculatorFixture","columns":["a","b","sum?"],"rows":[6],"snoozed_until":null},{"line":8,"class":"NoSuchFixture","columns":["a","sum?"],"rows":[12],"snoozed_until":null}]}
{"type":"table_started","file":"bad.md","line":2}
{"type":"row_started","file":"bad.md","table":2,"line":6}
{"type":"row_finished","file":"bad.md","table":2,"line":6,"status":"failed","duration_ms":0,"checks":[{"role":"setter","method":"setA","column":3,"status":"passed","value":"1"},{"role":"setter","method":"setB","column":7,"status":"passed","value":"2"},{"role":"getter","method":"sum","column":11,"status":"failed","expected":"4","actual":{"kind":"value","value":"3"}}]}
{"type":"table_finished","file":"bad.md","line":2,"status":"failed","duration_ms":0,"counts":{"failed":1}}
{"type":"table_started","file":"bad.md","line":8}
{"type":"setup_failed","file":"bad.md","line":8,"phase":"make","status":"error","actual":{"kind":"exception","message":"NO_CLASS NoSuchFixture","detail":"message:<<NO_CLASS NoSuchFixture>>"}}
{"type":"row_finished","file":"bad.md","table":8,"line":12,"status":"skipped","duration_ms":0,"checks":[{"role":"setter","method":"setA","column":3,"status":"skipped","value":"1"},{"role":"getter","method":"sum","column":7,"status":"skipped","expected":"2"}]}
{"type":"table_finished","file":"bad.md","line":8,"status":"error","duration_ms":0,"counts":{"skipped":1}}
{"type":"file_finished","file":"bad.md","status":"error","duration_ms":34,"counts":{"failed":1,"skipped":1}}
{"type":"run_finished","status":"error","exit_code":1,"duration_ms":35,"counts":{"failed":1,"skipped":1}}
)J";
static const char* kNoServer = R"J({"type":"run_started","schema":1,"temoc_version":"0.11.0"}
{"type":"file_started","file":"bad.md","tables":[{"line":2,"class":"CalculatorFixture","columns":["a","b","sum?"],"rows":[6],"snoozed_until":null}]}
{"type":"row_finished","file":"bad.md","table":2,"line":6,"status":"skipped","duration_ms":0,"checks":[{"role":"getter","method":"sum","column":11,"status":"skipped","expected":"4"}]}
{"type":"table_finished","file":"bad.md","line":2,"status":"skipped","duration_ms":0,"counts":{"skipped":1}}
{"type":"file_finished","file":"bad.md","status":"error","duration_ms":17,"counts":{"skipped":1}}
{"type":"run_error","kind":"server_start_failed","message":"failed to fill whole buffer (server exit status: 127)","file":"bad.md","stderr":"sh: /nonexistent/server: No such file or directory\n"}
{"type":"run_finished","status":"error","exit_code":1,"duration_ms":18,"counts":{"skipped":1}}
)J";

int main() {
  setenv("HOME", "/home/octocat", 1);

  // Config: defaults, repos, validation
  Config c;
  assert(parse_config(R"({
    // comments are fine
    "repos": {"Acme/Core": {"dir": "~/src/core", "config": "Config.toml", "env": {"X": "$HOME/x"}},
              "acme/web": {"dir": "/w", "config": "/etc/temoc.toml", "checkout": "as-is"}}})", c).empty());
  assert(c.key == "T" && c.timeout == 900 && c.repos.size() == 2);
  assert(c.repos["acme/core"].dir == "/home/octocat/src/core" && c.repos["acme/core"].checkout == "pr");
  assert(c.repos["acme/core"].env["X"] == "/home/octocat/x" && c.repos["acme/web"].checkout == "as-is");
  Config c2;
  assert(parse_config(R"({"key": "E", "timeout": 60})", c2).empty() && c2.key == "E" && c2.timeout == 60);
  assert(err_of("[]") == "not a JSON object");
  assert(err_of(R"({"key": "TT"})") == "key must be one character");
  assert(err_of(R"({"timeout": 0})") == "timeout must be 1-86400 seconds");
  assert(err_of(R"({"repos": {"core": {"dir": "/c", "config": "c"}}})") == "repos core: want owner/name");
  assert(err_of(R"({"repos": {"a/b": {"config": "c"}}})") == "repos a/b: dir is required, the path to a clone");
  assert(err_of(R"({"repos": {"a/b": {"dir": "/c"}}})") == "repos a/b: config is required, temoc's config file");
  assert(err_of(R"({"repos": {"a/b": {"dir": "/c", "config": "c", "checkout": "head"}}})") ==
         "repos a/b: checkout must be \"pr\" or \"as-is\"");
  assert(err_of(R"({"repos": {"a/b": {"dir": "/c", "config": "c", "env": {"X": 1}}}})") ==
         "repos a/b: env must be an object of strings");

  // Which changed files are specs
  assert(is_markdown("specs/a.md") && is_markdown("README.MD") && !is_markdown("src/a.cpp"));
  assert(has_directive("# x\n\n[//]: # \"decisionTable Calculator\"\n\n| a |\n"));
  assert(has_directive("[//]: # (decisionTable Calculator#log -- snooze until 2026-12-01)\n"));
  assert(has_directive("  [//]:  #  'decisionTable X'"));
  assert(!has_directive("[//]: # \"import Fixtures\"\n"));
  assert(!has_directive("Write `[//]: # \"decisionTable X\"` before a table.\n"));  // prose, not a directive
  assert(!has_directive("    [//]: # \"decisionTable X\"\n"));  // indented 4: a code block

  // The recorded run: calculator.md passes, calculator_2.md has snoozed rows
  std::ifstream rec(std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/temoc_calculator.jsonl");
  std::stringstream ss;
  ss << rec.rdbuf();
  assert(rec && ss.str().size() > 1000);
  Outcome o = fold_all(ss.str());
  o.ref = "0123456789abcdef";
  o.files = {"temoc/examples/calculator.md", "temoc/examples/calculator_2.md"};
  assert(o.finished && failure(o).empty());
  assert(o.counts == (std::map<std::string, int>{{"passed", 23}, {"snoozed", 4}}));
  assert(tally(o.counts) == "23 ✓ 💤 4");
  const std::string c1 = "temoc/examples/calculator.md", c2f = "temoc/examples/calculator_2.md";
  assert(has_mark(o, c1, 12, "ok", "") && has_mark(o, c1, 8, "ok", "4 passed") && has_mark(o, c1, 0, "ok", "12 passed"));
  assert(has_mark(o, c2f, 32, "warn",
                  "snoozed until 2099-12-31: exp: NO_METHOD_IN_CLASS exp Calculator.Fixtures.CalculatorFixture"));
  assert(has_mark(o, c2f, 28, "warn", "4 snoozed") && has_mark(o, c2f, 0, "warn", "11 passed, 4 snoozed"));
  assert(o.marks.size() == 27 + 7 + 2);  // rows, tables, files
  json j = json::parse(pr_line(PR, {false, o}));
  assert(j["tags"][0]["glyph"] == "🧪" && j["tags"][0]["text"] == "23 ✓ 💤 4");
  assert(j["tags"][0]["hint"] == "temoc at 0123456: 23 passed, 4 snoozed");
  assert(j["marks"]["ref"] == "0123456789abcdef" && j["marks"]["lines"].size() == 36);
  assert(j["marks"]["lines"][0] == json::parse(R"({"file": "temoc/examples/calculator.md", "line": 12,
                                                    "level": "ok", "glyph": "✓", "text": ""})"));
  assert(j["note"] == "temoc at `0123456`: 23 passed, 4 snoozed\n\n"
                      "**temoc/examples/calculator.md**: passed\n\n"
                      "**temoc/examples/calculator_2.md**: snoozed");

  // A wrong value, a missing fixture: expected/got, the setup failure, a skipped row
  o = fold_all(kFailing);
  o.ref = "abcdef0", o.files = {"bad.md", "gone.md"};
  assert(failure(o).empty() && tally(o.counts) == "1 ✗ 1 skipped");
  assert(has_mark(o, "bad.md", 6, "error", "sum: expected 4, got 3"));
  assert(has_mark(o, "bad.md", 8, "error", "make failed: NO_CLASS NoSuchFixture"));
  assert(has_mark(o, "bad.md", 12, "dim", "skipped") && has_mark(o, "bad.md", 0, "error", "1 failed, 1 skipped"));
  j = json::parse(pr_line(PR, {false, o}));
  assert(j["tags"][0]["text"] == "1 ✗ 1 skipped");
  assert(j["note"] == "temoc at `abcdef0`: 1 failed, 1 skipped\n\n"
                      "**bad.md**: error\n"
                      "- `bad.md:6` sum: expected 4, got 3\n"
                      "- `bad.md:8` make failed: NO_CLASS NoSuchFixture\n\n"
                      "**gone.md**: not run");
  // While a rerun goes, the last results stay
  j = json::parse(pr_line(PR, {true, o}));
  assert(j["tags"][0]["text"] == "temoc…" && j.contains("note") && j.contains("marks"));
  assert(pr_line(PR, {true, std::nullopt}) == R"({"pr":"https://github.com/acme/core/pull/12","tags":[{"glyph":"🧪","hint":"temoc: running","text":"temoc…"}]})");
  assert(pr_line(PR, {}) == R"({"pr":"https://github.com/acme/core/pull/12"})");

  // A run_error: temoc couldn't run the specs
  o = fold_all(kNoServer);
  o.exit_code = 1, o.files = {"bad.md"};
  assert(failure(o) == "server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)");
  assert(has_mark(o, "bad.md", 0, "error", "server_start_failed: failed to fill whole buffer (server exit status: 127)"));
  j = json::parse(pr_line(PR, {false, o}));
  assert(j["tags"][0]["text"] == "failed");
  assert(j["tags"][0]["hint"] == "temoc: server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)");
  std::string n = j["note"];
  assert(n.starts_with("**temoc failed**: server_start_failed in bad.md"));
  assert(n.find("**run error** server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)\n\n"
                "```\nsh: /nonexistent/server: No such file or directory\n```") != std::string::npos);

  // Other failures of the run as a whole
  Outcome bare;
  bare.exit_code = 139, bare.err = "segfault";
  assert(failure(bare) == "temoc stopped without finishing (exit 139)");
  assert(note(bare).ends_with("temoc's stderr:\n\n```\nsegfault\n```"));
  bare.timed_out = true, bare.timeout = 900;
  assert(failure(bare) == "timed out after 900s");
  Outcome usage = fold_all(R"({"type":"run_finished","status":"passed","exit_code":0,"counts":{"passed":1}})");
  usage.exit_code = 2;
  assert(failure(usage) == "temoc exited with 2");
  Outcome cancelled = fold_all(R"({"type":"run_finished","status":"cancelled","exit_code":1,"counts":{}})");
  cancelled.exit_code = 1;
  assert(failure(cancelled) == "temoc was cancelled");
  Outcome setup;
  setup.error = "acme/core is not in ~/.config/anton/temoc.json. Add it:\n\n```json\n...\n```";
  j = json::parse(pr_line(PR, {false, setup}));
  assert(j["tags"][0]["text"] == "failed" && j["tags"][0]["hint"] == "temoc: acme/core is not in ~/.config/anton/temoc.json. Add it:");
  assert(j["note"] == "**temoc failed**: " + setup.error && !j.contains("marks"));
  Outcome none;
  none.nothing = "No spec files changed.";
  j = json::parse(pr_line(PR, {false, none}));
  assert(j["tags"][0]["text"] == "no specs" && j["note"] == "No spec files changed.");

  // Odd events don't throw
  Outcome odd = fold_all("{\"type\":\"row_finished\",\"line\":\"x\",\"checks\":[1,{\"status\":\"failed\",\"actual\":3}]}\n"
                         "{\"type\":\"table_finished\",\"counts\":[1]}\nnot json\n[]\n");
  assert(odd.marks.size() == 2 && odd.marks[0].line == 0 && odd.marks[1].text == "no rows");

  // describe: what a check that didn't pass reads as
  assert(describe(json::parse(R"({"method":"get","expected":"a","actual":{"kind":"void"}})")) == "get: expected a, got nothing");
  assert(describe(json::parse(R"({"method":"l","expected":"[a]","actual":{"kind":"list","value":[{"kind":"value","value":"a"},{"kind":"list","value":[]}]}})")) ==
         "l: expected [a], got [a, []]");
  assert(describe(json::parse(R"({"method":"x","expected":"$V","actual":{"kind":"missing_symbol","symbol":"V"}})")) == "x: no symbol $V");
  assert(describe(json::parse(R"({"role":"reset","actual":{"kind":"protocol_error","message":"bad id"}})")) == "reset: protocol error: bad id");
  assert(tally({}) == "0 ✓" && tally({{"error", 1}, {"failed", 1}, {"passed", 2}}) == "2 ✗ 2 ✓");

  // Thousands of rows still fit anton's line: ok marks go first, errors stay
  Outcome big;
  big.ref = "r", big.files = {"specs/big.md"}, big.finished = true, big.counts = {{"passed", 4000}, {"failed", 1}};
  for (int i = 1; i <= 4000; ++i) big.marks.push_back({"specs/big.md", i, "ok", "✓", ""});
  big.marks.push_back({"specs/big.md", 4001, "error", "✗", "sum: expected 1, got 2"});
  std::string line = pr_line(PR, {false, big});
  assert(line.size() <= kMaxLine);
  j = json::parse(line);
  assert(j["marks"]["lines"].size() == 1 && j["marks"]["lines"][0]["line"] == 4001);

  std::puts("temoc_check ok");
}
