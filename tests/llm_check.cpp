// Assert-based self-check for plugins/llm/llm.hpp.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "llm.hpp"

using namespace anton::llm;

static const std::string PR = "https://github.com/acme/core/pull/12";

static std::string err_of(const std::string& text) {
  std::vector<Reviewer> rs;
  return parse_config(text, rs);
}

int main() {
  setenv("HOME", "/home/octocat", 1);

  // A full reviewer: defaults, fields, $HOME
  std::vector<Reviewer> rs;
  std::string e = parse_config(R"({
    // comments are fine
    "reviewers": [
      {"key": "R", "tag": "haiku", "model": "haiku", "rules": "Be terse.",
       "prompt": "Review $PR_URL ({repo} #{number}). Rules: {rules}. Keep {braces} and { \"json\": 1 }.",
       "stdin": "diff", "timeout": 30, "cwd": "~/src",
       "command": ["$HOME/bin/claude", "-p", "--model", "{model}", "{prompt}"]},
      {"key": "P", "command": ["post", "{note}", "${PR_URL}"]}
    ]})", rs);
  assert(e.empty());
  assert(rs.size() == 2);
  assert(rs[0].label == "review with haiku" && rs[0].timeout == 30 && rs[0].stdin_from == "diff");
  assert(rs[0].cwd == "/home/octocat/src");
  assert(rs[1].tag == "P" && rs[1].timeout == 600 && rs[1].stdin_from.empty());

  // Substitution: built-ins, fields one level deep, unknowns left alone
  auto a = argv(rs[0], builtins(PR, ""));
  assert(a[0] == "/home/octocat/bin/claude");
  assert(a[3] == "haiku");
  assert(a[4] == "Review https://github.com/acme/core/pull/12 (acme/core #12). Rules: Be terse.. "
                 "Keep {braces} and { \"json\": 1 }.");
  // A note goes in as written, even when it looks like a placeholder
  a = argv(rs[1], builtins(PR, "draft: {model} $PR_URL"));
  assert(a[1] == "draft: {model} $PR_URL" && a[2] == PR);
  // No recursion: a field naming itself, or a field inside a field's field
  Reviewer r;
  r.fields = {{"a", "x{a}y"}, {"b", "{c}"}, {"c", "{number}"}};
  assert(subst("{a}", builtins(PR, ""), r.fields) == "xx{a}yy");
  assert(subst("{b}", builtins(PR, ""), r.fields) == "{number}");
  assert(subst("{c}", builtins(PR, ""), r.fields) == "12");
  assert(subst("{", builtins(PR, ""), r.fields) == "{");
  assert(subst("{}{1x}", builtins(PR, ""), r.fields) == "{}{1x}");

  // Validation
  assert(err_of("[]") == "not a JSON object");
  assert(err_of(R"({"reviewers": []})") == "reviewers: want a non-empty list");
  assert(err_of(R"({"reviewers": [{"key": "RR", "command": ["x"]}]})") == "reviewers[0]: key must be one character");
  assert(err_of(R"({"reviewers": [{"key": "R", "command": []}]})") == "reviewer R: command must be a non-empty list");
  assert(err_of(R"({"reviewers": [{"key": "R", "command": ["x"]}, {"key": "R", "command": ["y"]}]})") ==
         "reviewer R: key used twice");
  assert(err_of(R"({"reviewers": [{"key": "R", "command": ["x", "{modle}"]}]})") ==
         "reviewer R: command uses {modle}, which is not a field or built-in");
  assert(err_of(R"({"reviewers": [{"key": "R", "note": "x", "command": ["x"]}]})") ==
         "reviewers[0]: \"note\" is a built-in, pick another name");
  assert(err_of(R"({"reviewers": [{"key": "R", "stdin": "file", "command": ["x"]}]})") ==
         "reviewer R: stdin must be \"diff\" or absent");
  assert(err_of(R"({"reviewers": [{"key": "R", "timeout": 0, "command": ["x"]}]})") ==
         "reviewer R: timeout must be 1-86400 seconds");
  // An unknown {x} inside a field is prose for the agent, not an error
  assert(err_of(R"({"reviewers": [{"key": "R", "p": "{whatever}", "command": ["x", "{p}"]}]})").empty());

  // pr_line: full state, a tag per reviewer, failures first in the note
  PrState st;
  assert(pr_line(PR, st, rs) == R"({"pr":"https://github.com/acme/core/pull/12"})");
  st.runs[0] = Run{};
  json j = json::parse(pr_line(PR, st, rs));
  assert(j["tags"].size() == 1 && j["tags"][0]["text"] == "haiku…" && !j.contains("note"));
  st.runs[0] = {Run::kOk, ""};
  st.output = "## Looks fine";
  st.runs[1] = {Run::kFailed, "exit 3\n\n```\nboom\n```"};
  j = json::parse(pr_line(PR, st, rs));
  assert(j["tags"][0]["text"] == "haiku" && j["tags"][1]["text"] == "P failed");
  assert(j["tags"][1]["hint"] == "review with P: exit 3");
  assert(j["note"] == "**P failed**: exit 3\n\n```\nboom\n```\n\n## Looks fine");

  // A huge note (control characters escape to 6 bytes) still fits the line
  st.output = std::string(200000, '\x01') + "é";
  std::string line = pr_line(PR, st, rs);
  assert(line.size() <= kMaxLine);
  std::string note = json::parse(line)["note"];
  assert(note.size() > 1000 && note.rfind("[truncated]") == note.size() - 11);
  // fit cuts at a UTF-8 boundary
  assert(fit("short", 100) == "short");
  std::string multi;
  for (int i = 0; i < 1000; ++i) multi += "é";
  std::string cut = fit(multi, 200);
  assert(json(cut).dump().size() <= 200 && cut.find("\n\n[truncated]") % 2 == 0);

  std::puts("llm_check ok");
}
