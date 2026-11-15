# anton-temoc — a PR's acceptance specs, run on a key

A C++ executable built from this repo (`plugins/temoc/`) that runs the
[temoc](https://github.com/killertux/temoc) specs a PR changes and shows how
they did: a tag on the row, a note you read with `n`, and a mark on every
table row of the spec files (`marks`, `02-plugins.md`), which the reader
shows on the spec's tab the way [temoc-ui](https://github.com/killertux/temoc-ui)
does (`01-anton.md`, Reader): each row tinted by its result with its notes
under it, each table's result, and what didn't pass.

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
| `timeout` | `900` | seconds for the whole run, the `setup` command and temoc together, 1–86400 |
| `repos` | `{}` | `"owner/name"` (matched without case) → the repo's settings below |

| repo key | default | |
| --- | --- | --- |
| `dir` | required | your clone, with `$HOME`/`~` expanded |
| `config` | required | temoc's config file, relative to the checkout unless absolute. A config in the repo comes from the PR, like its code |
| `checkout` | `"pr"` | `"pr"`: a temporary worktree of the PR's head, as anton-llm makes it (`04-plugin-llm.md`, Local clones). `"as-is"`: your clone as it is |
| `env` | `{}` | extra environment for temoc, its server and `setup`, values with `$HOME`/`~` expanded |
| `setup` | none | a command run in the checkout before temoc, as an argv: no shell (`["sh", "-c", "…"]` for `&&` and pipes), `$HOME`/`~` expanded as in `env` |

**`setup`** is for what a fresh checkout lacks and the fixture server
needs: installed dependencies, built packages. A `checkout: "pr"` worktree
has neither, so a server that loads the repo's code fails before any row
runs. A pnpm monorepo whose fixtures import a workspace package that has to
be built:

```json
"acme/web": {"dir": "~/src/web", "config": "docs/specs/temoc.toml",
             "setup": ["sh", "-c", "pnpm install --frozen-lockfile --ignore-scripts && pnpm --filter @acme/utils build"]}
```

It runs with the repo's `env`, in its own process group under the run's
`timeout`, killed like temoc on a timeout (below). While it runs the tag
says `🧪 setup…`. A non-zero exit fails the run: `🧪 failed`, and the note
says `setup failed (exit N)` with the last 20 lines of its output (stdout
and stderr, interleaved) in a code block. Every run runs it again: a
`checkout: "pr"` worktree is new each time.

Checked at startup: `key` is one character, `timeout` is in range, each repo
key is `owner/name` with a `dir` and a `config`, `checkout` is `"pr"` or
`"as-is"`, `env` holds strings, `setup` is a non-empty array of strings. A
problem, or a missing file, is `health: error` with the file and the
problem, no actions, and the plugin idles, as anton-llm does.

## Runs

The plugin offers one action, `{"id": "run", "key": "T", "line": true}`
(`02-plugins.md`, actions). anton sends:

| | runs |
| --- | --- |
| `{"action": "run", "pr": url}` | every spec the PR changes (`T` on the list, or on the reader's Description) |
| `…, "file": path}` | that spec (`T` on its tab in the reader) |
| `…, "file": path, "line": n}` | the table whose directive is on line `n`, or the row on line `n` (▶ and `r` in the reader): `temoc … path:n` |

On its own thread, anton-temoc:

1. Looks up the PR's repo in `repos`. Not there: the run fails with a note
   saying how to add it.
2. Checks out the PR's head (`checkout: "pr"`): `git fetch origin
   pull/<n>/head` and a detached `git worktree add` with hooks off, removed
   with its temp directory when the run ends. A fetch that takes over 120s
   fails the run.
3. Lists the PR's changed files (`gh api --paginate
   repos/<repo>/pulls/<n>/files`, 120s at most) and keeps the `.md` ones
   that, in the checkout, are regular files of at most 4 MB (not links: a PR
   could link `x.md` to `/dev/zero`) and have a `decisionTable` directive: a
   line `[//]: # "decisionTable …"` (or `(…)`, `'…'`), indented at most three
   spaces, outside ```` ``` ```` and `~~~` fenced blocks. A removed file isn't
   in the checkout, so it drops out. With a `file`, only that one: a file
   that isn't among them fails the run. None left: tag `🧪 no specs`, a note
   saying so, and temoc doesn't run. Each one's tables (a directive, then
   the table under it) are read from the checkout, so a table that only
   partly ran is `partial`.
4. Runs `setup`, if the repo has one.
5. Runs `temoc -c <config> --output jsonl -- <files…>` (or `<file>:<line>`)
   in the checkout, with `env` added, and folds its events as they come.

**Live**: while temoc runs, the PR's line goes out again whenever its
results moved, at most four times a second: rows fill in as temoc finishes
them, the row being run is `⋯`, its table and file `⋯ running`, and the tag
says what's running (`🧪 temoc calculator.md:15…`).

**Timeout**: SIGTERM to the process group (setup's or temoc's), SIGKILL 5s
later. temoc handles SIGTERM itself: it stops its server (which runs in a
group of its own), reports the remaining rows as `skipped`, and ends with
`run_finished` `cancelled`.

**One at a time per PR.** Pressing the key again while it runs prints
`anton-temoc: temoc is already running on this PR` on stderr, which anton
flashes. Different PRs run at once, up to 64, each with its own fixture
server. Two PRs of one repo whose temoc config fixes a TCP port collide: the
second fails with `server_start_failed`; run them one after the other (port
`1`, temoc's stdin/stdout transport, has no such problem). With `checkout:
"as-is"` they also share the clone.

## Results

Kept per PR, as temoc-ui keeps them: **starting a run clears the results of
what it is about to run** (every spec, a file, a table, a row), right away,
so an old result is never taken for a new one; the rest stay. A run on
another commit (the PR moved on) drops them all. They're in the plugin's
memory: when anton restarts they're gone; press the key again.

A row, a table or a file has one of temoc-ui's results, worst last:

| result | mark | level | |
| --- | --- | --- | --- |
| passed | `✓` | `ok` | |
| skipped | `-` | `dim` | it never ran |
| snoozed | `~` | `warn` | a failure under a snooze that's still on |
| partial | `·` | `dim` | some rows have a result, none failed |
| failed | `✗` | `error` | a wrong value |
| error | `!` | `error` | an exception, an unknown symbol, a lost server |
| running | `⋯` | `dim` | |

A table's is the worst of its rows' (one without a result counts as
partial) and its own failures'; a file's, the worst of its tables', or
`error` when temoc couldn't run it at all.

**Tag**:

- **Running**: `🧪 temoc…` (`🧪 temoc x.md:12…` for part of a file);
  `🧪 setup…` while `setup` runs.
- **Done**: rows with a result, counted across the PR's specs. `🧪 14 ✓`
  when all passed; `🧪 2 ✗ 12 ✓` when not, where ✗ is `failed` plus
  `error`. Snoozed rows show as `💤 4` and don't count as failures, as in
  temoc; skipped ones as `3 skipped`. The hint is `temoc at <sha>: 2 failed,
  12 passed`.
- **Failed**: `🧪 failed`, with the reason in the hint and atop the note,
  when temoc couldn't run the specs: the steps above failed, `setup`
  failed, temoc couldn't start, it timed out, a file's hooks failed to set
  up (`setup_failed` on line 0: no row of it ran), it reported a
  `run_error`, it ended without `run_finished` (a crash), it was cancelled,
  or it exited with something other than 0 or 1.

**Messages**, as temoc-ui words them: a check that didn't pass is
`<method> expected <cell>, got <value>` (`mul expected 5, got 4`), `got no
value`, `got exception <first line>`, `got unknown symbol $X`, `got
protocol error <message>`, or without `expected` for a setter (`setA got
unknown symbol $NOPE`); a snoozed one ends in `(snoozed until 2099-12-31)`;
a lost connection is temoc's message. A setup failure is `<phase> got
exception <message>`. For an exception there, and a `run_error`, the
deepest `Caused by:` in its detail (or the server's stderr) follows in
parentheses, without its exception type, unless the message already says
it: `make got exception COULD_NOT_INVOKE_CONSTRUCTOR Hooks[0] (NO_CLASS
Hooks)` — a server that fails to construct a class usually says why only
in its cause.

**Note**, the markdown `n` shows:

```
temoc at `f9ec853`: 1 failed, 11 passed

**docs/temoc-demo/calculator.md**: failed
- `docs/temoc-demo/calculator.md:13` sum expected 5, got 4
```

Under a `**temoc failed**: …` line when the run failed. Then, per spec file,
its result (`not run` when temoc never reached it), why temoc couldn't run
it, and each failing or erroring row's messages and each table's own
failures as `file:line` and the message. Then each `run_error` with the end
of the server's stderr, and, when temoc ran and the run failed, the end of
temoc's own stderr.

**Marks**, with `ref` the commit the run used (`git rev-parse HEAD` of the
checkout) and `command` the temoc command line (the config as written in
`temoc.json`):

- a row's: its result's mark and level, and a line per check that didn't
  pass (none for passed and skipped rows);
- a table's, on its directive's line: its result and how many rows got it
  (for running and partial, how many have one), `failed · 1 of 8`, then a
  line per failure of its own: a failed `make` (`make got exception NO_CLASS
  Missing`), or an `import` (it belongs to the first table after it);
- line 0, the file's: its result, `failed · 2 of 12`; and a `banner` per
  reason temoc couldn't run it: `temoc failed: <message>` for a failed
  `setup`, a `setup_failed` of its hooks, a `run_error` about it (or about
  no file in particular), `server_start_failed` included.

The line is kept under anton's 64 KiB: when there are too many marks, the
`ok` and `dim` ones go first, then the rest from the end (rows are last);
then the note is cut, as anton-llm cuts its notes.

## Running PR code

This runs code from the PR on your machine, exactly as running its tests
would: the fixtures, the system under test and, when `config` is a file in
the repo, the server command the PR's own config names. `setup` runs in the
PR's checkout too, so a `pnpm install` runs the PR's lockfile (and its
install scripts unless `--ignore-scripts`). It happens only when you press
the key, never on a refresh. Configure a repo only if you'd run its PRs'
tests yourself, and give `config` an absolute path to a file of yours to
keep the server command out of the PR's hands (the fixtures are still the
PR's).

## Build

Part of anton's CMake project (`-DANTON_TEMOC_PLUGIN=OFF` to skip). It shares
the worktree, temp directory and watchdog code with anton-llm through
`plugins/run.hpp`, and `fit`, `tilde` and `config_path` from `llm.hpp`.

```
plugins/temoc/main.cpp   flags, config load, stdin loop, run threads,
                         checkout, changed files, setup, the temoc run,
                         the ticker that sends live results
plugins/temoc/temoc.hpp  the pure part: config parse, directives and tables,
                         results kept across runs and cleared per run, the
                         event fold, messages, tag, note, marks, the pr line
plugins/run.hpp          shared with anton-llm: {tmp}, the PR's worktree,
                         a watched run in its own process group
tests/temoc_check.cpp    asserts over temoc.hpp: config validation (setup
                         too), directives and tables, temoc-ui's wording,
                         Caused-by extraction, the fold on a recorded run of
                         temoc's calculator example (temoc_calculator.jsonl),
                         a failing row, a missing fixture, a table run that
                         keeps the rest and fills in live, partial tables,
                         a run_error, failed hooks, a failed setup, run
                         failures, and the 64 KiB fit
```
