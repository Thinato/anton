# anton — PR review TUI

> "I don't *like* food, I *love* it. If I don't love it, I don't *swallow*."

A terminal UI that lists the GitHub pull requests that concern you, shows
how big each one is, what GitHub says about its state, and whatever external
tools (plugins) say about it — e.g. "alice is reviewing this" from Slack.

anton has **no Slack knowledge**. Slack is one plugin among possible others
(see `02-plugins.md`, `03-plugin-slkx.md`, `04-plugin-llm.md`).

## Non-goals (v1)

- Reviewing code inside the TUI (diffs, comments). Enter reads the PR's
  description and its markdown files; `w` opens the browser for the rest.
- Writing to GitHub (approve, merge, comment).
- Persistence between runs beyond config.json (which Save and Settings write). Everything else is rebuilt at startup.
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
      labels(first: 20) { nodes { name } }
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
`label:` (any of the PR's first 20 labels, case-insensitive; quote one with
a space: `label:"needs review"`), `reviewed-by:@me` (you have a review on it,
from `latestReviews`; other logins are an error: anton only knows your own),
`recent:<N>h|<N>d` (open, or closed/merged within that window: keeps a merge
on screen for a while, e.g. `recent:24h`), and `-` negation of each. Anything else is an error shown in the footer, not a
silent no-op. Full GitHub syntax remains available in search views.

The org/repo filter applies to the active view and is kept when switching
views (it is a session-wide scope, not per view). It works the same on both
kinds, since `org:` and `repo:` are in the local subset.

## The screen

```
 anton   Created 5   Review requests 2   Code review• 4   Assigned 1   +             , settings   ? commands
  list slack · code-review ▾   state open + merged 24h ✕   review approved ✕   + filter       sort list order ▾
 / recent:24h review:approved                  ● Code review changed   S Save   Save as new view   Revert
──────────────────────────────────────────────────────────────────────────────────────────────────────────
  REPO   SIZE        STATE       ↓AGE  AUTHOR   TAGS          TITLE
▌ core    +331 -61   💬 Ready?     30m  lucas                  feat(billing): invoices list each seat…   ┃
  core    +260 -20   approved ✓    30m  lucas                  feat(billing): a downgrade waits for t…   │
──────────────────────────────────────────────────────────────────────────────────────────────────────────
 core#4825  feat(billing): invoices list each seat change on its own line
 lucas · opened 30m ago · +331 −61 in 6 files · checks passing · approved       github.com/acme/core/pull/4825
 slack 💬 Ready?
──────────────────────────────────────────────────────────────────────────────────────────────────────────
  ⏎ Read   w Browser   y Copy URL   v Toggle 👀   T Run specs                 slack ●  temoc ●   ↻ 40s ago
 tip click a header to sort · wheel scrolls · double-click reads · right-click a tab for options
```

Top to bottom (colors are truecolor when `COLORTERM=truecolor`, else the
nearest ANSI color; the selected row is then inverted rather than tinted):

- **Tab bar**: ` anton `, then a tab per view with its row count (search
  views are all fetched once at startup so every tab has one), a yellow `•`
  after a view whose filters changed (below), ` + ` (a new search view),
  and `, settings   ? commands` on the right.
- **Filter bar**: the active view's query read as chips. A list view starts
  with `list <plugin> · <list>` (pick another list the plugin offers). Then
  `state` (`is:open|closed|merged`, `recent:Nh`), `review` (`review:`,
  `reviewed-by:`, `review-requested:`), `author`, `repo` (`repo:`, `org:`,
  `user:`), set ones first; then a chip per `label:` and per other
  qualifier or word. A chip at its default reads `review any ▾`; a set one
  shows its value in cyan with `✕`, which takes its qualifiers out. Clicking
  a chip opens its options; `+ filter` (`f`) adds label, author, reviewer,
  draft, checks, base branch, updated, involves or text in title, then a
  value: one seen in results this session (labels with how many PRs have
  them) or whatever you type. In list views, options the local filter can't
  answer are greyed out with `search views only` (and `recent:` is greyed
  in search views). `sort <name> ▾` is on the right.
- **Query row**: `/ <query>`, keys dim and values bright; click it or press
  `/` to edit. While editing, `Tab` completes the word under the cursor
  (qualifier names, then values from results: `label:bu` → `label:bug`,
  with a popup to pick from), `↑`/`↓` go through queries applied this
  session (or the popup), `⏎` applies, `esc` cancels. The chips follow the
  typing. Chips and query are one filter: each chip edit is an edit of the
  query, and the other way round. With an org/repo scope set (`o`, `p`),
  the scope qualifiers in an edited query become the new scope.
- **Changed views**: a view whose query, sort, list or the session's
  org/repo scope makes it differ from config.json gets the `•` and, on the
  query row, `● <view> changed  S Save  Save as new view  Revert`. Save
  (`S`) writes that view (name, query with the scope in it, plugin, list,
  sort) into config.json; Save as new view asks for a name, adds the tab
  after this one and reverts this one; Revert restores the saved query and
  sort and drops the org/repo scope. A view made with `+` isn't saved until
  you save it (then it asks for its name) or `Discard` it.
- **Tab menu** (right-click a tab, or `m` for the active one): Save filters
  `S`, Save as new view…, Revert to saved, Rename…, Duplicate, Move left
  `<`, Move right `>`, Delete view… (asks first; the last view can't go).
  Rename, duplicate, move and delete write config.json right away.
- **The list** (columns below), with a scrollbar when it doesn't fit. A
  column header that is a sort key (REPO, SIZE, AGE = created) sorts by it
  on click and shows `↓` while it's the sort (`SIZE ↓`, `↓AGE`).
- **Detail**, three lines for the selected PR: `repo#number` and the full
  title; author · opened when · `+a −d in N files` · checks · review state ·
  labels, with the URL on the right (click opens it); then what plugins say
  (status, tags with their hints), command-column errors, `n read note`.
- **Buttons**: `⏎ Read  w Browser  y Copy URL  n Note`, then a button per
  plugin action (02-plugins.md); plugin health dots and `↻ 40s ago` right.
  With no PR: `? Commands  r Refresh  , Settings`.
- **Message line**: notices and errors, else a dim tip; the update notice
  on the right.

Nothing matches: `No open PRs match this view.` (or `No PRs …`), the query,
and buttons for what would show something: drop the last qualifier added,
include closed (drops `is:open`), edit the query, and, on a changed view,
revert.

Narrow terminals (under 100 columns): the chips fold into one `filters N ▾`
chip (a menu of them) with a summary; before that, chips at their default
drop out when the bar is too full. The query row only shows while editing
or when the view changed; tabs other than the active one shorten
(`Review`, `Code rev`) and scroll with `‹ ›`; the buttons become keys only;
`#`, then TAGS, then AGE step aside so TITLE keeps 20 columns.

### Mouse

Everything clickable is: tabs (right-click: the tab menu), chips and their
`✕`, menu rows, column headers, rows (click selects, double-click reads),
every button, the reader's tabs, outline and buttons, every settings
control. The wheel scrolls the list (the selection follows), the reader,
menus, the note and settings. A click outside an open menu closes it and
goes through to what's under it. Settings lists reorder by dragging their
`⠿` rows. `"mouse": false` (also in Settings) stops mouse tracking, so the
terminal selects text as usual.

### Commands (`?`)

A filterable list of every action with its key, grouped THIS PR, LIST,
VIEWS, ANTON, plugin actions included (`<label> · <plugin>`); type to
filter, `↑`/`↓`, `⏎` or a click runs one. Every key works without it.
Plugin health and stderr moved to Settings › General › Plugins.

## Columns

The table part of the screen above.

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

`"columns"` in the config picks the columns and their order, by name:
`repo`, `number` (the `#` column), `size`, `state`, `age`, `author`, `tags`,
`title`. The default is all of them in that order. An unknown name is a config
error. Without `title`, nothing flexes and the row ends after the last column.
A column turned off in Settings stays in the list as
`{"name": "repo", "hidden": true}` (a command column gets `"hidden": true`
in its object), so it can be turned back on; at least one must be shown.

### Command columns

An object in `"columns"` is a column you fill with your own program:

```json
{ "name": "CI", "width": 8, "timeout": 10,
  "command": ["sh", "-c", "gh pr checks \"$1\" --json state -q 'map(.state)|unique|join(\",\")'", "sh", "{url}"] }
```

- The PR goes to the command as a JSON object on stdin (`url`, `repo`,
  `owner`, `number`, `title`, `author`, `state`, `draft`, `review_decision`,
  `checks`, `my_review`, `mergeable`, `additions`, `deletions`, and `created`,
  `updated`, `closed` as Unix seconds). Each `{field}` in `command` is
  replaced by that field (strings bare, others as JSON). Anything else in
  braces is left alone. There's no shell: `$HOME` and a leading `~` are
  expanded as in plugin commands, and `sh -c` is how you get pipes.
- The cell is the first line of stdout, with control characters removed, cut to
  `width` (default 10, at most 80). `name` is the header, as written.
- A cell runs when its row is first drawn, again on every refresh (the
  2-minute one and `r`), and when the PR's `updatedAt` changes. It shows `…`
  until the first run ends, then keeps the last output while a rerun is going.
  Four commands run at once.
- A non-zero exit, a command that can't start, or one still running at
  `timeout` (seconds, default 10; its process group gets SIGTERM) shows a red
  `✗`. The detail line of the selected PR says why: `[CI] ✗ exit 1: <first
  stderr line>`.
- Stubs (list-view PRs whose fields aren't fetched yet) get no run until they're filled.

Sort is per view. Search views: `updatedAt` descending; `s` cycles updated → created → size → repo.
List views start on `list` (the plugin's `order`, higher first; PRs without one go last, by update) and `s` cycles list → updated → created → size → repo.
A view's `"sort"` in the config (`updated`, `created`, `size`, `repo`, or
`list` in list views) is where it starts; the sort chip, a header click and
`s` change it, which marks the view changed.

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
| `j`/`k`, `↑`/`↓`, `g`/`G`, PgUp/PgDn | move |
| `⏎` | read the PR's description and markdown files (Reader, below) |
| `w` | open PR in browser (`gh pr view --web <url>`) |
| `y` | copy URL (`pbcopy` / `xclip` / `wl-copy`, first found) |
| `r` | refresh now |
| `/` `o` `p` `x` | filters (above); `/` edits the query row |
| `f` | add a filter (`+ filter`) |
| `s` | cycle sort |
| `1`..`9`, `Tab`/`Shift-Tab` | switch view |
| `S` | save the view's filters to config.json |
| `m` | the active tab's menu (save, revert, rename, duplicate, move, delete) |
| `<` `>` | move the active tab left / right |
| `+` | new search view |
| `n` | read a plugin's note on the PR, e.g. an LLM review (`02-plugins.md`) |
| `?` | commands: every action and its key (above) |
| `,` | settings |
| `e` | config.json in `$EDITOR` |
| `q` | quit |
| *plugin keys* | declared by a running plugin, e.g. `v` (anton-slkx: toggle 👀 in Slack) on the selected PR, also shown as buttons. anton's own keys win a clash. See `02-plugins.md`. |

Menus: `↑`/`↓` (`j`/`k` when there's nothing to type), `⏎` runs, `→`
opens a submenu, `←`/`esc` goes back, and a row's key (`S`, `<`) runs it.

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

## Reader

`⏎` (or a double-click) replaces the table with the selected PR's
description: ` ← back ` (click), `repo#number` and the title; then author ·
opened · `+a −d` · state · plugin tags, with where the text is from on the
right (the URL, or a file's `path @ sha`); a row of tabs, the open one
underlined; then the body rendered as markdown, glow-style, with a
scrollbar. `j`/`k`, `space`/PgDn, PgUp, `g`/`G` and the wheel scroll, and the
footer says `lines a–b of N`; `w` or `⏎` opens the PR in the browser, `y`
copies its URL, `q`/`esc` goes back to the table. Plugin keys work here
too, on this PR (`T` runs its temoc specs). The footer's buttons: `esc
Back`, `Tab Next file`, one per plugin action, `w Browser`, `y Copy URL`.

On terminals 120 columns or wider an outline sits on the right, `ON THIS
PAGE`: the headings (levels 1-3) and, in a file, each temoc table
(`decisionTable` directive) with the worst result on it (`✗ StepsFixture`,
`💤 UpgradeFixture`), in source order; the section you're reading is
highlighted, and a click jumps there (to the first display line of that
source line or after). Under it, a block per plugin with marks on the file:
its name, the counts (`✓ 5  ✗ 2  💤 1`), the commit it ran on and whether
that's still the head, and a button per action that plugin offers (`T Run
specs`).

The team reads more markdown than code, so the reader also shows the
markdown files the PR adds or changes, each in a tab after `Description`:
temoc specs first (`🧪`), then the others, in the PR's order. `Tab`/`]` and
`Shift-Tab`/`[` switch tabs, and each keeps its own scroll. A tab is named
by the file's basename, with as many parent directories as it takes to tell
two apart (`api/README.md`, `web/README.md`), followed by its marks, if a
plugin left any, counted by glyph (`🧪 calculator.md ✓ 10 ✗ 1 💤 1`); the
line under the title is then the file's full path and the commit it's at.
Tabs that don't fit drop off the right, or the left to keep the open one
on screen.

- The body comes from `gh pr view <url> --json body,headRefOid`, off the
  UI thread, and is kept per URL for the session. It's fetched again when
  the PR's `updatedAt` moves (editing the description or pushing bumps it).
  Meanwhile the reader says `loading…`; a failure says why (reopening
  retries, and a failed refetch keeps the text it had); an empty body reads
  `No description.`
- The files are the PR's `.md`/`.markdown` files that aren't removed
  (`gh api --paginate repos/O/R/pulls/N/files`), each read at the head
  commit the body came with (`gh api repos/O/R/contents/<path>?ref=<sha>`,
  raw), off the UI thread, 6 at a time, once per (URL, head) for the
  session. All of them are read when the reader opens the PR: putting specs
  first needs their text. Meanwhile the tab row says `loading files…`; a
  failure to list them says why there (reopening retries), and a file that
  couldn't be read says so on its tab.
- **temoc directives**: a `[//]: # "decisionTable Fixture"` line (or
  `(…)`, `'…'`; at most three spaces in, outside fenced code) is a link
  reference definition, which markdown shows as nothing. The reader shows
  it, in the description too, as a badge line before its table:
  `🧪 CalculatorFixture#log`, then `· snoozed until 2026-12-01` in yellow
  for `-- snooze until 2026-12-01`, or `· snooze expired 2026-12-01` dim
  once the day has passed (in UTC, as temoc counts). `import X` reads as a
  dim line. md4c needs a blank line between the directives and the table,
  as temoc's own files have.
- **Test tables**: in the table after a `decisionTable` directive, a column
  whose header has a `?` holds expected values and is cyan, header and
  cells; a `# comment` column is dim, and is the first to shrink when the
  table is too wide. Other tables are as below.
- **Marks** (`02-plugins.md`): on a file's tab, every plugin's marks on
  that file. Each line gets a 3-column gutter, left of any table, with the
  glyph of the worst mark on its source line, colored by level (ok green,
  error red, warn yellow, dim dim). An error's or warning's text goes under
  its line, and for a table row under the table: `✗ line 13 · sum:
  expected 5, got 4`. A mark on a directive belongs to its badge, its text
  after it (`🧪 CalculatorFixture · 1 failed, 3 passed`, or on the next line
  when that doesn't fit); line 0's are at the top. When a plugin's
  `marks.ref` isn't the head the tab shows, the tab starts with `temoc:
  results are for 86fa3ad; the PR has moved on`.
- Rendering (`src/markdown.cpp`, parsed by [md4c](https://github.com/mity/md4c)
  in its GitHub dialect) is pure, text and width in, styled lines out, and
  redone when the terminal's width changes. At most 120 columns wide.
  Headings keep their `#`s and get a color per level; paragraphs wrap by
  display width (CJK and emoji count two); lists nest with a hanging indent,
  task items show `☐`/`☑`; quotes get a `│` bar; code blocks are dim and
  never wrapped (a line too long ends in `…`); links show their URL dim after
  the text unless the text is the URL; images read `[image: alt]`.
- Tables get box borders and a line under the header, and keep each
  column's alignment. One too wide for the screen shrinks its widest columns
  until it fits and wraps text inside cells; then every row is separated, so
  wrapped rows don't blur together.
- PR templates are HTML-heavy: `<!-- comments -->` vanish,
  `<details><summary>X</summary>` reads `▸ X` followed by the content, `<br>`
  breaks the line, `<img alt>` is an image, other tags go and their text
  stays.
- Every display line keeps the source line it came from (each line of a
  table row carries the row's, a badge its directive's), which is how marks
  find their rows.
- Text goes through the same control-character filter as plugin output.

The `n` note overlay renders notes with the same renderer.

## Configuration

`$XDG_CONFIG_HOME/anton/config.json` (default `~/.config/anton/config.json`).
Optional; every key has a default.

```json
{
  "refresh_seconds": 120,
  "watch_seconds": 15,
  "auto_update": true,
  "mouse": true,
  "start_view": "Code review",
  "size": { "green_below": 450, "red_from": 1000 },
  "title_strip": "^\\[(PRO|ARG)-\\d+\\]\\s*",
  "people": {
    "VictorOda": { "name": "victor", "hex": "#e5534b", "ansi": "red" }
  },
  "views": [
    { "name": "Code review", "plugin": "slack", "list": "code-review", "query": "is:open" },
    { "name": "Created",     "query": "is:open author:@me", "sort": "size" }
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
is the tab order. The landing view is `start_view` (a view's name), else the
first. A view's `sort` is where its sort starts (Columns, above). The example
above is the personal setup: `#code-review` first, queued PRs marked.

`mouse` (default true): clicks and the wheel (The screen, above). False
leaves the mouse to the terminal.

`--query '<search>'` on the command line opens an ad-hoc view instead of the
configured ones; it's unsaved (`•`), and saving it adds it to `views`
without touching the others. `--no-plugins` starts without plugins.

### Settings (`,`)

A full screen: General, Views, Columns on the left (`Tab` switches),
`j`/`k` move between controls, `⏎`/space changes one (or asks for a typed
value), `h`/`l` or `-`/`+` step numbers and flip toggles, `J`/`K` (or
dragging `⠿`) reorder, `e` opens config.json in `$EDITOR`, `esc` closes.
Every change is checked and written to config.json at once; a problem
shows on the second line, red.

- **General**: refresh and watch seconds (`−`/`+` by 10 and 5, or typed;
  clamped to the same minimums as the file, 10 and 5), auto update, the
  start view, mouse. Then each plugin's state, health text, keys, PR count,
  command (dim) and last stderr line, with a Restart button (it stops the
  plugin and runs it again now). A plugin's command lives in the file only.
- **Views**: the tabs in order (reorder, `+ New view`), and for the one
  picked: name, kind (a GitHub search, or the PRs a plugin lists: plugin and
  list pickers, from the lists the plugin has sent), filter (a list view's
  must pass the local filter), sort, Duplicate, Delete view (asks first).
  Changes apply to the view as saved and as it is on screen.
- **Columns**: every built-in and command column, toggled and reordered (a
  command column's command stays in the file), the size colors with samples
  in each color, `title_strip` with a live before → after, and a preview of
  the table on the active view's first rows.

`e` (here, in the list, or from `?`) suspends anton's screen, runs
`$EDITOR` (split on spaces, so `code -w` works; `vi` without one) on
config.json and reads it back. A file that doesn't load leaves everything
as it was and says why; changed plugins take effect when anton restarts.

### Writing config.json

anton reads the file, sets only the keys it manages (`views`, `columns`,
`size`, `title_strip`, `refresh_seconds`, `watch_seconds`, `auto_update`,
`mouse`, `start_view`) and writes it back with every other key and value
(`plugins`, `people`, a command column's `command`, keys it doesn't know)
as they were, keys in their order, 2-space indent. A managed key the file
doesn't have is only added once it's off its default. A view or column
entry keeps the keys anton doesn't know, matched by name. The new text goes
to a temp file next to it, with the old one's permissions, renamed over
it. A file that doesn't parse, isn't an object, or has comments (rewriting
would drop them) is left alone: the change stays for the session and the
footer says `config.json not saved: …`. (`src/config_write.hpp`;
`tests/filters_check.cpp`.)

Validated at startup (anton exits with the problem before drawing): a list
view's `plugin` must name a configured plugin; at most 32 plugins.

Quitting — `q`, Ctrl-C, or SIGTERM/SIGHUP from outside — stops every plugin's
process group before anton exits.

## Updates

A release build knows its tag (`anton --version`, baked in by
`.github/workflows/release.yml`); a local build is `dev` and never updates.
At startup and every 15 minutes, anton asks `gh api repos/Thinato/anton/releases/latest`.
If that tag is newer (by number: `v0.1.12-beta` > `v0.1.9-beta`; the suffix
doesn't count), it downloads the tarball for this platform with
`gh release download`, unpacks it next to the running binary and `rename()`s
`anton` (whatever it's called), `anton-slkx`, `anton-llm` and `anton-temoc` (each only if
it's already there) over the old ones. The running process keeps its old file; the footer says
`⬆ updated to vX, restart anton to use it` until quit. Nothing is restarted
behind your back: plugins keep running the old binaries until anton
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
- [md4c](https://github.com/mity/md4c) parses markdown (FetchContent, pinned
  tag; its one C file is built as a static library).
- Subprocesses via `posix_spawnp` + pipes (`gh`, plugins). No shell, so query
  strings are never shell-interpreted.
- macOS and Linux. No Windows in v1.

### Layout

```
src/main.cpp          args, `gh auth status` preflight, runs the screen
src/config.hpp        config.json -> Config, defaults
src/config_write.hpp  Config -> config.json, merged into what's there; atomic save
src/filters.hpp       chips, menu options and completions read off a query — pure
src/proc.hpp/cpp      posix_spawnp + pipes, line reader (shared with the plugin)
src/github.hpp/cpp    search + alias fetch, gh graphql call, JSON → Pr
src/model.hpp         Pr, Tag, state derivation, compact(n), age(t),
                      qualifier editing, local query filter — pure
src/plugins.hpp/cpp   spawn plugins, read JSONL, restart, fold into the store
src/ui.hpp            run_ui()
src/ui_impl.hpp       the UI's state and shared pieces (colors, menus, hits)
src/ui.cpp            fetching, plugins, views, the list, filter bar, menus,
                      the palette, mouse dispatch
src/ui_reader.cpp     the reader (outline included) and the note overlay
src/ui_settings.cpp   settings, $EDITOR
src/markdown.hpp/cpp  markdown -> styled display lines (md4c), temoc
                      directives, marks laid onto the lines — pure
plugins/slkx/main.cpp the anton-slkx executable (03-plugin-slkx.md)
plugins/slkx/fold.hpp its pure event fold — testable without slkx
plugins/llm/main.cpp  the anton-llm executable (04-plugin-llm.md)
plugins/llm/llm.hpp   its pure config parse, substitution, pr line
plugins/temoc/main.cpp the anton-temoc executable (05-plugin-temoc.md)
plugins/temoc/temoc.hpp its pure config parse, temoc event fold, pr line
plugins/run.hpp       {tmp}, PR worktree, watched run: shared by llm and temoc
tests/model_check.cpp assert-based self-check for model.hpp
tests/filters_check.cpp   assert-based self-check for filters.hpp, config_write.hpp
tests/fold_check.cpp  assert-based self-check for fold.hpp
tests/llm_check.cpp   assert-based self-check for llm.hpp
tests/markdown_check.cpp  assert-based self-check for markdown.cpp
tests/temoc_check.cpp     assert-based self-check for temoc.hpp
```

One CMake project, four executables. `anton-slkx` is built by default
(`-DANTON_SLKX_PLUGIN=OFF` to skip); it only *execs* `slkx` at runtime, so
building it needs nothing slkx-related, and anton never requires it.
`anton-llm` likewise (`-DANTON_LLM_PLUGIN=OFF`); it only execs the commands
you configure. `anton-temoc` too (`-DANTON_TEMOC_PLUGIN=OFF`); it execs `temoc`.

`model.hpp`, `filters.hpp`, `config_write.hpp` (but its `save_config`), `fold.hpp`, `llm.hpp` and `temoc.hpp` are pure, with assert-based checks run by `ctest`,
in the same style as slkx's self-checks.

## Open questions

1. None left for anton itself; see `03-plugin-slkx.md`.
