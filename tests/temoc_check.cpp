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

static void fold_all(Outcome& o, Results& r, const std::string& jsonl) {
  std::istringstream in(jsonl);
  for (std::string l; std::getline(in, l);) fold(o, r, json::parse(l, nullptr, false));
}

static bool has_mark(const Results& r, const std::string& file, int line, const std::string& level,
                     const std::string& glyph, const std::string& text, bool banner = false) {
  for (auto& m : marks_of(r))
    if (m.file == file && m.line == line && m.level == level && m.glyph == glyph && m.text == text && m.banner == banner)
      return true;
  return false;
}

static PrState done(const Results& r, const Outcome& o) {
  PrState st;
  st.results = r;
  st.last = o;
  return st;
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
// Made up, shaped like temoc's file hooks failing in a fresh checkout whose
// fixture server can't load its classes: every row skipped.
static const char* kHooks = R"J({"type":"file_started","file":"hooks.md","tables":[{"line":5,"class":"Calc","columns":["a","sum?"],"rows":[9,10],"snoozed_until":null}]}
{"type":"setup_failed","file":"hooks.md","line":0,"phase":"make","status":"error","actual":{"kind":"exception","message":"COULD_NOT_INVOKE_CONSTRUCTOR Hooks[0]","detail":"message:<<COULD_NOT_INVOKE_CONSTRUCTOR Hooks[0]>>\n  at load (server.js:10)\nCaused by: Error: wrapped\nCaused by: SlimError: NO_CLASS Hooks\n  at find (server.js:20)"}}
{"type":"row_finished","file":"hooks.md","table":5,"line":9,"status":"skipped","checks":[]}
{"type":"row_finished","file":"hooks.md","table":5,"line":10,"status":"skipped","checks":[]}
{"type":"table_finished","file":"hooks.md","line":5,"status":"skipped","counts":{"skipped":2}}
{"type":"file_finished","file":"hooks.md","status":"error","counts":{"skipped":2}}
{"type":"run_finished","status":"error","exit_code":1,"counts":{"skipped":2}}
)J";

int main() {
  setenv("HOME", "/home/octocat", 1);

  // Config: defaults, repos, setup, validation
  Config c;
  assert(parse_config(R"({
    // comments are fine
    "repos": {"Acme/Core": {"dir": "~/src/core", "config": "Config.toml", "env": {"X": "$HOME/x"},
                            "setup": ["sh", "-c", "pnpm install --frozen-lockfile && pnpm --filter @acme/utils build"]},
              "acme/web": {"dir": "/w", "config": "/etc/temoc.toml", "checkout": "as-is", "setup": ["~/bin/deps"]}}})", c).empty());
  assert(c.key == "T" && c.timeout == 900 && c.repos.size() == 2);
  assert(c.repos["acme/core"].dir == "/home/octocat/src/core" && c.repos["acme/core"].checkout == "pr");
  assert(c.repos["acme/core"].env["X"] == "/home/octocat/x" && c.repos["acme/web"].checkout == "as-is");
  assert(c.repos["acme/core"].setup.size() == 3 && c.repos["acme/core"].setup[2].ends_with("build"));
  assert(c.repos["acme/web"].setup == std::vector<std::string>{"/home/octocat/bin/deps"});
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
  for (const char* bad : {R"("make deps")", "[]", R"([""])", R"(["make", 1])"})
    assert(err_of(std::string(R"({"repos": {"a/b": {"dir": "/c", "config": "c", "setup": )") + bad + "}}}") ==
           "repos a/b: setup must be a command, like [\"sh\", \"-c\", \"make deps\"]");

  // Which changed files are specs, and where their tables are
  assert(is_markdown("specs/a.md") && is_markdown("README.MD") && !is_markdown("src/a.cpp"));
  assert(has_directive("# x\n\n[//]: # \"decisionTable Calculator\"\n\n| a |\n"));
  assert(has_directive("[//]: # (decisionTable Calculator#log -- snooze until 2026-12-01)\n"));
  assert(has_directive("  [//]:  #  'decisionTable X'"));
  assert(!has_directive("[//]: # \"import Fixtures\"\n"));
  assert(!has_directive("Write `[//]: # \"decisionTable X\"` before a table.\n"));  // prose, not a directive
  assert(!has_directive("    [//]: # \"decisionTable X\"\n"));  // indented 4: a code block
  assert(!has_directive("```md\n[//]: # \"decisionTable X\"\n```\n"));  // an example in a fenced block
  assert(has_directive("~~~\n```\n~~~\n[//]: # \"decisionTable X\"\n"));
  auto tables = tables_of(
      "# Calc\n\n[//]: # \"decisionTable Calc\"\n\n| a | sum? |\n| - | ---- |\n| 1 | 1    |\n| 2 | 2    |\n\n"
      "text\n[//]: # \"decisionTable Calc\"\n| a |\n|---|\n| 3 |\n```\n[//]: # \"decisionTable X\"\n| a |\n|---|\n| 4 |\n```\n");
  assert(tables.size() == 2 && tables[0].line == 3 && tables[0].rows == std::vector<int>({7, 8}));
  assert(tables[1].line == 11 && tables[1].rows == std::vector<int>({14}));

  // How a check that didn't pass reads, as temoc-ui words it
  auto d = [](const char* j) { return describe(json::parse(j)); };
  assert(d(R"({"method":"mul","status":"failed","expected":"5","actual":{"kind":"value","value":"4"}})") == "mul expected 5, got 4");
  assert(d(R"({"method":"get","expected":"a","actual":{"kind":"void"}})") == "get expected a, got no value");
  assert(d(R"({"method":"l","expected":"[a]","actual":{"kind":"list","value":[{"kind":"value","value":"a"},{"kind":"list","value":[]}]}})") ==
         "l expected [a], got [a, []]");
  assert(d(R"({"method":"div","expected":"0","actual":{"kind":"exception","message":"division by zero\nat line 1"}})") ==
         "div expected 0, got exception division by zero");
  assert(d(R"({"role":"setter","method":"setA","value":"$NOPE","actual":{"kind":"missing_symbol","symbol":"NOPE"}})") ==
         "setA got unknown symbol $NOPE");
  assert(d(R"({"role":"reset","actual":{"kind":"protocol_error","message":"bad id"}})") == "reset got protocol error bad id");
  assert(d(R"({"method":"mul","status":"snoozed","expected":"5","actual":{"kind":"value","value":"4"},"snoozed_until":"2099-12-31"})") ==
         "mul expected 5, got 4 (snoozed until 2099-12-31)");
  assert(d(R"({"method":"mul","status":"failed","expected":"5","actual":{"kind":"value","value":"4"},"snoozed_until":"2020-01-01"})") ==
         "mul expected 5, got 4");  // an expired snooze is a plain failure
  // The deepest cause of an exception, without its type
  assert(cause("boom\nCaused by: java.lang.IllegalStateException: x\nCaused by: SlimError: NO_CLASS Y\n  at z") == "NO_CLASS Y");
  assert(cause("boom, no cause") == "" && cause("Caused by: plain words") == "plain words");
  assert(with_cause("NO_CLASS Y", "Caused by: SlimError: NO_CLASS Y") == "NO_CLASS Y");  // said once

  // The recorded run: calculator.md passes, calculator_2.md has snoozed rows
  std::ifstream rec(std::string(__FILE__).substr(0, std::string(__FILE__).rfind('/')) + "/temoc_calculator.jsonl");
  std::stringstream ss;
  ss << rec.rdbuf();
  assert(rec && ss.str().size() > 1000);
  Outcome o;
  Results r;
  r.ref = "0123456789abcdef";
  o.files = {"temoc/examples/calculator.md", "temoc/examples/calculator_2.md"};
  fold_all(o, r, ss.str());
  assert(o.finished && failure(o).empty());
  assert(counts_of(r) == (std::map<std::string, int>{{"passed", 23}, {"snoozed", 4}}));
  assert(tally(counts_of(r)) == "23 ✓ 💤 4");
  const std::string c1 = "temoc/examples/calculator.md", c2f = "temoc/examples/calculator_2.md";
  assert(has_mark(r, c1, 12, "ok", "✓", "") && has_mark(r, c1, 8, "ok", "✓", "passed · 4 of 4"));
  assert(has_mark(r, c1, 0, "ok", "✓", "passed · 12 of 12"));
  assert(has_mark(r, c2f, 32, "warn", "~",
                  "exp expected 1, got exception NO_METHOD_IN_CLASS exp Calculator.Fixtures.CalculatorFixture (snoozed until 2099-12-31)"));
  assert(has_mark(r, c2f, 28, "warn", "~", "snoozed · 4 of 4") && has_mark(r, c2f, 0, "warn", "~", "snoozed · 4 of 15"));
  assert(marks_of(r).size() == 27 + 7 + 2);  // rows, tables, files
  json j = json::parse(pr_line(PR, done(r, o)));
  assert(j["tags"][0]["glyph"] == "🧪" && j["tags"][0]["text"] == "23 ✓ 💤 4");
  assert(j["tags"][0]["hint"] == "temoc at 0123456: 23 passed, 4 snoozed");
  assert(j["marks"]["ref"] == "0123456789abcdef" && j["marks"]["lines"].size() == 36);
  assert(j["marks"]["lines"][0] == json::parse(R"({"file": "temoc/examples/calculator.md", "line": 0,
                                                    "level": "ok", "glyph": "✓", "text": "passed · 12 of 12"})"));
  assert(j["note"] == "temoc at `0123456`: 23 passed, 4 snoozed\n\n"
                      "**temoc/examples/calculator.md**: passed\n\n"
                      "**temoc/examples/calculator_2.md**: snoozed");

  // A wrong value, a missing fixture: the row's note, the table's own failure, a skipped row
  o = {}, r = {};
  r.ref = "abcdef0", o.files = {"bad.md", "gone.md"};
  fold_all(o, r, kFailing);
  assert(failure(o).empty() && tally(counts_of(r)) == "1 ✗ 1 skipped");
  assert(has_mark(r, "bad.md", 6, "error", "✗", "sum expected 4, got 3"));
  assert(has_mark(r, "bad.md", 2, "error", "✗", "failed · 1 of 1"));
  assert(has_mark(r, "bad.md", 8, "error", "!", "error\nmake got exception NO_CLASS NoSuchFixture"));
  assert(has_mark(r, "bad.md", 12, "dim", "-", "") && has_mark(r, "bad.md", 0, "error", "!", "error · 1 of 2"));
  j = json::parse(pr_line(PR, done(r, o)));
  assert(j["tags"][0]["text"] == "1 ✗ 1 skipped");
  assert(j["note"] == "temoc at `abcdef0`: 1 failed, 1 skipped\n\n"
                      "**bad.md**: error\n"
                      "- `bad.md:6` sum expected 4, got 3\n"
                      "- `bad.md:8` make got exception NO_CLASS NoSuchFixture\n\n"
                      "**gone.md**: not run");

  // Running a table clears it and keeps the rest; its rows fill in live
  Results kept = r;
  begin(r, {"bad.md", 8});
  assert(!r.files["bad.md"].rows.count(12) && r.files["bad.md"].rows.count(6) && r.files["bad.md"].tables[1].failures.empty());
  Outcome live;
  fold_all(live, r, R"({"type":"file_started","file":"bad.md","tables":[{"line":8,"rows":[12]}]}
{"type":"table_started","file":"bad.md","line":8}
{"type":"row_started","file":"bad.md","table":8,"line":12})");
  assert(has_mark(r, "bad.md", 12, "dim", "⋯", "") && has_mark(r, "bad.md", 8, "dim", "⋯", "running · 0 of 1"));
  assert(has_mark(r, "bad.md", 0, "dim", "⋯", "running · 1 of 2") && has_mark(r, "bad.md", 6, "error", "✗", "sum expected 4, got 3"));
  PrState going;
  going.running = true, going.target = {"specs/bad.md", 8}, going.results = r;
  j = json::parse(pr_line(PR, going));
  assert(j["tags"][0]["text"] == "temoc bad.md:8…" && j["tags"][0]["hint"] == "temoc: running specs/bad.md:8");
  live.timed_out = true, live.timeout = 30, live.files = {"bad.md"};
  end(r, live);  // a run cut short: nothing stays running, and the file says why
  assert(!r.files["bad.md"].rows.count(12) && !r.files["bad.md"].running && !r.files["bad.md"].tables[1].running);
  assert(has_mark(r, "bad.md", 0, "error", "!", "temoc failed: timed out after 30s", true));
  // A row: just that row; a file: all of it; every file: everything
  r = kept;
  begin(r, {"bad.md", 6});
  assert(!r.files["bad.md"].rows.count(6) && r.files["bad.md"].rows.count(12));
  assert(status(r.files["bad.md"].tables[0], r.files["bad.md"]) == St::None);  // its only row: not run now
  begin(r, {"bad.md", 0});
  assert(!r.files.count("bad.md"));
  r = kept;
  begin(r, {});
  assert(r.files.empty());
  // A row run on a table the checkout's text knows: the rest of it is partial
  Results part;
  set_tables(part.files["p.md"], tables_of("[//]: # \"decisionTable C\"\n\n| a |\n|---|\n| 1 |\n| 2 |\n"));
  fold_all(live, part, R"({"type":"row_finished","file":"p.md","table":1,"line":6,"status":"passed","checks":[]})");
  assert(has_mark(part, "p.md", 1, "dim", "·", "partial · 1 of 2") && has_mark(part, "p.md", 0, "dim", "·", "partial · 1 of 2"));
  // A rerun of a different commit's checkout starts over: main.cpp clears the results when the ref moves

  // A run_error: temoc couldn't run the specs, a banner on the file
  o = {}, r = {};
  o.exit_code = 1, o.files = {"bad.md"};
  fold_all(o, r, kNoServer);
  assert(failure(o) == "server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)");
  assert(has_mark(r, "bad.md", 0, "error", "!", "temoc failed: failed to fill whole buffer (server exit status: 127)", true));
  assert(has_mark(r, "bad.md", 0, "error", "!", "error"));
  j = json::parse(pr_line(PR, done(r, o)));
  assert(j["tags"][0]["text"] == "failed");
  assert(j["tags"][0]["hint"] == "temoc: server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)");
  assert(j["marks"]["lines"][0]["banner"] == true);
  std::string n = j["note"];
  assert(n.starts_with("**temoc failed**: server_start_failed in bad.md"));
  assert(n.find("**run error** server_start_failed in bad.md: failed to fill whole buffer (server exit status: 127)\n\n"
                "```\nsh: /nonexistent/server: No such file or directory\n```") != std::string::npos);

  // A file's hooks failed: every row skipped, and the reason with its cause
  // in the hint, the note and the banner
  o = {}, r = {};
  o.files = {"hooks.md"}, o.exit_code = 1;
  fold_all(o, r, kHooks);
  const std::string why = "make got exception COULD_NOT_INVOKE_CONSTRUCTOR Hooks[0] (NO_CLASS Hooks)";
  assert(failure(o) == "hooks.md: " + why);
  assert(has_mark(r, "hooks.md", 0, "error", "!", "temoc failed: " + why, true));
  assert(has_mark(r, "hooks.md", 5, "dim", "-", "skipped · 2 of 2"));
  j = json::parse(pr_line(PR, done(r, o)));
  assert(j["tags"][0]["text"] == "failed" && j["tags"][0]["hint"] == "temoc: hooks.md: " + why);
  n = j["note"];
  assert(n.starts_with("**temoc failed**: hooks.md: " + why) && n.find("- temoc failed: " + why) != std::string::npos);
  // ...and another file of that run that passed doesn't get its banner
  o.files = {"hooks.md", "fine.md"};
  fold_all(o, r, R"({"type":"row_finished","file":"fine.md","table":1,"line":5,"status":"passed","checks":[]})");
  end(r, o);
  assert(r.files["fine.md"].banner.empty() && r.files["hooks.md"].banner.size() == 1);
  // while a rerun goes, the note no longer leads with the last run's failure
  PrState rerun = done(r, o);
  rerun.running = true;
  assert(!json::parse(pr_line(PR, rerun))["note"].get<std::string>().starts_with("**temoc failed**"));

  // The repo's setup command failed: temoc never ran
  o = {}, r = {};
  o.files = {"a.md"};
  fail_run(o, r, "setup failed (exit 1)\n\n```\nERR_PNPM_NO_LOCKFILE\n```");
  j = json::parse(pr_line(PR, done(r, o)));
  assert(j["tags"][0]["text"] == "failed" && j["tags"][0]["hint"] == "temoc: setup failed (exit 1)");
  assert(has_mark(r, "a.md", 0, "error", "!", "temoc failed: setup failed (exit 1)", true));
  assert(j["note"].get<std::string>().starts_with("**temoc failed**: setup failed (exit 1)\n\n```\nERR_PNPM_NO_LOCKFILE\n```"));
  PrState setup;
  setup.running = true, setup.phase = "setup";
  assert(json::parse(pr_line(PR, setup))["tags"][0]["text"] == "setup…");

  // While a rerun goes, the last results stay; the command shows with the marks
  o = {}, r = {};
  r.ref = "abcdef0", r.command = "temoc -c temoc.toml --output jsonl -- bad.md";
  fold_all(o, r, kFailing);
  PrState again = done(r, o);
  again.running = true;
  j = json::parse(pr_line(PR, again));
  assert(j["tags"][0]["text"] == "temoc…" && j.contains("note") && j["marks"]["command"] == r.command);
  PrState first;
  first.running = true;
  assert(pr_line(PR, first) == R"({"pr":"https://github.com/acme/core/pull/12","tags":[{"glyph":"🧪","hint":"temoc: running the PR's specs","text":"temoc…"}]})");
  assert(pr_line(PR, {}) == R"({"pr":"https://github.com/acme/core/pull/12"})");

  // Other failures of the run as a whole
  Outcome bare;
  bare.exit_code = 139, bare.err = "segfault";
  assert(failure(bare) == "temoc stopped without finishing (exit 139)");
  assert(note(bare, {}).ends_with("temoc's stderr:\n\n```\nsegfault\n```"));
  bare.timed_out = true, bare.timeout = 900;
  assert(failure(bare) == "timed out after 900s");
  Outcome usage;
  Results ur;
  fold_all(usage, ur, R"({"type":"run_finished","status":"passed","exit_code":0,"counts":{"passed":1}})");
  usage.exit_code = 2;
  assert(failure(usage) == "temoc exited with 2");
  Outcome cancelled;
  fold_all(cancelled, ur, R"({"type":"run_finished","status":"cancelled","exit_code":1,"counts":{}})");
  cancelled.exit_code = 1;
  assert(failure(cancelled) == "temoc was cancelled");
  Outcome nocfg;
  nocfg.error = "acme/core is not in ~/.config/anton/temoc.json. Add it:\n\n```json\n...\n```";
  j = json::parse(pr_line(PR, done({}, nocfg)));
  assert(j["tags"][0]["text"] == "failed" && j["tags"][0]["hint"] == "temoc: acme/core is not in ~/.config/anton/temoc.json. Add it:");
  assert(j["note"] == "**temoc failed**: " + nocfg.error && !j.contains("marks"));
  Outcome none;
  none.nothing = "No spec files changed.";
  j = json::parse(pr_line(PR, done({}, none)));
  assert(j["tags"][0]["text"] == "no specs" && j["note"] == "No spec files changed.");

  // A huge run_error message still fits the line: the hint is cut
  Outcome huge;
  Results hr;
  fold_all(huge, hr, "{\"type\":\"run_error\",\"kind\":\"protocol_error\",\"message\":\"" + std::string(70000, 'x') + "\"}\n");
  std::string hl = pr_line(PR, done(hr, huge));
  assert(hl.size() <= kMaxLine && json::parse(hl)["tags"][0]["hint"].get<std::string>().size() < 400);

  // Odd events don't throw, and a row on no line is dropped
  Outcome odd;
  Results oddr;
  fold_all(odd, oddr, "{\"type\":\"row_finished\",\"file\":\"o.md\",\"line\":\"x\",\"checks\":[1,{\"status\":\"failed\",\"actual\":3}]}\n"
                      "{\"type\":\"row_finished\",\"file\":\"o.md\",\"line\":4,\"checks\":[1,{\"status\":\"failed\",\"actual\":3}]}\n"
                      "{\"type\":\"file_started\",\"file\":\"o.md\",\"tables\":[1,{\"line\":\"x\"},{\"line\":2,\"rows\":\"y\"}]}\n"
                      "{\"type\":\"table_finished\",\"counts\":[1]}\nnot json\n[]\n");
  assert(oddr.files["o.md"].rows.size() == 1 && oddr.files["o.md"].rows[4].notes == std::vector<std::string>{""});
  assert(tally({}) == "0 ✓" && tally({{"error", 1}, {"failed", 1}, {"passed", 2}}) == "2 ✗ 2 ✓");

  // Thousands of rows still fit anton's line: ok marks go first, errors stay
  Results big;
  big.ref = "r";
  FileRes& bf = big.files["specs/big.md"];
  bf.tables.emplace_back().line = 1;
  for (int i = 2; i <= 4002; ++i) {
    bf.tables[0].rows.push_back(i);
    bf.rows[i] = {i == 4002 ? St::Failed : St::Passed, i == 4002 ? std::vector<std::string>{"sum expected 1, got 2"} : std::vector<std::string>{}};
  }
  Outcome bo;
  bo.finished = true, bo.files = {"specs/big.md"};
  std::string line = pr_line(PR, done(big, bo));
  assert(line.size() <= kMaxLine);
  j = json::parse(line);
  assert(j["marks"]["lines"].size() == 3 && j["marks"]["lines"][2]["line"] == 4002);
  assert(j["marks"]["lines"][2]["text"] == "sum expected 1, got 2");

  std::puts("temoc_check ok");
}
