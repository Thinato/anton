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

  // {tmp} and {repo_dir}; an object field goes in as JSON, its strings filled in and escaped
  rs.clear();
  Vars repos;
  e = parse_config(R"J({
    "repos": {"Acme/Core": "~/src/core"},
    "reviewers": [{"key": "H", "checkout": "as-is", "x": "in {tmp}",
      "settings": {"permissions": {"allow": ["Edit(/{tmp}/review.json)", "Bash(gh pr view {pr_url} *)"]},
                   "n": "{note}", "x": "{x}", "k": 1},
      "command": ["claude", "--settings", "{settings}", "{repo_dir}", "{tmp}/review.json", "{x}"]}]})J",
                   rs, &repos);
  assert(e.empty() && rs[0].checkout == "as-is");
  assert(repos.size() == 1 && repos["acme/core"] == "/home/octocat/src/core");
  a = argv(rs[0], builtins(PR, "say \"hi\" {tmp}", "/private/tmp/t1", "/private/tmp/t1/pr"));
  assert(a[2] == R"J({"k":1,"n":"say \"hi\" {tmp}","permissions":{"allow":["Edit(//private/tmp/t1/review.json)",)J"
                 R"J("Bash(gh pr view https://github.com/acme/core/pull/12 *)"]},"x":"in {tmp}"})J");
  assert(a[3] == "/private/tmp/t1/pr" && a[4] == "/private/tmp/t1/review.json" && a[5] == "in /private/tmp/t1");
  assert(argv(rs[0], builtins(PR, ""))[3].empty());  // no clone: {repo_dir} is ""
  assert(err_of(R"({"reviewers": [{"key": "R", "command": ["x"]}]})").empty());
  assert(err_of(R"({"reviewers": [{"key": "R", "checkout": "head", "command": ["x"]}]})") ==
         "reviewer R: checkout must be \"pr\" or \"as-is\"");
  assert(err_of(R"({"reviewers": [{"key": "R", "tmp": "x", "command": ["x"]}]})") ==
         "reviewers[0]: \"tmp\" is a built-in, pick another name");
  assert(err_of(R"({"repos": {"core": "~/src/core"}, "reviewers": [{"key": "R", "command": ["x"]}]})") ==
         "repos: want {\"owner/name\": \"path to a clone\"}, not \"core\"");
  std::vector<Reviewer> one;
  parse_config(R"({"reviewers": [{"key": "R", "command": ["x"]}]})", one);
  assert(one[0].checkout == "pr");  // the default

  // --check-review: a PreToolUse hook's stdin -> "" (run it) or why not
  std::map<std::string, std::string> files = {
      {"/t/ok.json", R"({"commit_id": "abc", "event": "COMMENT", "body": "x", "comments": []})"},
      {"/t/approve.json", R"({"event": "APPROVE", "body": "lgtm"})"},
      {"/t/changes.json", R"({"event": "REQUEST_CHANGES"})"},
      {"/t/none.json", R"({"body": "pending until someone submits it"})"},
      {"/t/twice.json", R"({"event": "COMMENT", "event": "APPROVE"})"},
      {"/t/nested.json", R"({"body": "x", "comments": [{"event": "x"}], "event": "COMMENT"})"},
      {"/w/rel.json", R"({"event": "COMMENT"})"},
  };
  ReadFile read = [&](const std::string& p) -> std::optional<std::string> {
    auto it = files.find(p);
    return it == files.end() ? std::nullopt : std::optional(it->second);
  };
  auto hook = [&](const std::string& cmd) {
    return review_block({{"hook_event_name", "PreToolUse"}, {"tool_name", "Bash"}, {"cwd", "/w"},
                         {"tool_input", {{"command", cmd}, {"description", "x"}}}},
                        read);
  };
  const std::string post = "gh api --method POST repos/acme/core/pulls/12/reviews --input ";
  assert(hook(post + "/t/ok.json").empty());
  assert(hook(post + "rel.json").empty());  // relative to the hook's cwd
  assert(hook(post + "/t/nested.json").empty());
  assert(hook("gh api -X POST /repos/acme/core/pulls/12/reviews --input=/t/ok.json").empty());
  assert(hook(post + "/t/approve.json") == "a review's \"event\" must be \"COMMENT\" once, not \"APPROVE\"");
  assert(hook(post + "/t/changes.json") == "a review's \"event\" must be \"COMMENT\" once, not \"REQUEST_CHANGES\"");
  assert(hook(post + "/t/none.json") == "a review's \"event\" must be \"COMMENT\" once, not missing");
  assert(hook(post + "/t/twice.json") == "a review's \"event\" must be \"COMMENT\" once, not twice");
  assert(hook(post + "/t/missing.json") == "can't read /t/missing.json");
  assert(!hook("gh api repos/acme/core/pulls/12/reviews --input /t/approve.json").empty());  // --input: POST
  assert(!hook("gh api repos/acme/core/pulls/12/reviews/9/events -XPOST --input /t/approve.json").empty());
  assert(!hook("gh api repos/acme/core/pulls/12/reviews -f event=APPROVE").empty());
  assert(!hook(post + "/t/ok.json -f event=APPROVE").empty());
  assert(!hook(post + "-").empty());
  // the file can't change between the check and the call
  assert(!hook("echo '{\"event\":\"APPROVE\"}' > /t/ok.json && " + post + "/t/ok.json").empty());
  assert(!hook("sh -c '" + post + "/t/ok.json'").empty());
  assert(hook("gh pr review --approve " + PR) == "gh pr review may only comment, not approve or request changes");
  assert(!hook("gh pr review " + PR + " -a").empty());
  assert(!hook("gh pr review -r -b 'needs work' " + PR).empty());
  assert(!hook("gh pr review --request-changes=true " + PR).empty());
  assert(!hook("cd /w && bash -c \"gh pr review --approve 12\"").empty());
  assert(hook("gh pr merge 12 --squash") == "gh pr merge is not allowed");
  assert(hook("gh pr -R acme/core close 12") == "gh pr close is not allowed");
  assert(!hook("gh api graphql -f query='mutation { x }'").empty());
  assert(!hook("gh api -X PUT repos/acme/core/pulls/12/merge").empty());
  assert(!hook("gh api -X=POST repos/acme/core/pulls/12/reviews --input /t/approve.json").empty());
  assert(hook("gh api -X PATCH repos/acme/core/pulls/12 -f state=closed") == "changing or closing the PR is not allowed");
  assert(!hook("gh api repos/acme/core/issues/12 -f state=closed").empty());  // -f implies POST
  assert(hook("gh api repos/acme/core/pulls/12").empty());
  assert(hook("gh pr review --comment -b 'nice' " + PR).empty());
  assert(hook("gh pr view " + PR + " --json headRefOid,files").empty());
  assert(hook("gh api repos/acme/core/pulls/12/reviews").empty());  // GET lists them
  assert(hook("gh pr diff " + PR + " | head -50").empty());
  assert(hook("ls -la && git log --oneline -5").empty());
  assert(review_block({{"tool_name", "Read"}, {"tool_input", {{"file_path", "/x"}}}}, read).empty());
  assert(review_block(json::parse("[]"), read) == "can't parse the hook input");
  assert((shell_split("a 'b c'\"d\\\"e\" f\\ g;h|i&&j$(k)") ==
          std::vector<std::vector<std::string>>{{"a", "b cd\"e", "f g"}, {"h"}, {"i"}, {"j$"}, {"k"}}));

  std::puts("llm_check ok");
}
