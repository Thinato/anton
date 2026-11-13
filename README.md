# anton

The pull requests you need to look at, in your terminal, with what Slack knows about them.

![anton showing a team's code-review channel as a table of PRs](docs/image.png)

anton lists the GitHub PRs that concern you: what you opened, what you were
asked to review, what your team posted in `#code-review`. Each row shows the
size of the change, where it stands (approved, conflicting, failing checks,
queued for deploy, merged), how long it's been open, and who is reviewing it.
`⏎` opens it in the browser. anton doesn't show diffs and doesn't write
to GitHub.

Named after the critic in *Ratatouille*.

## Install

Prebuilt for macOS (Apple silicon) and Linux (x86_64):

```sh
curl --proto '=https' --tlsv1.2 -sSf \
  https://raw.githubusercontent.com/Thinato/anton/main/install.sh | sh
```

It puts `anton`, `anton-slkx` and `anton-llm` in `~/.local/bin` (override
with `ANTON_INSTALL_DIR`) and fetches the latest release (pin one with
`ANTON_VERSION=v0.1.0-beta`). After that, anton updates itself: when a release
is out it swaps in the new binaries and asks you to restart
(`"auto_update": false` in the config to only be told). anton needs [`gh`](https://cli.github.com),
logged in; the Slack plugin also needs [slkx](https://github.com/Thinato/slkx).

To build from source you need CMake 3.20+ and a C++20 compiler:

```sh
git clone https://github.com/Thinato/anton && cd anton
cmake -B build && cmake --build build
./build/anton
```

Dependencies (FTXUI, nlohmann/json) are fetched by CMake. `cmake --install
build` puts `anton`, `anton-slkx` and `anton-llm` in your prefix.

## Use

anton opens on GitHub's own pull-request tabs: Created, Review requests,
Assigned, Mentioned. Every tab is a GitHub search, so anything the search box
takes works here.

| key | |
| --- | --- |
| `j` `k` `g` `G` | move |
| `⏎` / `y` | open in browser / copy URL |
| `/` | edit the query: `is:open review-requested:@me label:bug` |
| `o` `p` `x` | scope to an org / a repo / clear it |
| `s` | sort: updated, created, size, repo |
| `1`–`9` `Tab` | switch tab |
| `r` | refresh (also automatic, every 2 min) |
| `n` | read a plugin's note on the PR, like an LLM review |
| `?` | help, including keys plugins add |

`anton --query 'repo:me/thing is:open'` opens a single ad-hoc tab.

SIZE is `+additions -deletions` in short form (`+12k -40`): green when small,
red when it'll eat your afternoon. STATE shows the one thing that matters most,
in this order: merged, conflict, a plugin's status, draft, failing checks,
changes requested, approved, your own review, waiting for review.

## Configure

`~/.config/anton/config.json`. Every key is optional.

```json
{
  "views": [
    { "name": "Code review", "plugin": "slack", "list": "code-review", "query": "recent:24h" },
    { "name": "Mine",        "query": "is:open author:@me" }
  ],
  "size": { "green_below": 450, "red_from": 1000 },
  "title_strip": "^\\[[A-Z]+-\\d+\\]\\s*",
  "people": {
    "octocat": { "name": "mona", "hex": "#a2a5d8", "ansi": 146 }
  },
  "plugins": [
    { "name": "slack",
      "command": ["anton-slkx", "--review-channel", "#code-review",
                                "--deploy-channel", "#deploy"],
      "rerun": "exit" }
  ]
}
```

- `views` replaces the default tabs. The first is where anton opens.
- `title_strip` cuts a regex out of every title, e.g. the ticket ID your team
  prefixes PRs with.
- `people` renames GitHub logins and colors them. Use the same name your Slack
  shows and the 👀 tags line up with the author column. `hex` is used on
  truecolor terminals and `ansi` (a name or 0–255) on everything else.
- `recent:24h` is anton's own filter: open PRs, plus those closed or merged in
  the last 24 hours. It's handy on a Slack tab, where merges are worth seeing.

## Plugins

A plugin is any program that prints JSON lines. This is a whole plugin:

```sh
#!/bin/sh
echo '{"pr": "https://github.com/me/thing/pull/42", "tags": [{"glyph": "🔥", "text": "hotfix"}]}'
```

```json
{ "name": "hot", "command": ["./hot.sh"], "rerun": "refresh" }
```

A plugin can put PRs on a list (which a tab can show), set a custom STATE,
add tags, attach a note you read with `n`, and register keys. Long-running plugins also get a stdin, where
anton sends the keys you pressed and your approvals. The protocol is in
[specs/02-plugins.md](specs/02-plugins.md).

### Slack

`anton-slkx` is built with anton. It reads Slack through
[slkx](https://github.com/Thinato/slkx), which needs to be on your `PATH` and
logged in.

- Every PR posted to `--review-channel` shows up on a `code-review` list,
  newest post first, the same order as the channel.
- `:eyes:` reactions become `👀 name` tags.
- `v` marks you as reviewing (adds your `:eyes:`). When you approve on GitHub,
  anton swaps it for `:+1:`.
- With `--deploy-channel`, a posted PR reads `⚙ queued` until it merges.
  `💬 Ready?` means the deploy bot is waiting on you.

It follows the channel live over Slack's websocket, so a new post shows up in
about a second. Flags and details are in
[specs/03-plugin-slkx.md](specs/03-plugin-slkx.md).

### LLM reviews

`anton-llm` is built with anton too. It runs your own agent on a PR and puts
what it prints in the PR's note. It doesn't know any model: you give it
commands, one per key, in `~/.config/anton/llm.json`.

```json
{
  "reviewers": [
    {
      "key": "R", "tag": "haiku", "stdin": "diff", "checkout": "as-is",
      "prompt": "Review the diff on stdin for {repo}#{number}. Only flag real bugs. Be terse.",
      "command": [
        "claude", "-p", "--model", "haiku", "--tools", "", "--setting-sources", "",
        "--no-session-persistence", "{prompt}"
      ]
    },
    {
      "key": "S", "tag": "deepseek", "stdin": "diff",
      "system": "You review pull requests. Be terse.",
      "prompt": "Review this diff of {repo}#{number}.",
      "command": ["llm", "-m", "deepseek-chat", "-s", "{system}", "{prompt}"]
    }
  ]
}
```

```json
{ "name": "llm", "command": ["anton-llm"] }
```

Press `R` on a PR and its tags show `🤖 haiku…` while it runs, then
`🤖 haiku`; `n` reads the review. `{name}` in a command is replaced by the
reviewer's field of that name, or by `{pr_url}` (also `$PR_URL`), `{repo}`,
`{number}`, `{note}` (the last review, so another key can post it), `{tmp}`
(a temp directory for this run) and `{repo_dir}`. `"stdin": "diff"` pipes in
`gh pr diff`. A run that fails or passes its `timeout` (600s) shows
`🤖 haiku failed` and the error in the note.

With `"repos": {"owner/name": "~/path/to/clone"}`, a PR of that repo is
reviewed in a temporary `git worktree` of its head (`"checkout": "pr"`, the
default), or in your clone as it is (`"as-is"`). The worktree is detached
and removed afterwards; your clone's branch and files aren't touched.

An agent can post its review on GitHub itself. Here Haiku reads the PR's
code, writes a review with inline comments and `suggestion` blocks, and
posts it as a comment-only review:

```json
{
  "repos": {"acme/core": "~/src/core"},
  "reviewers": [
    {
      "key": "H", "tag": "haiku", "label": "Haiku reviews on GitHub", "timeout": 900,
      "prompt": "Review {pr_url} and post your review on it.",
      "system": "You review a GitHub pull request and post the review on GitHub yourself.\n\nThe current directory is a checkout of the PR's head: read code there with Read, Grep and Glob. The only commands you can run are `gh pr view {pr_url} ...`, `gh pr diff {pr_url} ...`, `gh api repos/{repo}/pulls/{number}/files` and the post command below. Anything else is refused, so don't look for other ways.\n\n1. Get the diff (`gh pr diff {pr_url}`) and the head commit (`gh pr view {pr_url} --json headRefOid`).\n2. Read the code around the changes where you need context. Only flag real problems: bugs, security holes, missed edge cases, docs that say the wrong thing. No praise, no style nits.\n3. Write the review to {tmp}/review.json:\n   {\"commit_id\": \"<headRefOid>\", \"event\": \"COMMENT\", \"body\": \"<summary>\", \"comments\": [{\"path\": \"src/a.cpp\", \"line\": 42, \"side\": \"RIGHT\", \"body\": \"...\"}]}\n   - \"event\" is always \"COMMENT\". Never approve or request changes.\n   - \"line\" is a line of the new file inside a hunk of the diff; any other line makes GitHub reject the whole review. For several lines add \"start_line\" (same hunk) and \"start_side\": \"RIGHT\".\n   - When you have a concrete fix, end the comment with a ```suggestion block holding the exact new text of those lines, indented as in the file. Otherwise just say what is wrong.\n   - \"body\" is a summary of two or three sentences. If nothing is wrong, post no comments and say so.\n4. Post it with exactly this command: gh api --method POST repos/{repo}/pulls/{number}/reviews --input {tmp}/review.json\n   If GitHub answers 422, fix review.json and post again.\n5. End with a short markdown summary of what you posted: the review's html_url from the response, then one line per comment (path:line and the gist).",
      "settings": {
        "permissions": {
          "allow": [
            "Bash(gh pr view {pr_url} *)",
            "Bash(gh pr diff {pr_url} *)",
            "Bash(gh api repos/{repo}/pulls/{number}/files)",
            "Bash(gh api --method POST repos/{repo}/pulls/{number}/reviews --input {tmp}/review.json)",
            "Edit(/{tmp}/review.json)"
          ],
          "deny": [
            "Bash(gh pr comment *--body-file*)", "Bash(gh pr comment *-F*)",
            "Bash(gh pr comment *--edit-last*)", "Bash(gh pr comment *--delete-last*)",
            "Bash(gh api graphql *)", "Bash(gh extension *)", "Bash(gh alias *)",
            "Bash(npx *)", "Bash(docker *)"
          ],
          "blockReadsOutsideWorkingDirectories": true
        },
        "hooks": {"PreToolUse": [{"matcher": "Bash", "hooks": [
          {"type": "command", "command": "anton-llm", "args": ["--check-review"], "onFailure": "block"}
        ]}]}
      },
      "command": ["claude", "-p", "--model", "haiku",
                  "--setting-sources", "", "--settings", "{settings}", "--add-dir", "{tmp}",
                  "--strict-mcp-config", "--tools", "Read,Grep,Glob,Bash,Write", "--permission-mode", "dontAsk",
                  "--max-turns", "40", "--max-budget-usd", "1", "--no-session-persistence",
                  "--append-system-prompt", "{system}", "{prompt}"]
    }
  ]
}
```

It can't approve, request changes, merge, push or edit your files: its only
tools are the ones named, `dontAsk` denies anything not allowed, the allow
rules are exact commands for this PR, `--setting-sources ""` keeps the
PR's own `.claude/` settings (and yours) out, and `anton-llm
--check-review`, a hook, refuses any review whose `event` isn't
`"COMMENT"`. A draft-then-post pair, where you read the draft before a
second key posts it, and the details are in
[specs/04-plugin-llm.md](specs/04-plugin-llm.md).

## Develop

```sh
cmake --build build && ctest --test-dir build
```

The specs in [specs/](specs/) describe what anton does and why. Change the
spec when you change the behavior. The logic that matters (state ranking,
filters, the Slack event fold, the LLM config) is kept in pure headers,
`src/model.hpp`, `plugins/slkx/fold.hpp` and `plugins/llm/llm.hpp`, checked by the asserts in `tests/`.

Work goes through pull requests. CI builds and runs the checks on each one,
and they must pass to merge. Every merged PR publishes the next release,
`v<major.minor>.N-beta`, with macOS and Linux binaries; bump `VERSION` in
`CMakeLists.txt` to start a new major.minor.

`docs/demo/run.sh` runs anton on made-up PRs with a fake `gh` and a fake Slack
plugin. The screenshot comes from it.
