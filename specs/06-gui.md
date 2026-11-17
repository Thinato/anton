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
┌ ⧉ anton   [⌕ Search or run a command                ⌘K]       Refreshed 1m ago ↻ ☾ ┐  toolbar 38
├ Views ────────┬ Code review 25 ──────────────────────────┬ Preview  core#4812 × ─────┤  tab bars 28
│ Created     5 │ [≡ recent:24h          ] [Sort List order] [Columns]│ Onboarding: skip the …    │
│ Review r.   2 │ (list slack · code-review ▾) (state open ×) + Filter│ ● Review required · ren · │
│▌Code review 25│ #    Repo Size     State            Age Author Title│ ─────────────────────────│
│ Assigned    1 │ 4812 core +385 −0  ● Review required  3h Ⓡ ren  Onb…│ ✗ Failed · 1 of 4  Run T │
│ + New view    │ 4825 core +331 −61 ● Ready?          30m Ⓛ lucas…   ├ Results  Run log  Problems┤  ~196
│ Repositories  │ …                                                   │ ✗ Onboarding steps  line 6│
├───────────────┴─────────────────────────────────────────────────────┴───────────────────────────┤
│ ● slack  ● temoc   25 pull requests                GitHub API 4,808 / 5,000  Refreshes every 120s │  status 22
└───────────────────────────────────────────────────────────────────────────────────────────────────┘
```

Top to bottom:

- **Menus.** On macOS the system's menu bar, nothing in the window
  (below). On Linux an ImGui menu bar in the window with the same menus.
- **Toolbar**, 38px: the logo and name; in the middle a 380px search box
  with its key hint (`⌘K`, `Ctrl K`) that opens the command palette; on
  the right the refresh state (`Refreshed 1m ago`), a refresh button and
  the theme button (sun in dark, moon in light), 24px each.
- **One dock space**: Views 200px | Pull requests | the right column
  520px with Preview. Opening a PR adds the reader as a tab beside
  Preview, and a node of about 196px under them with Results, Run log
  and Problems (The reader).
- **Status bar**, 22px of 11px text: a dot per plugin (green up, yellow
  starting, red broken, blue running), the PR count (or, while the reader
  has the focus, its open file and line count), the last `gh` error, then
  any message, the update notice, GitHub's GraphQL budget (`gh api
  rate_limit`, once a minute), the refresh interval and the version.

### The menu bar

`Gui::menus()` is the menus as things are now: each entry's label, its
shortcut, its check mark (read from the live config), whether it's
enabled, and what it runs. Both menu bars are built from it, and `keys()`
runs an entry whose shortcut has `Mod` in it (`Cmd` on macOS, `Ctrl`
elsewhere): a menu item, its shortcut, a button and the palette reach the
same code.

On macOS `src/gui_menu_mac.mm` (Objective-C++, Cocoa, built only there)
installs NSMenus in place of GLFW's. A top-level menu is rebuilt from
`menus()` each time it opens (the views, the plugins' actions, labels
like Enter/Exit Full Screen), and `validateMenuItem` re-reads an entry's
enabled state and check mark before the menu shows and before a key
equivalent fires. An item remembers its place in the table and its label;
a click or its key equivalent finds the entry again and posts its action
to the UI loop. Only shortcuts with Cmd are key equivalents: a plain key
(`H`, `↵`) is drawn after a tab in the item's title, since as a key
equivalent the menu would take it from the text inputs. An entry the menu
takes never reaches ImGui, so nothing runs twice; ⌘= (Zoom In is `⌘+`)
isn't matched by the menu and reaches `keys()`, which runs it once.

- **anton**: About anton, Settings… `⌘,`, Hide anton `⌘H`, Quit anton
  `⌘Q`. (On Linux, Settings and Quit are in File.)
- **File**: Open config.json, Close Reader `⌘W`.
- **Edit**: Cut `⌘X`, Copy `⌘C`, Paste `⌘V`, Select All `⌘A`; Copy URL
  `Y`, Edit the Query `/`, Clear org/repo `X`; Command Palette `⌘K`. Cut,
  Copy, Paste and Select All send their key to ImGui with Cmd held, so
  they work in a text input from the menu and from the shortcut alike;
  they're enabled while one is being typed in (Copy also copies the PR's
  URL otherwise).
- **View**, as the canvas: Views `⌘1`, Preview `⌘2`, Results `⇧⌘R`,
  Run log `⌘J` (each a pane, checked while shown); Columns ▸ (Number,
  Repo, Size (+/−), Size blocks, State, Age, Author, Avatars, Labels,
  Plugin tags, Customize… `⌘,`), Row height ▸ (Compact 24, Cozy 28, Roomy
  34), State style ▸ (Circle and label, Coloured text, Pill); Theme ▸
  (Dark, Light, Match System), Zoom In `⌘+`, Zoom Out `⌘−`, Actual Size
  `⌘0` (the interface size); Reset Layout, Enter Full Screen `⌃⌘F`
  (`F11` on Linux).
- **Pull requests**: Open in Reader `↵`, Open on GitHub `W`, Copy URL
  `Y`; Refresh `R`, Sort ▸; the views (`1`–`9`), New View `+`.
- **Plugins**: per running plugin its actions (the short button text and
  the key) and Restart; Plugin Settings….
- **Window**: Minimize `⌘M`, Zoom; Outline, LLM review (with a reader
  open), Status bar.
- **Help**: Search or Run a Command, anton on GitHub, the version.

### The list

- **Views**: 24px rows, every view (`views` in the config) with its row
  count in mono, the plugin of a list view, a yellow dot on a view whose
  filters changed. A click switches; right-click is the TUI's tab menu
  (save, save as new, revert, rename, duplicate, move, delete). `New
  view` adds an unsaved search view and puts the cursor in its query.
  Under a rule, `Repositories` (a 22px label) and the repositories in the
  view with their counts: a click scopes every view to one (the TUI's
  `p`), again to clear it.
- **Pull requests** (its tab says the view and its count): the query, in
  JetBrains Mono, with `Tab` completing the word under the cursor
  (`filters.hpp`'s completions, listed under the input while typing) and
  `Enter` applying it, then `Sort` (the view's sort) and `Columns` (View
  › Columns' entries) on the same 24px row. Under it the chips read off
  the query (`filters.hpp`), 22px: list, state, review, author, repo,
  label, every other qualifier, `+ Filter`. A chip's menu is its options,
  greyed with why when the view can't use them; a set chip's `×` takes it
  out. A changed view gets `● <view> changed  Save  Save as new view
  Revert`.
  The table, from `gui.columns` in their order, the title last: `#`
  (mono 11), Repo, Size (`+a −d`, mono), Size blocks (GitHub's five-block
  diffstat, 7px squares; off by default), State (`gui.state_style`: a dot
  and the label, the label in the state's colour, or a pill; derive_state's
  pick in GitHub's words), Age (right-aligned), Author (`gui.author_style`:
  an avatar, the first letter on the person's `people` colour, and the
  name; or the name alone in weight 500 and a colour from the canvas's
  NAMES palette for the theme, picked by a stable hash of the login,
  `name_color`), and Title with its labels as 16px chips and the plugins'
  tags after it (each when its column is on). The canvas's widths: 40,
  40, 78, 42, 128, 34, 92. Rows are 24px (compact), 28 (cozy) or 34
  (roomy). Repo, Size and Age sort on a click, the sort marked `↓`. A
  click selects, a double-click reads, right-click is open / browser /
  copy / the plugin actions. The TUI's empty states: `not running`,
  `Loading…`, and `No open PRs match this view.` with the buttons that
  would show something.
- **Preview**: the selected PR: `owner/repo#n` (mono 11), the title
  (14/600), a meta line (the state, the author, the age, `+a −d · n
  files`); `Open in reader ↵` (primary), `GitHub` and a copy button, 24px.
  Then three 22px sections: **Reviewers** (each reviewer's latest review,
  then the people a plugin tags on the PR, anton-slkx's 👀), **Plugins**
  (a 28px row each: a dot, its name, one line of what it says, the run's
  state, a status, its tags and the start of its note, all of it on
  hover; its actions as 20px buttons, `Review H`) and **Description** (the
  PR's body, rendered).

### The reader

`Enter`, a double-click, `Open in reader`, the menu or the palette opens
the reader on the PR: one window, titled like `core#4812`, with the ID
`###reader` so ImGui remembers where it was. Opening another PR turns it
to that one. Where it opens is `gui.reader_placement`:

- `beside_preview` (the default): a tab in Preview's node;
- `last`: wherever the reader was last (the first time, beside Preview);
- `window`: floating, in a window of its own. Multi-viewports are on, so
  it's an OS window you can move off the main one (it never merges back
  into it); off for `--screenshot`, whose window is hidden.

The first time a layout opens the reader, Results, Run log and Problems
get a node split off under Preview's, about 196px; it hides with the
reader and comes back with it. Outline and LLM review open from the
Window menu, as tabs beside Results. Closing the reader's tab (its `×`,
`⌘W`/`Ctrl W`, `Esc`, `q`) gives the list the focus. The reader's keys
work while it, or a pane about its PR, has the focus; the list's keys
otherwise.

The reader shows the PR's title (14/600), its meta line (state, author,
age, `core#4812 · 3f9c2a1`) and a `GitHub` link, then a 28px tab of
11px text per document: the Description, then each markdown file the PR
changes, specs first, named as in the TUI, each with its file's state as a
glyph (`✗`, `!`, `⋯`, `✓`, from its line-0 marks; a banner is `!`).

Markdown is `md::blocks()` (`src/markdown.hpp`): the TUI's md4c parse,
unwrapped, laid out GitHub-style in the type scale: H1 18/600 with a rule
under it, H2 15/600, the rest as Title 14/600; paragraphs at the reading
size (13/20); nested lists and task lists, quotes, code blocks (mono 11),
rules, tables. Up to 880px wide.

temoc's results, the way the canvas and temoc-ui show them, from the same
marks the TUI uses (`02-plugins.md`):

- **Banners** at the top, 28px: the glyph, the state in its colour
  (broken plugins and every plugin's `summary`, `reader_top`; stale
  marks, `results are for 86fa3ad`; each line-0 mark: running with its
  clock, passed, failed, `temoc failed` with the message in monospace
  under it), `Run file T` / `Run all specs T` on the right. The file's
  state is left out when a banner already says it couldn't run.
- **Spec tables**: a rounded border, 24px rows of 12px text, the header
  11/600 muted on the panel colour, temoc's expected-value columns in the
  accent colour and `# comment` columns muted; each row tinted by its
  result (passed a soft green, failed and error red, snoozed yellow,
  running blue; skipped and queued muted text) with the worst mark's
  glyph in a 16px gutter; an error's or warning's text in mono 11 under
  its row. Under the mouse, a test row's gutter is a `▶` that runs the
  row. A test table's title line is an 18px `▶` (run this table), its
  name (the heading above it, else its fixture) and the fixture in mono;
  under the table its result as an 18px pill (`✗ failed · 1 of 4`), then
  the table's own failures.
- Marks on other lines: their glyph in the gutter, an error's text under
  the line as `✗ line 13 · text`.

The panes around it:

- **Results**: a 22px line with the document and its counts by glyph
  (`✓ 5 ✗ 1 ! 1`), `Run file` and `Run all` on the right; a 24px row per
  test table (its glyph, name, `line N` and result word; a click focuses
  it and scrolls there); `What did not pass` (a row per failing row,
  table or file, in line order); on the description, a row per spec file;
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

`⌘,`/`Ctrl ,`, the menus, or the palette. A floating window over a dimmed
main window (a click outside closes it), 860×640: a 32px title bar (drag
it to move the window, a corner to resize it), the sections on the left
(24px rows), a 40px footer. Every change is written to config.json at
once through `config_write`, as the TUI's Settings do; a file anton can't
rewrite (comments, bad JSON) says so in the footer and the change stays
for the session.

- **General**: the start view (`start_view`), refresh every
  (`refresh_seconds`: 30s to 30 min), check watched PRs every
  (`watch_seconds`), install updates (`auto_update`), strip ticket prefixes
  from titles (`title_strip`, with a default pattern and an editable
  regex, and a before → after).
- **Appearance**: Dark / Light / Match system as cards drawn in each
  theme's colours; the code font (JetBrains Mono, or the system's
  monospace: SF Mono or Menlo on macOS, DejaVu Sans Mono on Linux,
  JetBrains Mono when there's none); hover tips (`tooltips`); the panes
  shown (Views, Preview, Results, Run log and Problems, Status bar);
  Reset layout.
- **PR list**, as the canvas's ListSettings: a live preview row (the
  selected PR, drawn by the list's own code with the settings as they
  are); the columns, each with its checkbox and what it shows, dragged to
  reorder (the title is always shown); segments for Author (Avatar circle,
  Name in colour), State (Circle and label, Coloured text, Pill) and Row
  height (Compact 24, Cozy 28, Roomy 34); steppers for the three text
  sizes (Interface, Reading, Code; 9–20px); and where Open in reader puts
  the PR (In a tab beside Preview, Where the last reader tab was, In its
  own window). The View menu has the same switches.
- **Views**: the views in order (drag a row's grip to reorder), each
  with Edit: name, what it shows (a GitHub search, or a plugin's list and
  which), its filter (a list view's must pass the local filter), sort,
  Duplicate, Delete (asks first). `New view` adds a saved one.
- **Plugins**: per plugin its dot, state, PR count and keys, `Restart`,
  `Open <file>` (its `--config`, else `llm.json` / `temoc.json` in the
  config dir for anton-llm / anton-temoc), its command, health text and
  last stderr line.

The footer: `Saved as you go to <config path>`; on Appearance and PR list
`Reset to defaults` (that page's settings), elsewhere `Open config file`;
`Done`. Files open in `$VISUAL`/`$EDITOR` when that's a graphical editor
(`code -w`), else with `open -t` on macOS and `xdg-open` on Linux: there's
no terminal to run vi in.

### The command palette

`⌘K`/`Ctrl K`, the search box, or `?`. A box over the dimmed window: type,
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

The panes are ImGui docking windows in one dock space. Drag a pane's tab
onto another pane's edge or center to dock it there, out of the window to
float it (an OS window of its own), drag the line between panes to resize.
Each node's `▾` lists its tabs.

The layout is saved per user in `$XDG_CONFIG_HOME/anton/gui-layout.ini`
(default `~/.config/anton/gui-layout.ini`; `gui_layout_path()`), never in
the current directory. With no layout file (or Reset Layout, in View or
Settings) the dock space is built with ImGui's DockBuilder to the canvas's
proportions, scaled with the interface size: Views 200px on the left,
Preview 520px on the right, Pull requests in between; the reader's node
and the one under it come when a PR is opened.

View (and Settings › Appearance) hides or shows Views, Preview, Results,
Run log (with Problems) and the status bar; they're saved in config.json.

## Sizes and the type scale

The canvas's metrics (`TypeScale.dc.html`), in px at the 12px interface
size and scaled with it (`k()`): toolbar 38, pane tab bar 28, list rows
24 · 28 · 34, buttons and inputs 24, chips 22, buttons inside a pane 20,
status bar 22; Views 200 wide, the right column 520.

Seven text roles; inside a row, weight and colour carry emphasis, not
size, and colour means state:

| role | size / line · weight | where |
| --- | --- | --- |
| Reader H1 | 18 / 24 · 600 | a markdown h1 in the reader, a rule under it |
| Reader H2 | 15 / 20 · 600 | a markdown h2 |
| Title | 14 / 20 · 600 | the PR's title in Preview and the reader; h3 and below |
| Reading | 13 / 20 · 400 | descriptions, spec prose, notes (`reading_size`) |
| Interface | 12 / 16 · 400–600 | rows, tabs, buttons (500), the open document's tab and the selected view (600) (`font_size`) |
| Meta | 11 / 16 · 400 | chips, ages, plugin status, muted; section labels at 600 |
| Code | mono 11 / 16 · 400 | numbers, diff counts, paths, logs (`code_size`); key hints at 10 |

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
(`01-anton.md`, Writing config.json), with any key it doesn't know left
where it is:

```json
"gui": {
  "theme": "system",
  "font_size": 12, "reading_size": 13, "code_size": 11,
  "code_font": "JetBrains Mono",
  "density": "compact",
  "columns": ["number", "repo", "size", {"name": "blocks", "hidden": true}, "state", "age", "author", "labels", "tags"],
  "author_style": "avatar",
  "state_style": "dot",
  "reader_placement": "beside_preview",
  "tooltips": true,
  "panes": { "views": true, "preview": true, "results": true, "run_log": true, "status_bar": true }
}
```

| key | default | |
| --- | --- | --- |
| `theme` | `"system"` | `dark`, `light` or `system` |
| `font_size` | `12` | the interface size in px, 9–20 (clamped); everything scales with it |
| `reading_size` | `13` | descriptions, spec prose and notes, 9–20 |
| `code_size` | `11` | mono: numbers, paths, logs, 9–20 |
| `code_font` | `"JetBrains Mono"` | or `"System monospace"` |
| `density` | `"compact"` | the list's rows: `compact` 24px, `cozy` 28, `roomy` 34 (`comfortable`, the old name, reads as `roomy`) |
| `columns` | as above | the list's columns in order: `number`, `repo`, `size`, `blocks` (the size as five squares), `state`, `age`, `author`, `labels`, `tags` (the plugins'); a name shows it, `{"name": ..., "hidden": true}` keeps its place hidden; a column left out goes last, hidden. The title is always shown, last |
| `author_style` | `"avatar"` | `avatar` (a circle and the name) or `color` (the name in a per-person colour) |
| `state_style` | `"dot"` | `dot` (a dot and the label), `text` (the label in the state's colour) or `pill` |
| `reader_placement` | `"beside_preview"` | where Open in reader puts the reader: `beside_preview`, `last` or `window` |
| `tooltips` | `true` | hover tips, the plugin actions' long labels among them |
| `panes` | all `true` | `views`, `preview`, `results`, `run_log`, `status_bar` |

Everything else (views, people, plugins, refresh) is the TUI's and means
the same; the TUI's `columns` stay the TUI's. A bad `gui` value doesn't
stop either UI from starting: the `gui` keys fall back to their defaults
and anton-gui says why in its status bar; the TUI ignores them.

## Keys

Mouse and keyboard both work. In the list, as the TUI: `j`/`k`, `↑`/`↓`,
`g`/`G`, Home/End, PgUp/PgDn move; `Enter` reads; `w` browser; `y` copy
URL; `r` refresh; `s` next sort; `/` the query; `f` + Filter; `x` clear
the org/repo scope; `S` save the view; `<` `>` move it; `+` new view; `n`
the LLM review; `1`–`9`, `Tab`/`Shift-Tab` views; `e` config.json; `,`
settings; `?` the palette; a plugin's key runs its action on the PR.

In the reader (while it, or a pane about its PR, has the focus):
`Esc`/`q` close it; `Tab`/`]` and `Shift-Tab`/`[` next and previous
document; `j`/`k`, `↑`/`↓`, `space`, PgUp/PgDn, `g`/`G` scroll; `J`/`K`
focus the next/previous test table and scroll to it; `r` runs the focused
one; `T` (a "line" action's key) runs the file on a spec tab, every spec
on the description; `w`/`Enter` browser; `y` copy; `n` the LLM review.

Anywhere, each a menu entry (`Mod` is `Cmd` on macOS, `Ctrl` elsewhere):
`Mod K` palette, `Mod ,` settings, `Mod W` close the reader, `Mod 1` /
`Mod 2` Views / Preview, `Shift Mod R` Results, `Mod J` Run log, `Mod +` /
`Mod −` / `Mod 0` the interface size, `Mod M` minimize, `Mod Q` quit, full
screen `Ctrl Cmd F` (`F11` on Linux). `Esc` closes the palette, then
Settings, then the reader.

## Screenshot mode

```sh
anton-gui --screenshot out.png --screen list|reader|settings|palette \
          [--theme dark|light] [--pr N] [--file F] [--size WxH] [--scale S] \
          [--palette-query Q] [--settings-page general|appearance|list|views|plugins] \
          [--press KEY [--wait SECONDS]] [--keys 'j j <Enter> J r']
```

It opens a hidden window, renders into an offscreen framebuffer at
`--size` (1440x900) times `--scale` (1–3), waits until the list has its
rows (then the selected PR's description; in the reader, its body and
files), sets the screen up (`--pr` selects that PR, switching to a view
that has it; `--screen reader` opens it in the reader, a tab beside
Preview; `--file` opens its tab by path, tab name or path tail;
`--press` sends a plugin's key, like `T`, and waits `--wait` seconds for
it; `--keys` types keys, a few frames apart: characters, or `<Enter>`,
`<Esc>`, `<Tab>`, `<Up>`, `<Down>`, `<Ctrl-K>`, `<Ctrl-,>`, so a key path
can be seen without a person at the keyboard), lets it settle a second and a half, writes the PNG (stb_image_write)
and exits. 30 seconds without the data, it captures what's there and says
so. It reads no layout file and writes neither the layout nor config.json,
so a screenshot is always the default layout; `--theme` holds for that
run only. Multi-viewports are off and, on macOS, the menu bar isn't
installed: the shot is the window alone. With the demo:

```sh
ANTON=build/anton-gui ./docs/demo/run.sh --screenshot shot.png --screen reader --pr 4812 --file specs/onboarding.md
```

CI (`test.yml`) shoots every screen on Linux under xvfb and keeps them as
an artifact, and builds anton-gui on macOS (its runner has no OpenGL to
shoot with). `--query`, `--no-plugins` and `--version` are the TUI's.

## Build

Part of anton's CMake project, on by default (`-DANTON_GUI=OFF` to skip):

- Dear ImGui, docking branch, `v1.92.9b-docking`, with its GLFW and
  OpenGL 3 backends and `imgui_stdlib` (FetchContent, URL and SHA-256).
- GLFW 3.4 (FetchContent), X11 only on Linux: Wayland sessions run it
  through XWayland. It needs the X11 headers (`xorg-dev`) to build; X11
  and OpenGL are loaded at run time.
- On macOS, Cocoa for the menu bar (`src/gui_menu_mac.mm`, Objective-C++
  compiled with ARC).
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
src/gui.hpp           the Gui state, the menus' entries (Cmd), its drawing helpers
src/gui.cpp           the frame: the menus (one table, both menu bars),
                      the toolbar, the dock space and its layout, the
                      reader's placement, status bar, keys, theme,
                      helpers, icons
src/gui_menu_mac.mm   macOS: NSMenus from the table
src/gui_list.cpp      Views, Pull requests (filter, chips, table), Preview
src/gui_reader.cpp    the reader, markdown with marks, Results, Outline,
                      LLM review, Run log, Problems
src/gui_settings.cpp  Settings and the command palette
src/gui_main.cpp      flags, the window and loop, fonts, --screenshot
src/gui_logic.hpp     pure: theme tokens, pills, diffstat, emoji, results
                      off marks, where marks land on blocks, table names,
                      the run log's diff, name colours, button labels
tests/gui_check.cpp   asserts over gui_logic.hpp, fuzzy_find, the "gui"
                      keys' parse, defaults and round trip (unknown keys
                      kept), the layout's path
```

## Not done

- The canvas's Preview tabs `Checks` and `Slack thread`: anton has no data
  for them (one rollup state; Slack threads stay in anton-slkx).
- Command columns are the TUI's only; the GUI's Columns menu greys them.
- Emoji in plugin text are dropped, not drawn.
- A dock node's tab labels are one weight: ImGui draws every tab of a
  node in the same font (the canvas has the active one at 600), and the
  bottom node's tabs are 28px like the others (the canvas's are 26 of
  11px text).
- Settings has no Reader or Keys page (the canvas's nav lists them; the
  reader's settings are on PR list).
- No config reload while running: edit config.json and restart.
