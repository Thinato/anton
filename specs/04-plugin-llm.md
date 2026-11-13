# anton-llm — reviews by your own agent

A C++ executable built from this repo (`plugins/llm/`) that turns a key in
anton into a run of *your* agent on the selected PR. When the run ends, its
stdout becomes the PR's `note` (`02-plugins.md`), which you read with `n`.

It knows nothing about any model. A "reviewer" is a command: `claude -p`,
`llm -m deepseek-chat`, a script of yours. anton-llm fills in the PR, runs it
without a shell, and shows what it printed.

```json
"plugins": [{ "name": "llm", "command": ["anton-llm"] }]
```

## Config

`$XDG_CONFIG_HOME/anton/llm.json`, default `~/.config/anton/llm.json`;
`--config PATH` picks another file. Read once at startup: restart anton to
pick up a change. Comments are allowed, as in `config.json`.

```json
{
  "reviewers": [
    {
      "key": "R", "label": "review with Haiku", "tag": "haiku",
      "model": "haiku",
      "rules": "Be terse. Only flag real bugs.",
      "prompt": "Review $PR_URL. The diff is on stdin. Rules: {rules}",
      "stdin": "diff",
      "timeout": 600,
      "command": ["claude", "-p", "--model", "{model}", "{prompt}"]
    }
  ]
}
```

| key | default | |
| --- | --- | --- |
| `key` | required | one character; the key in anton. Unique. anton's own keys win a clash (`02-plugins.md`), so pick one anton doesn't use (capitals other than `G` are free) |
| `command` | required | argv, non-empty, run without a shell. Substitution below; `$HOME`, `${HOME}` and a leading `~` are expanded first, as for plugin commands |
| `tag` | the key | shown as `🤖 <tag>` in TAGS |
| `label` | `review with <tag>` | flashed when you press the key, listed in `?` |
| `stdin` | none | `"diff"`: `gh pr diff <url>` is piped to the command, cut at 200 KB with a `[diff truncated at 200 KB]` line. Without it stdin is `/dev/null` |
| `timeout` | `600` | seconds, 1–86400 |
| `cwd` | anton's cwd | working directory, with `$HOME`/`~` expanded. Not used when the PR's repo has a clone (Local clones) |
| `checkout` | `"pr"` | when the PR's repo has a clone: `"pr"` runs in a temporary worktree of the PR's head, `"as-is"` in the clone as it is |
| *anything else* | | any string or object field, usable as `{name}` |

Next to `reviewers`, an optional `"repos": {"owner/name": "~/path/to/clone"}`
names your local clones (Local clones below).

The config is checked at startup: `reviewers` is a non-empty list, each
`key` is one character and unique, `command` is a non-empty list of strings,
`stdin` is `"diff"` or absent, `checkout` is `"pr"` or `"as-is"`, every
`{name}` in `command` is known, no field is named after a built-in, and
`repos` maps `owner/name` keys to strings. Any problem → `health: error` with the file
and the problem, no actions, and the plugin idles until anton stops it (so the
message isn't repeated by restarts). A missing file is the same, with
`no ~/.config/anton/llm.json`, and nothing on stderr: a user who never set up
reviewers sees `llm ○` and nothing else.

## Substitution

Inside `command` args:

| | becomes |
| --- | --- |
| `{pr_url}`, `$PR_URL`, `${PR_URL}` | the PR URL, normalized (`https://github.com/acme/core/pull/12`) |
| `{repo}` | `acme/core` |
| `{number}` | `12` |
| `{note}` | this plugin's last successful output on that PR, `""` if none. So a second reviewer can act on the first one's draft (post it, shorten it) |
| `{tmp}` | a fresh directory for this run, absolute and real (`mkdtemp`, then `realpath`: `/private/var/folders/…` on macOS), removed with everything in it when the run ends. For files the agent writes, such as a review to post |
| `{repo_dir}` | the directory the run is in when the PR's repo has a clone (Local clones), else `""` |
| `{name}` | the reviewer's own field `name`: `{model}`, `{prompt}`, `{rules}`… |

Rules:

- **One level.** A field's text is expanded once before it goes in: built-ins
  and other fields' raw text. So `prompt` can say `$PR_URL`, `{number}` or
  `{rules}`, but a `{x}` inside `rules` stays as written. Nothing recurses, so
  no loop is possible.
- **Built-in values go in as written.** A note is agent output; a `{model}`
  in it is just text.
- **Unknown names.** An unknown `{x}` in a `command` arg is a config error
  (`command uses {modle}, which is not a field or built-in`): a typo should
  fail at startup, not reach the agent. An unknown `{x}` inside a field is
  left as written, because fields are prose for the agent and may well
  contain braces. Only `{identifier}` is a placeholder: `{}`, `{ "a": 1 }`
  and `{1x}` are plain text.
- **Object fields** go in as compact JSON, after the placeholders inside
  their strings are filled in (one level, like a string field). A value is
  filled into its JSON string, so a note with a `"` in it stays inside that
  string. This is how a reviewer writes `claude --settings` as JSON rather
  than as an escaped string: `"settings": {...}` and `"--settings",
  "{settings}"`. Other non-string fields (numbers, lists) aren't fields.
- Only `$PR_URL` is a `$` placeholder (besides `$HOME`): there's no shell.

## Local clones

```json
"repos": { "acme/core": "~/src/core" }
```

When the PR's repo is in `repos` (matched without case), the run's working
directory is that clone, and `{repo_dir}` names it. `checkout` says which
version of the code is there:

- **`"pr"`** (default): a temporary, detached worktree of the PR's head,
  `{tmp}/pr`. anton-llm runs `git -C <clone> fetch origin pull/<n>/head`,
  then `git -C <clone> worktree add --detach {tmp}/pr FETCH_HEAD`, with
  `core.hooksPath=/dev/null` so no hook runs (a `core.hooksPath` inside the
  repo, as husky sets, would otherwise run the PR's own `post-checkout`).
  After the run: `git worktree remove --force` and `git worktree prune`. No
  branch or ref is created, and your working copy and its current branch
  aren't touched. A failed fetch (no `origin`, offline, no such PR) fails the
  run: `git fetch origin pull/12/head in ~/src/core: fatal: …`.
- **`"as-is"`**: the clone itself, on whatever you have checked out.

`"pr"` is the default because a reviewer should read the code under review,
not whatever branch the clone happens to be on, and because it never touches
your files. The price is that the PR's own files, `.claude/` included, are
the working directory: see Agents that post below.

Runs on one repo can overlap. Each worktree is in its own `{tmp}`, so paths
can't collide; a fetch and the `worktree add` that reads its `FETCH_HEAD`
hold a lock per plugin, so two PRs can't swap heads. A plugin killed mid-run
leaves its `{tmp}`; the next run's `prune` drops the stale worktree entry.

With no clone for the repo, `{repo_dir}` is `""` and the run is in `cwd`.

## Runs

Pressing a reviewer's key sends `{"action": key, "pr": url}` (`02-plugins.md`);
anton-llm starts the run on its own thread and re-sends the PR's line.

- **Running**: tag `🤖 <tag>…`, hint `<label>: running`.
- **Success** (exit 0): tag `🤖 <tag>`; stdout, trimmed, becomes the note. No
  output reads `(<tag> printed nothing)`. stdout past 1 MiB is dropped and
  a stored output is cut to 60 KB, both with a `[truncated]` marker.
- **Failure** (non-zero exit, timeout, can't start, `gh pr diff` failed, `cwd`
  not a directory, the clone's fetch or worktree failed): tag `🤖 <tag> failed`, hint `<label>: exit 3` (or `timed
  out after 600s`, …). The note gets a `**<tag> failed**: …` section with the
  last 2000 bytes of stderr (stdout if stderr is empty) in a code block. The
  last successful output stays below it, so a failed run doesn't throw away a
  review.
- **Timeout**: SIGTERM to the run's process group, SIGKILL 5s later if it is
  still there.
- **One at a time per (reviewer, PR).** Pressing the key again while it runs
  prints `anton-llm: <tag> is already running on this PR` on stderr, which
  anton flashes. Different PRs and different reviewers run at once, up to 64
  runs.

**State.** Each `pr` line is the full state for the PR: one tag per reviewer
that has run on it (in config order), and a note made of the failure sections
followed by the last successful output. The line is kept under anton's 64 KiB
limit by cutting the note (at a UTF-8 boundary) and ending it with
`[truncated]`. State lives in the plugin's memory: when anton restarts, notes
are gone; run the reviewer again.

**Processes.** Each run is its own process group, so a timeout reaches
whatever the agent spawns. anton stops the plugin by signalling the plugin's
group, which doesn't include the runs; the plugin forwards SIGTERM to every
running group before it exits. It also exits, the same way, when its stdin
closes (anton is gone).

## Agents that post

A reviewer can post on GitHub itself, using `gh`, if its command lets it.
The example below (b) has Claude Code with Haiku read the PR's checkout and
post a review of comments and `suggestion` blocks. It can't approve,
request changes, merge, push or edit your files. That rests on the command's
flags, in layers:

- **Tools.** `--tools Read,Grep,Glob,Bash,Write` is the whole tool set: no
  Edit, no WebFetch, no Skill, no subagents. (On macOS and Linux, Glob and
  Grep only exist when named.)
- **Permissions**, in `--settings`. `--permission-mode dontAsk` denies
  anything not allowed, so nothing waits on a prompt. File reads in the
  working directory need no rule; `blockReadsOutsideWorkingDirectories`
  makes reads elsewhere fail, for the file tools and for `cat` and friends.
  The allow rules are exact, filled in for the PR with `{pr_url}`, `{repo}`,
  `{number}` and `{tmp}`: a rule ending in `*` would let any flag through.
  The one write is `gh api --method POST repos/{repo}/pulls/{number}/reviews
  --input {tmp}/review.json`, and Write may touch only that file:
  `Edit(/{tmp}/review.json)` (Write is governed by Edit rules; an absolute
  path starts with `//`, which the `/` before `{tmp}` makes). A compound
  command (`&&`, `;`, `|`) runs only if every part is allowed. Deny rules
  win over allow rules; the example denies `gh pr comment` with a file
  (`--body-file`, `-F`) or `--edit-last`/`--delete-last`, `gh api graphql`,
  `gh extension`, `gh alias`, `npx` and `docker`, in case a rule is widened
  later.
- **No project settings: `--setting-sources ""`.** `claude -p` never asks
  whether you trust a folder. Run in a checkout of the PR, it would load the
  PR's own `.claude/settings.json`, so the PR's author could ship hooks
  (commands that run on your machine), allow rules, an `env` block and
  `.mcp.json` servers. `--setting-sources ""` reads no settings file: not
  the project's, and not yours either, so your own hooks, plugins and broad
  allow rules don't reach the reviewer. Only `--settings` (and managed
  settings) apply. `--setting-sources user` would keep your settings and
  still drop the project's. `--bare` would also do, but needs an API key. The
  PR's `CLAUDE.md` is still read: it is the PR's text, like its code, and
  the rules above are what bound it.
- **The hook: `anton-llm --check-review`**, below, checks every Bash call
  before it runs, whatever the rules say. `"onFailure": "block"` blocks the
  call if the hook can't run (`anton-llm` not on `PATH`, say).
- **GitHub** refuses an approval or a request for changes on your own PR.
  On anyone else's it doesn't, which is what the hook is for.

`--max-turns` and `--max-budget-usd` bound a run's length and cost. The
reviewer's own text is untrusted too: a PR can tell the agent to say
anything, and it can say it in a review comment, but only on that PR.

### `--check-review`

`anton-llm --check-review` is a Claude Code `PreToolUse` command hook. It
reads the hook's JSON on stdin (`tool_name`, `tool_input.command`, `cwd`)
and exits 0 to let the call run, or 2 with the reason on stderr, which
blocks the call and shows Claude the reason. In a Bash command, split into
simple commands at `;`, `&&`, `|`, `$(…)` and the like, and looking inside
`sh -c '…'`, it blocks:

- `gh pr review` with `--approve`/`-a` or `--request-changes`/`-r`;
- `gh pr merge` and `gh pr close`;
- `gh api graphql` (it can approve and merge), and a non-GET `gh api` on
  `…/pulls/<n>/merge`;
- a review POST, `gh api` on `…/pulls/<n>/reviews` or
  `…/reviews/<id>/events` (POST is explicit, or implied by `--input` or
  `-f`), unless it is one plain command with `--input FILE`, no `-f`/`-F`
  fields, and the file holds a JSON object whose `event` is exactly
  `"COMMENT"`, once. A missing `event` would leave a pending review that a
  later call could submit as an approval. "One plain command" means no `;`,
  `&`, `|`, `<`, `>`, `(`, `` ` `` or `$` in it, so nothing can rewrite the
  file between the check and the call.

Everything else passes, for the permissions to decide. A relative `--input`
is read from the hook's `cwd`.

## Examples

**(a) Read-only.** The diff on stdin, no tools (`--tools ""`), so it can
only answer. `checkout: as-is` because it reads nothing from the clone.

```json
{
  "key": "R", "tag": "haiku", "stdin": "diff", "checkout": "as-is",
  "prompt": "Review the diff on stdin for {repo}#{number}. Only flag real bugs. Be terse.",
  "command": [
    "claude", "-p", "--model", "haiku", "--tools", "", "--setting-sources", "",
    "--no-session-persistence", "{prompt}"
  ]
}
```

**(b) Haiku reviews and posts.** It reads the checkout and the PR, writes
`{tmp}/review.json`, posts it with the one command it's allowed, and prints
a summary of what it posted, which becomes the note. The system prompt says
what a good review looks like: `commit_id` from `headRefOid`, comments on
lines inside the diff, `suggestion` blocks only for a concrete fix, `event`
always `"COMMENT"`. `"settings"` is an object field, so `{settings}` is its
JSON with the PR filled in.

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
                  "--setting-sources", "", "--settings", "{settings}", "--strict-mcp-config",
                  "--tools", "Read,Grep,Glob,Bash,Write", "--permission-mode", "dontAsk",
                  "--max-turns", "40", "--max-budget-usd", "1", "--no-session-persistence",
                  "--append-system-prompt", "{system}", "{prompt}"]
    }
  ]
}
```

**(c) A draft, then a post.** `D` drafts a review without posting anything
(no Write, no write rules); you read it with `n`. If it's worth posting, `P`
gets it as `{note}` and posts it, under the same rules as (b). Press `P`
right after `D`: `{note}` is the last successful output on the PR, whichever
reviewer wrote it.

```json
{
  "key": "D", "tag": "draft", "label": "Haiku drafts a review",
  "prompt": "Review {pr_url}. The current directory is a checkout of its head; `gh pr diff {pr_url}` shows the diff. Only flag real problems: bugs, security holes, missed edge cases. Print a draft review: a summary of two or three sentences, then one section per finding headed with `path:line` (a line of the new file inside a hunk of the diff), saying what is wrong, with a ```suggestion block holding the exact new text of those lines when the fix is concrete.",
  "settings": {
    "permissions": {
      "allow": [
        "Bash(gh pr view {pr_url} *)",
        "Bash(gh pr diff {pr_url} *)",
        "Bash(gh api repos/{repo}/pulls/{number}/files)"
      ],
      "blockReadsOutsideWorkingDirectories": true
    }
  },
  "command": ["claude", "-p", "--model", "haiku",
              "--setting-sources", "", "--settings", "{settings}", "--strict-mcp-config",
              "--tools", "Read,Grep,Glob,Bash", "--permission-mode", "dontAsk",
              "--max-turns", "40", "--max-budget-usd", "1", "--no-session-persistence", "{prompt}"]
},
{
  "key": "P", "tag": "posted", "label": "post the draft as a GitHub review", "checkout": "as-is",
  "prompt": "Post this draft review of {pr_url} on GitHub as it is, without adding findings of your own.\n\n{note}",
  "system": "You turn a draft review into a GitHub review and post it. The only commands you can run are `gh pr view {pr_url} ...`, `gh pr diff {pr_url} ...` and the post command below.\n\n1. Get the head commit (`gh pr view {pr_url} --json headRefOid`) and the diff (`gh pr diff {pr_url}`).\n2. Write {tmp}/review.json: {\"commit_id\": \"<headRefOid>\", \"event\": \"COMMENT\", \"body\": \"<the draft's summary>\", \"comments\": [{\"path\": \"...\", \"line\": 42, \"side\": \"RIGHT\", \"body\": \"<the finding, with its suggestion block>\"}]}. \"event\" is always \"COMMENT\". \"line\" must be inside a hunk of the diff; a finding you can't place goes into \"body\" instead.\n3. Post it with exactly: gh api --method POST repos/{repo}/pulls/{number}/reviews --input {tmp}/review.json\n   If GitHub answers 422, fix review.json and post again.\n4. End with a short markdown summary: the review's html_url, then one line per comment.",
  "settings": {
    "permissions": {
      "allow": [
        "Bash(gh pr view {pr_url} *)",
        "Bash(gh pr diff {pr_url} *)",
        "Bash(gh api --method POST repos/{repo}/pulls/{number}/reviews --input {tmp}/review.json)",
        "Edit(/{tmp}/review.json)"
      ],
      "blockReadsOutsideWorkingDirectories": true
    },
    "hooks": {"PreToolUse": [{"matcher": "Bash", "hooks": [
      {"type": "command", "command": "anton-llm", "args": ["--check-review"], "onFailure": "block"}
    ]}]}
  },
  "command": ["claude", "-p", "--model", "haiku",
              "--setting-sources", "", "--settings", "{settings}", "--strict-mcp-config",
              "--tools", "Bash,Write", "--permission-mode", "dontAsk",
              "--max-turns", "30", "--max-budget-usd", "1", "--no-session-persistence",
              "--append-system-prompt", "{system}", "{prompt}"]
}
```

Any other CLI, e.g. [`llm`](https://llm.datasette.io) with DeepSeek:

```json
{ "key": "S", "tag": "deepseek", "stdin": "diff",
  "system": "You review pull requests. Only flag real bugs.",
  "prompt": "Review this diff of $PR_URL.",
  "command": ["llm", "-m", "deepseek-chat", "-s", "{system}", "{prompt}"] }
```

Posting a note as a plain comment, no agent involved:

```json
{ "key": "C", "tag": "commented", "label": "post the note as a PR comment",
  "command": ["gh", "pr", "comment", "{pr_url}", "--body", "{note}"] }
```

## Build

Part of anton's CMake project (`-DANTON_LLM_PLUGIN=OFF` to skip). Reuses
`src/proc.hpp` for spawning (with a `cwd`) and `expand_home` from
`src/config.hpp`.

```
plugins/llm/main.cpp   flags, config load, stdin loop, run threads, watchdog,
                       {tmp}, the clone's worktree, --check-review's I/O
plugins/llm/llm.hpp    the pure part: config parse, substitution, pr line,
                       the --check-review verdict
tests/llm_check.cpp    asserts over llm.hpp: defaults, validation errors,
                       one-level substitution, notes that don't expand,
                       object fields, {tmp}/{repo_dir}, the pr line for
                       running/ok/failed, the 64 KiB fit, and hook inputs
                       that pass or are blocked
```
