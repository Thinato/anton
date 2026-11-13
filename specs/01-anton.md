# anton — PR review TUI

> "I don't *like* food, I *love* it. If I don't love it, I don't *swallow*."

A terminal UI that lists the GitHub pull requests that concern you, shows
how big each one is, what GitHub says about its state, and whatever external
tools (plugins) say about it — e.g. "alice is reviewing this" from Slack.

anton has **no Slack knowledge**. Slack is one plugin among possible others
(see `02-plugins.md`, `03-plugin-slkx.md`).

## Non-goals (v1)

- Reviewing code inside the TUI (diffs, comments). Enter opens the browser.
- Writing to GitHub (approve, merge, comment).
- Persistence between runs. Everything is rebuilt at startup.
- Any GitHub auth of its own. `gh auth status` must already succeed.

## Data source: one `gh api graphql` call

`gh search prs --json` has no `additions`/`deletions`, so anton does not use it.
GitHub's GraphQL `search` takes the **same query string** as the web search box
and returns everything needed in one round trip (verified 2026-11-05):

```graphql
query($q: String!, $after: String) {
  viewer { login }
  search(query: $q, type: ISSUE, first: 100, after: $after) {
    issueCount
    pageInfo { hasNextPage endCursor }
    nodes { ... on PullRequest {
      number title url isDraft createdAt updatedAt
      additions deletions changedFiles
      reviewDecision                       # APPROVED | CHANGES_REQUESTED | REVIEW_REQUIRED | null
      repository { nameWithOwner owner { login } }
      author { login }
      latestReviews(first: 20) { nodes { author { login } state } }
      commits(last: 1) { nodes { commit { statusCheckRollup { state } } } }
    } }
  }
}
```

Run as `gh api graphql -f query=... -f q=<search string>`, stdout parsed with
nlohmann/json. Pages are fetched until `hasNextPage` is false or 500 PRs
(5 pages) — whichever first.

### Second path: fetching a known list of PRs

A **list view** (below) already knows exactly which PRs it wants — URLs handed
over by a plugin. Search cannot select by URL, so these are fetched by alias:

```graphql
query {
  p0: repository(owner: "brick-so", name: "core") { pullRequest(number: 20387) { ...PrFields } }
  p1: repository(owner: "brick-so", name: "core") { pullRequest(number: 20384) { ...PrFields } }
}
```

`additions`/`deletions` are what make this slow (measured: ~4.7s per 50 PRs,
vs 0.7s for `state` alone), and a busy `#code-review` lists ~1000 PRs, nearly
all merged. So it runs in two passes, each 6 requests at a time:

1. **state** for every URL, 100 per request (~1s for a thousand).
2. **full fields** only for the `OPEN` ones, 10 per request.

Merged/closed PRs are cached as stubs (URL, repo, number, state). A stub gets
its full fetch only when the active view could show it (its query admits
closed PRs and the org/repo match). On refresh, only URLs that are new or were
last seen `OPEN` are fetched again: a merged PR never changes.

## Filters = GitHub search syntax, passed through

Every filter GitHub supports works because anton does not interpret the query;
it hands the string to `search`. `is:pr` is always prepended if absent.

Repository and organization are first-class because they are the common case:

| key | action | effect on the query |
| --- | --- | --- |
| `/` | edit the raw query | free text, e.g. `is:open review-requested:@me label:bug` |
| `o` | pick an org | sets the scope to `org:<pick>`, replacing any `org:`/`user:`/`repo:` in the query |
| `p` | pick a repo | sets the scope to `repo:<owner/name>`, replacing the same qualifiers |
| `x` | clear org/repo | removes `org:`, `user:`, `repo:` qualifiers |

The org/repo pickers list every owner/repo anton has seen in any result this
session, plus whatever you type (fuzzy match). No extra API call. Not just the
active view's rows: once you scope to one org, the others must stay pickable.

### Views

Tabs across the top, `1`..`9` or `Tab` to switch. Two kinds:

**Search views** — a named GitHub query. The defaults mirror GitHub's own
pull-requests page, landing on the first:

| name | query |
| --- | --- |
| Created | `is:open author:@me` |
| Review requests | `is:open review-requested:@me` |
| Assigned | `is:open assignee:@me` |
| Mentioned | `is:open mentions:@me` |

**List views** — the rows are the PRs a plugin puts on a named list; GitHub
only fills in their data. This is how "every PR posted to `#code-review`"
works: the Slack plugin maintains a `code-review` list (see
`03-plugin-slkx.md`), and a personal config line turns it into a tab:

```json
{ "name": "Code review", "plugin": "slack", "list": "code-review", "query": "is:open" }
```

A list view's `query` is applied **locally** to the fetched PRs, and supports
the qualifiers that can be answered from the fetched fields:
`is:open|closed|merged`, `is:draft`, `-is:draft`, `org:`, `user:`, `repo:`,
`author:` (with `@me`), `review:approved|changes_requested|required`,
`recent:<N>h|<N>d` (open, or closed/merged within that window: keeps a merge
on screen for a while, e.g. `recent:24h`), and `-` negation of each. Anything else is an error shown in the footer, not a
silent no-op. Full GitHub syntax remains available in search views.

The org/repo filter applies to the active view and is kept when switching
views (it is a session-wide scope, not per view). It works the same on both
kinds, since `org:` and `repo:` are in the local subset.

## Columns

```
 Code review (14) │ Created (5) │ Review requests (3)            org:brick-so
─────────────────────────────────────────────────────────────────────────────────
 REPO          #      SIZE          STATE        AGE  AUTHOR    TAGS      TITLE
 core       20387   +425 -8         review       2d   clemente  👀 alice  feat(server): conv…
 core       20384    +1k -3k        ✗ checks     5h   victor              server: workflow…
 server      1270   +12 -40         ⚙ queued     1w   guininov            Feat/pipeline att…
 web          881    +88 -0         draft        3w   paulo               wip: new nav
─────────────────────────────────────────────────────────────────────────────────
 r refresh  / query  o org  p repo  x clear  ⏎ open  y copy url  q quit   ↻ 40s ago
```

- **REPO**: repo name; owner shown only if results span more than one owner.
- **SIZE**: `+additions -deletions`, compact (below). Colored, in every view,
  by `additions + deletions`: green below `size.green_below` (default 200),
  red from `size.red_from` (default 1000), yellow in between.
- **STATE**: first match wins:
  1. `merged` (purple) / `closed` (dim) — only visible in views whose query
     allows them (`recent:24h`, `is:merged`, ...)
  2. `⚠ conflict` (red) — `mergeable == CONFLICTING`. Above plugin statuses: a
     queued PR with a conflict can't be merged, so that's what needs action.
     GitHub computes `mergeable` lazily: right after a push it may be UNKNOWN
     for one fetch.
  3. **custom status from a plugin** — e.g. `⚙ queued` or `💬 Ready?` from the Slack plugin's
     `#deploy-gogogo` rule. If several plugins set one, config order wins.
     Opt-in by nature: no plugin configured, no custom status.
  4. `draft` — `isDraft`
  5. `✗ checks` — rollup `FAILURE` or `ERROR`
  6. `changes` — `reviewDecision == CHANGES_REQUESTED`
  7. `approved ✓` — `reviewDecision == APPROVED`
  8. `you ✓` / `you ✎` — your own latest review is APPROVED / CHANGES_REQUESTED
     but the overall decision is still pending
  9. `review` — anything else

  A custom status sits below merged/closed/conflict so a stale plugin can never make a
  finished PR look in-flight, and above everything else because it is the more
  specific fact (a queued PR is also approved; "queued" is what you want to see).
- **AGE**: since `createdAt`: `5m`, `5h`, `2d`, `3w`, `4mo`, `1y`.
- **TAGS**: plugin-provided (see `02-plugins.md`), e.g. `👀 alice`.
- **TITLE**: takes the remaining width, truncated with `…`.

Sort is per view. Search views: `updatedAt` descending; `s` cycles updated → created → size → repo.
List views start on `list` (the plugin's `order`, higher first; PRs without one go last, by update) and `s` cycles list → updated → created → size → repo.

## Compact line counts

A PR over 1k lines is already too big; `1.9k` vs `1k` changes nothing, so no
decimals. Values are **truncated**, not rounded (999 never shows as `1k`, and
1999 stays `1k` — the column says "at least").

| n | shown |
| --- | --- |
| 0 – 999 | `n` |
| 1 000 – 999 999 | `⌊n / 1000⌋k` → `1k`, `10k`, `999k` |
| ≥ 1 000 000 | `⌊n / 1 000 000⌋M` |

Width is therefore at most 4 chars per side (`+999`, `-10k`).

## Keys

| key | |
| --- | --- |
| `j`/`k`, `↑`/`↓`, `g`/`G` | move |
| `⏎` | open PR in browser (`gh pr view --web <url>`) |
| `y` | copy URL (`pbcopy` / `xclip` / `wl-copy`, first found) |
| `r` | refresh now |
| `/` `o` `p` `x` | filters (above) |
| `s` | cycle sort |
| `1`..`9`, `Tab` | switch view |
| `n` | read a plugin's note on the PR, e.g. an LLM review (`02-plugins.md`) |
| `?` | help overlay (also lists plugin keys) |
| `q` | quit |
| *plugin keys* | declared by a running plugin, e.g. `v` (anton-slkx: toggle 👀 in Slack) on the selected PR. anton's own keys win a clash. See `02-plugins.md`. |

Refresh: on start, on `r`, on view/filter change, and every
`refresh_seconds` (default 120). A refresh never blocks input; the footer shows
`↻ loading…` and the old rows stay until the new ones arrive. A failed refresh
keeps the old rows and shows the `gh` stderr line in the footer.

Watched PRs: open PRs a plugin marks `"watch": true` (anton-slkx: the ones
you're reviewing) are re-fetched every `watch_seconds` (default 15, min 5) in
one quiet request, so an approval shows up (and turns into 👍) within seconds.
Live webhooks (`gh webhook forward`) were considered and rejected: they need
repo/org admin, allow one forwarder per org, and are documented testing-only.
Later: a relay (an org webhook into a small AWS service exposing a socket)
could feed a plugin that tells anton to re-fetch a PR the moment it changes.

## Configuration

`$XDG_CONFIG_HOME/anton/config.json` (default `~/.config/anton/config.json`).
Optional; every key has a default.

```json
{
  "refresh_seconds": 120,
  "watch_seconds": 15,
  "auto_update": true,
  "size": { "green_below": 450, "red_from": 1000 },
  "title_strip": "^\\[(PRO|ARG)-\\d+\\]\\s*",
  "people": {
    "VictorOda": { "name": "victor", "hex": "#e5534b", "ansi": "red" }
  },
  "views": [
    { "name": "Code review", "plugin": "slack", "list": "code-review", "query": "is:open" },
    { "name": "Created",     "query": "is:open author:@me" }
  ],
  "plugins": [
    { "name": "slack",
      "command": ["anton-slkx",
                  "--review-channel", "#code-review",
                  "--deploy-channel", "#deploy-gogogo"] }
  ]
}
```

`title_strip` (default off) is a regex (ECMAScript) removed from every title
as displayed, e.g. a Linear ticket prefix `[PRO-123] `. Display only: searches
and filters still see the full title. A bad regex is a config error.

`people` (default empty) maps a GitHub login (any case) to how you want to
see them. `name` replaces the login in the AUTHOR column; set it to their
Slack name and plugin tags with that text (`👀 victor`) get the same color, so
one person reads the same everywhere. Your own `you` tag uses your login's
entry. Colors: `hex` (`"#rrggbb"`) on truecolor terminals (`COLORTERM=truecolor`);
`ansi` (`"red"`, `"bright_red"`, `"gray"`, or 0-255) on the others. Give both
to choose each look; with only `hex`, older terminals get the nearest palette
color; with only `ansi`, every terminal uses it. A bad value is a config error.
Display only: `author:` filters still take the login.

`views`, when present, replaces the defaults entirely, so the order you write
is the tab order and the first is the landing view. The example above is the
personal setup: `#code-review` first, queued PRs marked.

`--query '<search>'` on the command line opens an ad-hoc view instead of the
configured ones. `--no-plugins` starts without plugins.

Validated at startup (anton exits with the problem before drawing): a list
view's `plugin` must name a configured plugin; at most 32 plugins.

Quitting — `q`, Ctrl-C, or SIGTERM/SIGHUP from outside — stops every plugin's
process group before anton exits.

## Updates

A release build knows its tag (`anton --version`, baked in by
`.github/workflows/release.yml`); a local build is `dev` and never updates.
At startup and every 6 hours, anton asks `gh api repos/Thinato/anton/releases/latest`.
If that tag is newer (by number: `v0.1.12-beta` > `v0.1.9-beta`; the suffix
doesn't count), it downloads the tarball for this platform with
`gh release download`, unpacks it next to the running binary and `rename()`s
`anton` (whatever it's called) and `anton-slkx` (only if it's already there)
over the old ones. The running process keeps its old file; the footer says
`⬆ updated to vX, restart anton to use it` until quit. Nothing is restarted
behind your back: plugins keep running the old `anton-slkx` until anton
restarts.

- `"auto_update": false` in the config: only `⬆ anton vX is out`.
- Can't write the install directory (e.g. a root-owned `/usr/local/bin`):
  same, plus why.
- Offline, rate-limited, a platform without prebuilt binaries: silent.

## Build & stack

Same toolchain as slkx, so the two feel like siblings:

- C++20, CMake ≥ 3.20, `-Wall -Wextra -Wpedantic`.
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI) for the TUI (FetchContent,
  pinned tag). Its `ScreenInteractive::Post` is the thread-safe way for the
  `gh` worker and plugin reader threads to hand results to the UI.
- nlohmann/json (system package or FetchContent, as slkx does).
- Subprocesses via `posix_spawnp` + pipes (`gh`, plugins). No shell, so query
  strings are never shell-interpreted.
- macOS and Linux. No Windows in v1.

### Layout

```
src/main.cpp          args, `gh auth status` preflight, runs the screen
src/config.hpp        config.json -> Config, defaults
src/proc.hpp/cpp      posix_spawnp + pipes, line reader (shared with the plugin)
src/github.hpp/cpp    search + alias fetch, gh graphql call, JSON → Pr
src/model.hpp         Pr, Tag, state derivation, compact(n), age(t),
                      qualifier editing, local query filter — pure
src/plugins.hpp/cpp   spawn plugins, read JSONL, restart, fold into the store
src/ui.hpp/cpp        FTXUI components: tabs, table, pickers, footer
plugins/slkx/main.cpp the anton-slkx executable (03-plugin-slkx.md)
plugins/slkx/fold.hpp its pure event fold — testable without slkx
tests/model_check.cpp assert-based self-check for model.hpp
tests/fold_check.cpp  assert-based self-check for fold.hpp
```

One CMake project, two executables. `anton-slkx` is built by default
(`-DANTON_SLKX_PLUGIN=OFF` to skip); it only *execs* `slkx` at runtime, so
building it needs nothing slkx-related, and anton never requires it.

`model.hpp` and `fold.hpp` are pure, with assert-based checks run by `ctest`,
in the same style as slkx's self-checks.

## Open questions

1. None left for anton itself; see `03-plugin-slkx.md`.
