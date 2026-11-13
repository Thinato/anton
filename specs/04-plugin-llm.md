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
| `cwd` | anton's cwd | working directory, with `$HOME`/`~` expanded |
| *anything else* | | any string field, usable as `{name}` |

The config is checked at startup: `reviewers` is a non-empty list, each
`key` is one character and unique, `command` is a non-empty list of strings,
`stdin` is `"diff"` or absent, every `{name}` in `command` is known, and no
field is named after a built-in. Any problem → `health: error` with the file
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
| `{name}` | the reviewer's own string field `name`: `{model}`, `{prompt}`, `{rules}`… |

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
- Only `$PR_URL` is a `$` placeholder (besides `$HOME`): there's no shell.

## Runs

Pressing a reviewer's key sends `{"action": key, "pr": url}` (`02-plugins.md`);
anton-llm starts the run on its own thread and re-sends the PR's line.

- **Running**: tag `🤖 <tag>…`, hint `<label>: running`.
- **Success** (exit 0): tag `🤖 <tag>`; stdout, trimmed, becomes the note. No
  output reads `(<tag> printed nothing)`. stdout past 1 MiB is dropped and
  a stored output is cut to 60 KB, both with a `[truncated]` marker.
- **Failure** (non-zero exit, timeout, can't start, `gh pr diff` failed, `cwd`
  not a directory): tag `🤖 <tag> failed`, hint `<label>: exit 3` (or `timed
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

## Examples

Claude Code, with the diff on stdin:

```json
{ "key": "R", "tag": "haiku", "model": "haiku", "stdin": "diff",
  "prompt": "Review the diff on stdin for {repo}#{number}. Only flag real bugs.",
  "command": ["claude", "-p", "--model", "{model}", "{prompt}"] }
```

Without the diff, letting the agent fetch what it needs (it runs `gh` itself):

```json
{ "key": "C", "tag": "sonnet", "timeout": 900, "cwd": "~/src/core",
  "command": ["claude", "-p", "--model", "sonnet", "Review $PR_URL with gh. Be terse."] }
```

Any other CLI, e.g. [`llm`](https://llm.datasette.io) with DeepSeek:

```json
{ "key": "D", "tag": "deepseek", "stdin": "diff",
  "system": "You review pull requests. Only flag real bugs.",
  "prompt": "Review this diff of $PR_URL.",
  "command": ["llm", "-m", "deepseek-chat", "-s", "{system}", "{prompt}"] }
```

Posting the draft another reviewer wrote:

```json
{ "key": "P", "tag": "posted", "label": "post the note as a PR comment",
  "command": ["gh", "pr", "comment", "{pr_url}", "--body", "{note}"] }
```

## Build

Part of anton's CMake project (`-DANTON_LLM_PLUGIN=OFF` to skip). Reuses
`src/proc.hpp` for spawning (with a `cwd`) and `expand_home` from
`src/config.hpp`.

```
plugins/llm/main.cpp   flags, config load, stdin loop, run threads, watchdog
plugins/llm/llm.hpp    the pure part: config parse, substitution, pr line
tests/llm_check.cpp    asserts over llm.hpp: defaults, validation errors,
                       one-level substitution, notes that don't expand,
                       the pr line for running/ok/failed, the 64 KiB fit
```
