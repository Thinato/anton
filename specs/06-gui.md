# anton-gui — the desktop UI

`anton-gui` is anton in a window: the same views, filters, plugins and
reader as the terminal UI (`01-anton.md`), drawn with
[Dear ImGui](https://github.com/ocornut/imgui) on GLFW and OpenGL 3, in
GitHub's light and dark colors. It reads and writes the same
`config.json`, runs the same plugins and speaks the same protocol
(`02-plugins.md`). The TUI is unchanged; both binaries ship together.

Nothing here is a second implementation of anton's logic. Fetching,
filtering, the plugins and what they say, config writes and the markdown
parse live in `Core` (`src/core.hpp`), which both UIs derive from; the
GUI only draws it.

## The window

```
┌ anton  File Edit View Pull requests Plugins Help   [Search or run a command  Ctrl K]   ↻ Refreshed 1m ago  ☀ ┐
├ Views ──────────┬ Code review 12 ────────────────────────────────────────┬ Preview ──────────────────────┤
│ Created       5 │ [≡ recent:24h                          click a chip…]  │ acme/core#4812                │
│ Review req.   2 │ (state open ✕) (author anyone ▾) (+ Filter)  Sort Cols │ Onboarding: skip the team …   │
│▌Code review  12 │ Repo  #     Size          State          Age Author …  │ ● Review required  Ⓡ ren  3h  │
│ Assigned      1 │ core  4812  +385 −0 ▮▮▮▮▯ ● Review required 3h  Ⓡ ren  │ [Open in reader] [GitHub] [⧉] │
│ + New view      │ …                                                      │ Reviewers · From plugins ·    │
│ Repositories    │                                                        │ Description                   │
├─────────────────┴────────────────────────────────────────────────────────┴───────────────────────────────┤
│ ● slack  ● temoc   12 pull requests                              GitHub API 4,812 / 5,000   anton v0.1.N │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Top to bottom:

- **Menu bar**: the logo, File (Settings, config.json, Quit), Edit (copy
  URL, the query, clear the org/repo scope, the palette), View (the list
  or the reader, Panes, Theme, text size, Reset layout), Pull requests
  (open, browser, copy, refresh, sort, the views, New view), Plugins (each
  plugin's actions and Restart) and Help. In the middle a search box that
  opens the command palette; on the right the refresh state
  (`Refreshed 1m ago`, a click refreshes) and the theme button (sun in
  dark, moon in light).
- **Panes**, docked: Views, Pull requests and Preview in the list; Reader,
  Results / Outline / LLM review and Run log / Problems in the reader.
- **Status bar**: a dot per plugin (green up, yellow starting, red broken,
  blue running), the PR count (or, in the reader, the open file and its
  line count), the last `gh` error, then any message, the update notice,
  GitHub's GraphQL budget (`gh api rate_limit`, once a minute), the refresh
  interval and the version.

### The list

- **Views**: every view (`views` in the config) with its row count, the
  plugin of a list view, a yellow dot on a view whose filters changed. A
  click switches; right-click is the TUI's tab menu (save, save as new,
  revert, rename, duplicate, move, delete). `New view` adds an unsaved
  search view and puts the cursor in its query. Under it, the
  repositories in the view with their counts: a click scopes every view
  to one (the TUI's `p`), again to clear it.
- **Pull requests** (its tab says the view and its count): the query, in
  JetBrains Mono, with `Tab` completing the word under the cursor
  (`filters.hpp`'s completions, listed under the input while typing) and
  `Enter` applying it; then the chips read off it (`filters.hpp`): list,
  state, review, author, repo, label, every other qualifier, `+ Filter`.
  A chip's menu is its options, greyed with why when the view can't use
  them; a set chip's `✕` takes it out. `Sort` (the view's sort) and
  `Columns` (the built-in columns' `hidden`, in config.json, shared with
  the TUI; command columns stay the TUI's) are on the right. A changed
  view gets `● <view> changed  Save  Save as new view  Revert`.
  The table: Repo, #, Size (`+a −d`, compact, and GitHub's five-block
  diffstat), State (a pill: derive_state's pick in GitHub's words and
  colors; a plugin status shows its text), Age, Author (an avatar: the
  first letter on the person's `people` color) and Title with its label
  pills and, with the `tags` column on, the plugins' tags as pills (a
  person's in their color). Repo, Size and Age sort on a click, the sort
  marked `↓`. Rows are 38px, or 28px at Compact density. A click selects,
  a double-click reads, right-click is open / browser / copy / the plugin
  actions. The TUI's empty states: `not running`, `Loading…`, and `No open
  PRs match this view.` with the buttons that would show something.
- **Preview**: the selected PR: `owner/repo#n`, the title, its state
  pill, author, age, `+a −d` with the diffstat and the file count;
  `Open in reader` (Enter), `GitHub`, and a copy button. **Reviewers**:
  each reviewer's latest review (`latestReviews`), then the people a
  plugin tags on the PR (anton-slkx's 👀). **From plugins**: per plugin a
  dot, its state (the marks' `summary`, a status, else its tags), why it
  failed when it did, the start of its note, and its actions as buttons
  (`Run specs  T`). **Description**: the PR's body, rendered.

### The reader

`Enter`, a double-click or `Open in reader` opens it on the PR (the TUI's
reader: `01-anton.md`, Reader). The PR's header (back, the title, GitHub;
state, author, age, size, reviewers, each plugin's word on it), then a tab
per document: the Description, then each markdown file the PR changes,
specs first, named as in the TUI. A tab shows its file's state as an icon
(`✗`, `!`, `⋯`, `✓`, from its line-0 marks; a banner is `!`) and its rows'
marks counted by glyph, or the running clock.

Markdown is `md::blocks()` (`src/markdown.hpp`): the TUI's md4c parse,
unwrapped, laid out GitHub-style: headings with a rule under the first
two levels, paragraphs, nested lists and task lists, quotes, code blocks,
rules, tables with borders, aligned columns and a shaded header, temoc's
expected-value columns in the accent color and `# comment` columns
muted. Up to 880px wide, as GitHub's.

temoc's results, the way the canvas and temoc-ui show them, from the same
marks the TUI uses (`02-plugins.md`):

- **Banners** at the top: broken plugins and every plugin's `summary`
  (`reader_top`), stale marks (`results are for 86fa3ad`), and each line-0
  mark: running (blue, its phase and clock, ticking as the plugin re-sends
  it), passed (green), failed (red), `temoc failed` (red, the message in
  monospace). The file's state is left out when a banner already says it
  couldn't run. `Run file  T` / `Run all specs  T` on the right.
- **Rows**: a 32px gutter with the worst mark's glyph (`✓ ✗ ! ~ – ⋯ ◦`),
  colored by level, and the row tinted (passed a soft green, failed and
  error red, snoozed yellow, running blue; skipped and queued muted text).
  An error's or warning's text goes on lines under the row, across the
  table. Under the mouse, a test row's gutter is a `▶` that runs the row.
- **Tables**: a test table's title line is `▶` (run this table), its name
  (the heading above it, else its fixture) and the fixture with its
  snooze. Under the table, its result as a pill (`✗ failed · 1 of 4`),
  then the table's own failures (`make got exception …`) in red.
- Marks on other lines: their glyph in the gutter, an error's text under
  the line as `✗ line 13 · text`.

The panes around it:

- **Results**: `This file` (or `This pull request`) with its state pill,
  `Run file` and `Run all specs`; on a spec, `Tables` (each with its
  glyph and result word; a click focuses it and scrolls there) and `What
  did not pass` (a card per failing row, table or file, in line order; a
  click jumps to it); on the description, `Spec files` with their results;
  `Last command` (the marks' `command`) and the commit it ran on (yellow
  when the PR has moved on); the PR's other plugin actions.
- **Outline**: the headings, levels 1–3, and each test table with its
  result glyph; a click jumps there.
- **LLM review**: every plugin's note on the PR, rendered.
- **Run log**: the plugins' progress as it streams, newest at the bottom
  (it follows while scrolled to the end): what was sent (`action run
  specs/seats.md:9`), each change a `pr` line brings (`summary`, a file's
  state, a table's or row's result, `running`), `command`, `health`,
  stderr and exits. A clock ticking isn't news, nor are queued rows
  (`gui_logic.hpp`, `log_diff`). The reader shows the entries of its PR
  and the ones about no PR.
- **Problems** (its tab counts them): every error and warning mark on the
  PR, in any file, and broken plugins; a click opens the file there.

### Settings

`Ctrl/Cmd ,`, File › Settings, or the palette. A floating window over a
dimmed main window (a click outside closes it): drag its title bar to
move it, a corner to resize it; where it was is kept with the layout.
Every change is written to config.json at once through `config_write`,
as the TUI's Settings do; a file anton can't rewrite (comments, bad JSON)
says so in the footer and the change stays for the session.

- **General**: the start view (`start_view`), refresh every
  (`refresh_seconds`: 30s to 30 min), check watched PRs every
  (`watch_seconds`), install updates (`auto_update`), strip ticket prefixes
  from titles (`title_strip`, with a default pattern and an editable
  regex, and a before → after). The canvas's notification toggle has no
  key in config.json and is left out.
- **Appearance**: Dark / Light / Match system as cards drawn in each
  theme's colors; the interface size (11–18px, `Ctrl +`/`Ctrl -` too); the
  code font (JetBrains Mono, or the system's monospace: SF Mono or Menlo
  on macOS, DejaVu Sans Mono on Linux, JetBrains Mono when there's none);
  row density (Compact, Comfortable); the panes shown (Views, Preview, Run
  log, Status bar); Reset layout.
- **Views**: the views in order (drag a row's grip to reorder), each
  with Edit: name, what it shows (a GitHub search, or a plugin's list and
  which), its filter (a list view's must pass the local filter), sort,
  Duplicate, Delete (asks first). `New view` adds a saved one.
- **Plugins**: per plugin its dot, state, PR count and keys, `Restart`,
  `Open <file>` (its `--config`, else `llm.json` / `temoc.json` in the
  config dir for anton-llm / anton-temoc), its command, health text and
  last stderr line.

The footer: where it's saved, `Open config file`, `Done`. Files open in
`$VISUAL`/`$EDITOR` when that's a graphical editor (`code -w`), else with
`open -t` on macOS and `xdg-open` on Linux: there's no terminal to run vi
in.

### The command palette

`Ctrl/Cmd K`, the search box, or `?`. A box over the dimmed window: type,
`↑`/`↓`, `Enter` runs, `Esc` (or a click outside) closes. Results are
grouped: `Actions on core#4812` (the plugins' actions, `… on this file
only` on a spec tab, jump to the first row that didn't pass, open, GitHub,
copy, the note), `Pull requests` (every fetched PR, the view's first,
with what a plugin's run says: `· temoc failed`; Enter reads it), `Views`
(go to each, New view, Refresh) and `Settings` (open them, each theme,
show/hide panes, Reset layout, config.json, restart each plugin, open its
file). The match is `fuzzy_find` (`filters.hpp`, shared with the TUI's
`?`): the query's characters in order, ignoring case, highlighted; when
something has the query as a whole run, scattered matches are dropped.

## Panes and their layout

The panes are ImGui docking windows. Drag a pane's tab onto another
pane's edge or center to dock it there, out of the window to float it,
drag the line between panes to resize. Each pane's `▾` lists the tabs in
its node. The list and the reader are two dock spaces: each keeps its own
arrangement, and switching shows one.

The layout is saved per user in `$XDG_CONFIG_HOME/anton/gui-layout.ini`
(default `~/.config/anton/gui-layout.ini`; `gui_layout_path()`), never in
the current directory. With no layout file (or Reset layout, in View or
Settings) each dock space is built with ImGui's DockBuilder to the
canvas's proportions, scaled with the interface size: Views 224px on the
left, Preview 392px on the right, Pull requests in between; the reader in
the middle, Results / Outline / LLM review 360px on the right, Run log /
Problems 172px under the reader, opened on Results and Run log.

View › Panes (and Settings) hides or shows Views, Preview, Run log
(with Problems) and the status bar; they're saved in config.json.

## Themes

GitHub's light and dark defaults, with the canvas's tokens exactly
(`make_theme`, `src/gui_logic.hpp`, checked by `tests/gui_check.cpp`):
`bg`, `panel`, `inset`, `border`, `borderMuted`, `fg`, `muted`, `accent`,
`success`, `danger`, `attention`, `done` and their muted variants, the
tab line (`#f78166` dark, `#fd8c73` light), the primary button greens
(`#238636`, `#1f883d`; hover `#29903b`, `#1c8139`), and the palette's,
the scrim's and the shadow's.

`Match system` follows the OS, checked every 5 seconds off the UI thread:
macOS's `AppleInterfaceStyle`; on Linux GNOME's `color-scheme`
(`prefer-dark`, `prefer-light`), else a GTK theme with `dark` in its name,
else dark. The theme button and Settings switch it; the choice is saved.

Text is Inter (400, 500, 600) and JetBrains Mono, both under the SIL Open
Font License 1.1 (`assets/fonts/*-OFL.txt`), compiled into the binary.
Symbols Inter lacks come from JetBrains Mono, then the system's symbol
font (Apple Symbols, DejaVu Sans). Emoji (U+1F000 up) aren't drawn: a tag
`👀 paz` reads `paz` (`strip_emoji`); ImGui has no color glyphs without
FreeType.

## Configuration

anton-gui's own keys are under `"gui"` in config.json; the TUI ignores
them and `config_write` keeps them, written only once off their defaults
(`01-anton.md`, Writing config.json):

```json
"gui": {
  "theme": "system",
  "font_size": 13,
  "code_font": "JetBrains Mono",
  "density": "comfortable",
  "panes": { "views": true, "preview": true, "run_log": true, "status_bar": true }
}
```

| key | default | |
| --- | --- | --- |
| `theme` | `"system"` | `dark`, `light` or `system` |
| `font_size` | `13` | the interface size in px, 11–18 (clamped) |
| `code_font` | `"JetBrains Mono"` | or `"System monospace"` |
| `density` | `"comfortable"` | `compact` or `comfortable`: the list's rows |
| `panes` | all `true` | `views`, `preview`, `run_log`, `status_bar` |

Everything else (views, columns, people, plugins, refresh) is the TUI's
and means the same. A bad `gui` value doesn't stop either UI from
starting: the `gui` keys fall back to their defaults and anton-gui says
why in its status bar; the TUI ignores them.

## Keys

Mouse and keyboard both work. In the list, as the TUI: `j`/`k`, `↑`/`↓`,
`g`/`G`, Home/End, PgUp/PgDn move; `Enter` reads; `w` browser; `y` copy
URL; `r` refresh; `s` next sort; `/` the query; `f` + Filter; `x` clear
the org/repo scope; `S` save the view; `<` `>` move it; `+` new view; `n`
the LLM review; `1`–`9`, `Tab`/`Shift-Tab` views; `e` config.json; `,`
settings; `?` the palette; a plugin's key runs its action on the PR.

In the reader: `Esc`/`q` back; `Tab`/`]` and `Shift-Tab`/`[` next and
previous document; `j`/`k`, `↑`/`↓`, `space`, PgUp/PgDn, `g`/`G` scroll;
`J`/`K` focus the next/previous test table and scroll to it; `r` runs the
focused one; `T` (a "line" action's key) runs the file on a spec tab,
every spec on the description; `w`/`Enter` browser; `y` copy; `n` the LLM
review.

Anywhere: `Ctrl/Cmd K` palette, `Ctrl/Cmd ,` settings, `Ctrl/Cmd +`/`-`
text size, `Ctrl/Cmd Q` quit, `Esc` closes the palette, then Settings,
then the reader.

## Screenshot mode

```sh
anton-gui --screenshot out.png --screen list|reader|settings|palette \
          [--theme dark|light] [--pr N] [--file F] [--size WxH] [--scale S] \
          [--palette-query Q] [--settings-page general|appearance|views|plugins] \
          [--press KEY [--wait SECONDS]] [--keys 'j j <Enter> J r']
```

It opens a hidden window, renders into an offscreen framebuffer at
`--size` (1440x900) times `--scale` (1–3), waits until the list has its
rows (then the selected PR's description; in the reader, its body and
files), sets the screen up (`--pr` selects that PR, switching to a view
that has it; `--file` opens its tab by path, tab name or path tail;
`--press` sends a plugin's key, like `T`, and waits `--wait` seconds for
it; `--keys` types keys, a few frames apart: characters, or `<Enter>`,
`<Esc>`, `<Tab>`, `<Up>`, `<Down>`, `<Ctrl-K>`, `<Ctrl-,>`, so a key path
can be seen without a person at the keyboard), lets it settle a second and a half, writes the PNG (stb_image_write)
and exits. 30 seconds without the data, it captures what's there and says
so. It reads no layout file and writes neither the layout nor config.json,
so a screenshot is always the default layout; `--theme` holds for that
run only. With the demo:

```sh
ANTON=build/anton-gui ./docs/demo/run.sh --screenshot shot.png --screen reader --pr 4812 --file specs/onboarding.md
```

CI (`test.yml`) shoots every screen on Linux under xvfb and keeps them as
an artifact. `--query`, `--no-plugins` and `--version` are the TUI's.

## Build

Part of anton's CMake project, on by default (`-DANTON_GUI=OFF` to skip):

- Dear ImGui, docking branch, `v1.92.9b-docking`, with its GLFW and
  OpenGL 3 backends and `imgui_stdlib` (FetchContent, URL and SHA-256).
- GLFW 3.4 (FetchContent), X11 only on Linux: Wayland sessions run it
  through XWayland. It needs the X11 headers (`xorg-dev`) to build; X11
  and OpenGL are loaded at run time.
- stb_image_write, public domain, pinned to a commit (FetchContent).
- Inter 4.1 and JetBrains Mono 2.304 in `assets/fonts/`, embedded by a
  build step (`cmake/embed.cpp`).

On Linux without the X11 headers, configuring stops and says to install
them or pass `-DANTON_GUI=OFF`. Releases (`release.yml`) build the TUI
and its plugins first without the GUI, then anton-gui; if it fails, the
release ships without it on that platform and says so.

`self_update` knows which binary it is: anton-gui replaces itself with the
release's `anton-gui`, and `anton` and the plugins only where they are,
as `anton` does the other way round.

```
src/core.hpp/cpp      what both UIs share (moved out of ui*.cpp)
src/gui.hpp           the Gui state, its drawing helpers
src/gui.cpp           the frame: menu bar, dock spaces and layouts,
                      status bar, keys, theme, helpers, icons
src/gui_list.cpp      Views, Pull requests (filter, chips, table), Preview
src/gui_reader.cpp    the reader, markdown with marks, Results, Outline,
                      LLM review, Run log, Problems
src/gui_settings.cpp  Settings and the command palette
src/gui_main.cpp      flags, the window and loop, fonts, --screenshot
src/gui_logic.hpp     pure: theme tokens, pills, diffstat, emoji, results
                      off marks, where marks land on blocks, table names,
                      the run log's diff
tests/gui_check.cpp   asserts over gui_logic.hpp, fuzzy_find, the "gui"
                      keys' parse and round trip, the layout's path
```

## Not done

- The canvas's Preview tabs `Checks` and `Slack thread`: anton has no data
  for them (one rollup state; Slack threads stay in anton-slkx).
- Command columns are the TUI's only; the GUI's Columns menu greys them.
- Emoji in plugin text are dropped, not drawn.
- Floating panes stay inside the main window (no ImGui multi-viewport).
- No config reload while running: edit config.json and restart.
