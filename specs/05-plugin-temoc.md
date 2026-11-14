# anton-temoc — a PR's acceptance specs, run on a key

A C++ executable built from this repo (`plugins/temoc/`) that runs the
[temoc](https://github.com/killertux/temoc) specs a PR changes and shows how
they did: a tag on the row, a note you read with `n`, and a mark on every
table row of the spec files (`marks`, `02-plugins.md`).

temoc runs acceptance tests written as markdown tables. A table is a test
when a hidden directive comes before it, `[//]: # "decisionTable Fixture"`;
a project's temoc config (TOML) says how to start its fixture server.

```json
"plugins": [{ "name": "temoc", "command": ["anton-temoc"] }]
```

It needs a `temoc` with `--output jsonl` (0.11 has it) on `PATH`, `gh`
logged in, and a local clone of each repo.

## Config

`$XDG_CONFIG_HOME/anton/temoc.json`, default `~/.config/anton/temoc.json`;
`--config PATH` picks another file. Read once at startup. Comments are
allowed.

```json
{
  "key": "T",
  "timeout": 900,
  "repos": {
    "acme/core": {"dir": "~/src/core", "config": "Config.toml", "checkout": "pr",
                  "env": {"DATABASE_URL": "postgres://localhost/core_test"}}
  }
}
```

| key | default | |
| --- | --- | --- |
| `key` | `"T"` | one character, the key in anton |
| `timeout` | `900` | seconds for the whole temoc run, 1–86400 |
| `repos` | `{}` | `"owner/name"` (matched without case) → the repo's settings below |

| repo key | default | |
| --- | --- | --- |
| `dir` | required | your clone, with `$HOME`/`~` expanded |
| `config` | required | temoc's config file, relative to the checkout unless absolute. A config in the repo comes from the PR, like its code |
| `checkout` | `"pr"` | `"pr"`: a temporary worktree of the PR's head, as anton-llm makes it (`04-plugin-llm.md`, Local clones). `"as-is"`: your clone as it is |
| `env` | `{}` | extra environment for temoc and its server, values with `$HOME`/`~` expanded |

Checked at startup: `key` is one character, `timeout` is in range, each repo
key is `owner/name` with a `dir` and a `config`, `checkout` is `"pr"` or
`"as-is"`, `env` holds strings. A problem, or a missing file, is
`health: error` with the file and the problem, no actions, and the plugin
idles, as anton-llm does.

## Runs

Pressing the key sends `{"action": "run", "pr": url}`. On its own thread,
anton-temoc:

1. Looks up the PR's repo in `repos`. Not there: the run fails with a note
   saying how to add it.
2. Checks out the PR's head (`checkout: "pr"`): `git fetch origin
   pull/<n>/head` and a detached `git worktree add` with hooks off, removed
   with its temp directory when the run ends.
3. Lists the PR's changed files (`gh api --paginate
   repos/<repo>/pulls/<n>/files`) and keeps the `.md` ones that, in the
   checkout, have a `decisionTable` directive: a line `[//]: # "decisionTable
   …"` (or `(…)`, `'…'`), indented at most three spaces. A removed file isn't
   in the checkout, so it drops out. None left: tag `🧪 no specs`, a note
   saying so, and temoc doesn't run.
4. Runs `temoc -c <config> --output jsonl <files…>` in the checkout, with
   `env` added, and folds its events as they come.

**Timeout**: SIGTERM to temoc's process group, SIGKILL 5s later. temoc
handles SIGTERM itself: it stops its server (which runs in a group of its
own), reports the remaining rows as `skipped`, and ends with `run_finished`
`cancelled`.

**One at a time per PR.** Pressing the key again while it runs prints
`anton-temoc: temoc is already running on this PR` on stderr, which anton
flashes. Different PRs run at once, up to 64.

## Results

- **Running**: tag `🧪 temoc…`. The last run's note and marks stay until the
  new one ends.
- **Done**: rows counted from `run_finished`'s `counts`. `🧪 14 ✓` when all
  passed; `🧪 2 ✗ 12 ✓` when not, where ✗ is `failed` plus `error`. Snoozed
  rows show as `💤 4` and don't count as failures, as in temoc; skipped ones
  as `3 skipped`. The hint is `temoc at <sha>: 2 failed, 12 passed`.
- **Failed**: `🧪 failed`, with the reason in the hint and atop the note,
  when temoc couldn't run the specs: the setup above failed, temoc couldn't
  start, it timed out, it reported a `run_error`, it ended without
  `run_finished` (a crash), it was cancelled, or it exited with something
  other than 0 or 1.

**Note**, the markdown `n` shows:

```
temoc at `f9ec853`: 1 failed, 11 passed

**docs/temoc-demo/calculator.md**: failed
- `docs/temoc-demo/calculator.md:13` sum: expected 5, got 4
```

Under a `**temoc failed**: …` line when the run failed. Then, per spec file,
its status (`not run` when temoc never reached it) and each failing or
erroring row as `file:line`, the method and `expected X, got Y` (or the
exception, `no symbol $V`, …), and setup failures (`make failed: NO_CLASS
Fixture`). Then each `run_error` with the end of the server's stderr, and,
when the run failed, the end of temoc's own stderr.

**Marks**, with `ref` the commit the run used (`git rev-parse HEAD` of the
checkout):

- one per row, from `row_finished`: `ok ✓` with no text, `error ✗` with
  what didn't match, `warn 💤` with `snoozed until <date>: …`, `dim –`
  `skipped`;
- one per table, on its directive's line, from `table_finished`: its
  status and counts, `1 failed, 3 passed`; and a `setup_failed` there
  (`make failed: …`);
- line 0 for the file: its status and counts, and a `run_error` about it.

The line is kept under anton's 64 KiB: when there are too many marks, the
`ok` and `dim` ones go first, then the rest from the end; then the note is
cut, as anton-llm cuts its notes.

**State.** Results are kept per PR in the plugin's memory, and each `pr`
line carries all of them, so a rerun replaces them and nothing else does.
When anton restarts they're gone; press the key again.

## Running PR code

This runs code from the PR on your machine, exactly as running its tests
would: the fixtures, the system under test and, when `config` is a file in
the repo, the server command the PR's own config names. It happens only
when you press the key, never on a refresh. Configure a repo only if you'd
run its PRs' tests yourself, and give `config` an absolute path to a file of
yours to keep the server command out of the PR's hands (the fixtures are
still the PR's).

## Build

Part of anton's CMake project (`-DANTON_TEMOC_PLUGIN=OFF` to skip). It shares
the worktree, temp directory and watchdog code with anton-llm through
`plugins/run.hpp`, and `fit`, `tilde` and `config_path` from `llm.hpp`.

```
plugins/temoc/main.cpp   flags, config load, stdin loop, run threads,
                         checkout, changed files, the temoc run
plugins/temoc/temoc.hpp  the pure part: config parse, directive detection,
                         the event fold, tag, note, marks, the pr line
plugins/run.hpp          shared with anton-llm: {tmp}, the PR's worktree,
                         a watched run in its own process group
tests/temoc_check.cpp    asserts over temoc.hpp: config validation,
                         directives, the fold on a recorded run of temoc's
                         calculator example (temoc_calculator.jsonl), a
                         failing row, a setup failure, a run_error, run
                         failures, and the 64 KiB fit
```
