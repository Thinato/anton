# anton plugins

A plugin is **any executable that prints JSON lines to stdout**. anton starts
it, reads its lines, and uses what it says about PRs. That is the whole
contract — no SDK, no library, no socket, any language.

A plugin can say four things about a PR:

| | what it does in anton | example |
| --- | --- | --- |
| **lists** | puts the PR on a named list; a *list view* shows that list as a tab | "posted to `#code-review`" |
| **status** | a custom value for the STATE column | `⚙ queued` from `#deploy-gogogo` |
| **tags** | small annotations in the TAGS column | `👀 alice` |
| **note** | longer text you open with `n` | an LLM's review |

All of them are optional, and all of them are opt-in for the user: nothing a
plugin says is visible unless the plugin is configured, and lists only show if
a view points at them. People without a Slack, or without a deploy queue, never
see any of it.

One mechanism covers both shapes of integration:

- **Streaming** (Slack): a long-running process that wraps a live feed, e.g.
  `slkx watch`, and prints a line whenever something changes.
- **One-shot** (Claude-Code-hook style): a script that prints its lines and
  exits. anton reruns it on every refresh if `"rerun": "refresh"`.

Why a process and not a JSON file anton polls: a file needs a writer that is
already running, a location convention, locking, and a polling interval — and
the writer is still a process. stdout gives ordering, liveness (the pipe
closes when the plugin dies), and zero setup. A plugin that *wants* to work
from a file can be `["tail", "-F", "/path/feed.jsonl"]`.

## Configuration

In `~/.config/anton/config.json`:

```json
"plugins": [
  { "name": "slack",  "command": ["anton-slkx", "--review-channel", "#code-review"] },
  { "name": "ci-bot", "command": ["./ci-tags.sh"], "rerun": "refresh" }
]
```

| key | default | |
| --- | --- | --- |
| `name` | required | referenced by list views; shown in errors and in Settings › General › Plugins |
| `command` | required | argv, run without a shell, cwd = anton's cwd. `$HOME`, `${HOME}` and a leading `~` are expanded (only these: there's no shell), so `"$HOME/.local/bin/anton-slkx"` works in a config shared between people |
| `rerun` | `"exit"` | `"exit"`: restart 5s after it exits (doubling to 5 min on repeated quick exits). `"refresh"`: run once per anton refresh. `"never"`: run once. |
| `env` | `{}` | extra environment variables |

## Protocol: plugin → anton (stdout)

One JSON object per line, UTF-8, `\n`-terminated. Lines that fail to parse are
ignored and counted (shown in Settings). stderr is not protocol: each line
flashes in the message line for 10s (warnings like a bad setting), and the
last line is shown in Settings › General › Plugins for debugging.

### `pr` — everything this plugin says about one PR

```json
{"pr": "https://github.com/brick-so/core/pull/20387",
 "lists": ["code-review"],
 "status": {"text": "queued", "glyph": "⚙"},
 "tags": [{"text": "alice", "glyph": "👀", "hint": "reviewing since 14:02"}]}
```

| field | |
| --- | --- |
| `pr` | PR URL. Normalized: scheme/host lowercased, trailing `/`, `/files`, `#…`, `?…` stripped. `owner/repo#123` also accepted. |
| `lists` | names of this plugin's lists the PR is on. |
| `order` | optional number; a list view shows higher first by default (e.g. a post time, so the newest is on top). |
| `watch` | optional `true`: anton re-fetches this PR every `watch_seconds` (15) while it's open, e.g. one you're reviewing. Keep it to a handful. |
| `status` | custom STATE value: `text` (≤ 10 chars shown), optional `glyph`. Ranking against GitHub's states is in `01-anton.md`. |
| `tags` | annotations: `text`, optional `glyph`, optional `hint` (shown in the detail line). |
| `note` | optional longer text, e.g. an LLM's review of the PR. `n` opens it in a scrollable overlay; the detail line says `n read note` when there is one. It's rendered as markdown, the same way as a PR's description in the reader (`01-anton.md`). Like everything else it must fit the 64 KiB line. |
| `marks` | optional notes on lines of files in the PR's head, below. The reader shows them on the file's tab. |

`marks` lets any plugin annotate lines of the PR's files: a test result on
the row of a spec table, a linter's finding on a line of code.

```json
"marks": {"ref": "f9ec85333a272433d5440a4567602e3fd0e1fd2d",
          "command": "temoc -c temoc.toml --output jsonl -- specs/a.md", "lines": [
  {"file": "specs/a.md", "line": 0,  "level": "error", "glyph": "!", "text": "temoc failed: server_died", "banner": true},
  {"file": "specs/a.md", "line": 0,  "level": "error", "glyph": "✗", "text": "failed · 1 of 12"},
  {"file": "specs/a.md", "line": 8,  "level": "error", "glyph": "✗", "text": "failed · 1 of 4\nmake got exception NO_CLASS X"},
  {"file": "specs/a.md", "line": 12, "level": "error", "glyph": "✗", "text": "mul expected 5, got 4\nsum expected 1, got 2"},
  {"file": "specs/a.md", "line": 13, "level": "warn",  "glyph": "~", "text": "mul expected 5, got 4 (snoozed until 2026-12-01)"},
  {"file": "specs/a.md", "line": 14, "level": "ok",    "glyph": "✓", "text": ""}]}
```

| field | |
| --- | --- |
| `ref` | the commit the marks are about, normally the PR's head when the plugin looked. A viewer of another commit knows they may be off. |
| `command` | optional: the command that made them, as the plugin wants it shown (the reader's `LAST COMMAND`). |
| `summary` | optional `{"level", "glyph", "text", "banner"}`: the PR as a whole, e.g. a run's state (`⋯ setting up · 0:23`, `✓ passed · 15 of 15`, `! temoc failed: …`). Atop the reader's Description, and atop a file's tab while the plugin has no marks on any file of the PR (a run still checking out, a config error). |
| `file` | path in the repo, as in the PR's diff. |
| `line` | 1-based line of `file` at `ref`; `0` is the file as a whole. A line can have several marks. |
| `level` | `ok`, `error`, `warn` or `dim` (didn't run, running, not relevant): how to color it. Anything else reads as `dim`. |
| `glyph`, `text` | what to show next to the line; both may be empty. `text` may hold several lines (`\n`), each shown on its own row. |
| `banner` | optional `true`, on line 0 only: the mark is a banner across the top of the file, for something that kept the plugin from looking at the file at all (temoc couldn't run it). |

A mark without a `file`, or whose `line` isn't a whole number from 0 up,
is skipped; the rest of the line still counts.

The reader (`01-anton.md`) shows a markdown file's marks on its tab, from
every plugin: the glyph in a gutter left of the line, colored by level (the
worst level when a line has several). Where the text goes depends on the
line:

- **a table row**: the row is tinted by its worst mark (`ok` green, `error`
  red, `warn` yellow, `dim` not at all), and an `error` or `warn` mark's
  text goes under the row, inside the table, a line each, indented.
- **a temoc directive** (its badge): the mark is its table's result, a
  footer inside the table's border: the glyph and the text's first line
  (`✗ failed · 1 of 4`), then the text's other lines under it (failures
  that belong to the table, not a row). Up to ` · ` the first line is the
  short form the outline shows (`failed`).
- **line 0**: the file's state, at the top: a line across the page, bold,
  tinted by level (`ok` green, `error` red, `warn` yellow, `dim` not at
  all; a `banner` always red). The worst non-banner one is the file's
  result in the outline. A plugin that runs something says on line 0 what
  it is doing, and re-sends it as it goes (anton-temoc: every second, with
  a clock), so a long step never looks idle.
- **anything else**: an `error` or `warn` mark's text under the line, as
  `✗ line 13 · text`.

anton adds its own line-0 states: a red banner on every tab (the
Description too) for each plugin whose `health` isn't ok or that exited
(`temoc: ~/.config/anton/temoc.json: line 13, column 7: …`, `temoc exited
(exit code 127): <its last stderr line>`), since a broken plugin can't name
a file; with it, a `🧪 error` tag in the reader (the plugin's last tag
glyph, `⚠ <name> error` before it ever tagged). And on a spec file nothing
has a line-0 mark on: `· not run · T to run`, `T` the first action that
takes a line.

`ok` and `dim` marks on rows are only their glyph and tint. A mark on a
line that shows nothing (a blank line, the middle of a paragraph) goes on
the nearest shown line above it. When `ref` isn't the head the tab shows,
the tab says the results are for that commit. The tab's name counts the
marks on rows by glyph: `✓ 10 ✗ 1 ~ 1`.

**State, not deltas.** Every `pr` line is the *complete* truth for
(plugin, PR) and replaces the previous one; an omitted field means "none"
(`{"pr": url}` alone clears everything that plugin said about it). A plugin
never has to remember what it told anton before, and a lost or duplicated
line self-heals on the next one.

When a plugin's state is replaced depends on `rerun`:
- `"exit"` (long-running): lines apply as they arrive. When the process
  exits, anton drops all of its state — on restart it re-sends what is still
  true.
- `"refresh"` / `"never"` (one-shot): a run's lines are collected and replace
  the plugin's whole state when the run ends, so tags don't blink out between
  runs and a PR the script stopped mentioning disappears.

anton starts each plugin as the leader of its own process group and stops it
with a signal to the group, so whatever a plugin spawns (as long as it stays
in the group) stops with it.

PRs a plugin mentions that are not in the current rows are kept, and show up
if a later refresh brings them in. PRs on a list are fetched from GitHub by
anton (see "Second path" in `01-anton.md`); a plugin only has to know URLs.

### `health` — plugin liveness (optional)

```json
{"health": "ok"}
{"health": "error", "text": "slack session expired"}
```

The latest `health` per plugin is shown in the footer as `slack ●` (ok),
`slack ○` (anything else), with `text` in Settings › General › Plugins,
which also has a Restart button: it stops the plugin and runs it again now.

### Anything else

Objects with neither `pr` nor `health` are ignored, so the protocol can grow
without breaking old anton builds.

### `actions` — keys the plugin handles (optional)

```json
{"actions": [{"id": "review", "key": "v", "label": "toggle 👀 reviewing in Slack", "button": "Toggle 👀"},
             {"id": "run", "key": "T", "label": "run the PR's temoc specs", "button": "Run specs", "line": true}]}
```

Each `key` (one character) becomes a key on the selected PR, and in the
reader on the PR it shows; pressing it sends `{"action": id, "pr": url}` on
the plugin's stdin and flashes the `label`. Each action is also a button
(`v Toggle 👀`) in the list's button row and the reader's footer, and in the
reader's outline under that plugin's marks; `button` (optional) is the short
text for it, else the `label` is. The palette (`?`) lists them as
`<label> · <plugin>`. The latest `actions` line replaces the previous one;
actions vanish while the plugin is down. anton's own keys win a clash.

An action with `"line": true` also works on part of a PR: a file, or a
line of one (a temoc table's directive, or a row). In the reader, its key
on a spec file's tab sends that `file` (`T Run file`), and the ▶ on a test
table's header or a hovered row, and `r` on the focused table, send the
`file` and `line` (`01-anton.md`, Reader). The first such action is the
one ▶ and `r` use.

## Protocol: anton → plugin (stdin)

Long-running (`"rerun": "exit"`) plugins get a stdin pipe; one-shot plugins
keep `/dev/null`. One JSON object per line:

| line | when |
| --- | --- |
| `{"action": id, "pr": url}` | you pressed a key the plugin declared |
| `{"action": id, "pr": url, "file": path}` | ...on a file, for an action with `"line": true`: `path` as in the PR's diff |
| `{"action": id, "pr": url, "file": path, "line": n}` | ...on line `n` (1-based) of it: a table's directive line or a row's |
| `{"event": "approved", "pr": url}` | anton saw a **new** approval by you: a fetch shows your latest review APPROVED with a different `submittedAt` than the last fetch of that PR. Sent once, when the plugin is at `health: ok` (queued until then). Never replayed on restart, so an old approval can't undo a re-review; approvals made while anton was closed are not sent. |

A plugin that doesn't care never reads stdin: anton's writes are
non-blocking and dropped when the pipe is full, so it can't stall the UI.
Unknown lines must be ignored, so this can grow too.

Effects (a Slack reaction, say) should show up the normal way: the plugin
re-sends the PR's `pr` line. anton never updates plugin state on its own.

## Rendering

anton-gui (`06-gui.md`) shows all of it the same way, in panes: STATE as a
pill, tags as pills and in the preview, actions as buttons, marks in its
reader, notes in its LLM review pane, a run's progress in its Run log.

- **List views**: rows = union of PRs on `(plugin, list)`, then the view's
  local query filter. A list with no plugin running shows an empty table with
  `slack: not running` instead of "no PRs", so empty never lies.
- **STATE**: custom status per the ranking in `01-anton.md`.
- **TAGS**: tags from all plugins, plugin order from config, then tag order.
  `glyph text`, joined by space; overflow becomes `+N` → `👀 alice +1`.
- **Detail** under the list (third line): per plugin, its name, status and
  tags (`glyph text hint`), then `n read note` if a plugin attached one.
- **Reader** (`⏎`): marks on the tab of their file, above. On a wide
  terminal the outline of a spec file shows its result, its tables'
  results, what did not pass and the marks' `command`; of any other file, a
  block per plugin with marks on it: its counts, the commit they're for,
  and its action buttons.
- **Note overlay** (`n`): every plugin's note on the selected PR, headed
  `[plugin]` when there's more than one. `j`/`k`, `space`/PgUp/PgDn, `g`/`G`
  scroll; `y` copies the text; `esc`, `q` or `n` closes.

## Writing a plugin — minimal example

```sh
#!/bin/sh
# Put every open PR labelled "hotfix" in brick-so/core on a "hotfix" list.
gh search prs --repo brick-so/core --state open --label hotfix --json url \
  | jq -c '.[] | {pr: .url, lists: ["hotfix"], tags: [{glyph: "🔥", text: "hotfix"}]}'
```

With `"rerun": "refresh"` and a view
`{"name": "Hotfix", "plugin": "ci-bot", "list": "hotfix", "query": "is:open"}`,
that is a new tab in about four lines.

## Limits

- No plugin sandboxing: a plugin runs with your user's permissions, like a
  Claude Code hook or a git hook. Only configure commands you trust.
- Lines over 64 KiB are dropped.
- Plugins cannot change filters or what anton shows beyond their own `pr`
  lines. Actions run only on a key you press; events only report what anton
  already fetched.
