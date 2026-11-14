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
| `name` | required | referenced by list views; shown in errors and the `?` overlay |
| `command` | required | argv, run without a shell, cwd = anton's cwd. `$HOME`, `${HOME}` and a leading `~` are expanded (only these: there's no shell), so `"$HOME/.local/bin/anton-slkx"` works in a config shared between people |
| `rerun` | `"exit"` | `"exit"`: restart 5s after it exits (doubling to 5 min on repeated quick exits). `"refresh"`: run once per anton refresh. `"never"`: run once. |
| `env` | `{}` | extra environment variables |

## Protocol: plugin → anton (stdout)

One JSON object per line, UTF-8, `\n`-terminated. Lines that fail to parse are
ignored and counted (shown in `?`). stderr is not protocol: each line flashes in
the message line for 10s (warnings like a bad setting), and the last line is
shown in the `?` overlay for debugging.

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
`slack ○` (anything else), with `text` in `?`.

### Anything else

Objects with neither `pr` nor `health` are ignored, so the protocol can grow
without breaking old anton builds.

### `actions` — keys the plugin handles (optional)

```json
{"actions": [{"id": "review", "key": "v", "label": "toggle 👀 reviewing in Slack"}]}
```

Each `key` (one character) becomes a key on the selected PR; pressing it
sends `{"action": id, "pr": url}` on the plugin's stdin and flashes the
`label`. The latest `actions` line replaces the previous one; actions vanish
while the plugin is down. anton's own keys win a clash. Listed in `?`.

## Protocol: anton → plugin (stdin)

Long-running (`"rerun": "exit"`) plugins get a stdin pipe; one-shot plugins
keep `/dev/null`. One JSON object per line:

| line | when |
| --- | --- |
| `{"action": id, "pr": url}` | you pressed a key the plugin declared |
| `{"event": "approved", "pr": url}` | anton saw a **new** approval by you: a fetch shows your latest review APPROVED with a different `submittedAt` than the last fetch of that PR. Sent once, when the plugin is at `health: ok` (queued until then). Never replayed on restart, so an old approval can't undo a re-review; approvals made while anton was closed are not sent. |

A plugin that doesn't care never reads stdin: anton's writes are
non-blocking and dropped when the pipe is full, so it can't stall the UI.
Unknown lines must be ignored, so this can grow too.

Effects (a Slack reaction, say) should show up the normal way: the plugin
re-sends the PR's `pr` line. anton never updates plugin state on its own.

## Rendering

- **List views**: rows = union of PRs on `(plugin, list)`, then the view's
  local query filter. A list with no plugin running shows an empty table with
  `slack: not running` instead of "no PRs", so empty never lies.
- **STATE**: custom status per the ranking in `01-anton.md`.
- **TAGS**: tags from all plugins, plugin order from config, then tag order.
  `glyph text`, joined by space; overflow becomes `+N` → `👀 alice +1`.
- **Detail line** under the selected row: every tag and status as
  `[plugin] glyph text — hint`, then `n read note` if a plugin attached one.
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
