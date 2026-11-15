#pragma once
// The terminal UI's state and the pieces its three files share: ui.cpp (the
// PR list, menus, the palette, mouse dispatch), ui_reader.cpp (the reader
// and the note overlay), ui_settings.cpp (settings, $EDITOR). The data and
// logic it shares with anton-gui are in Core (core.hpp). Everything here
// runs on the UI thread unless it says otherwise.

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <ftxui/component/app.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>

#include "config.hpp"
#include "core.hpp"
#include "filters.hpp"
#include "markdown.hpp"
#include "model.hpp"
#include "plugins.hpp"

namespace anton {

// -- look ----------------------------------------------------------------------

// The design's colors. Truecolor terminals get them as is; others get the
// ANSI color each stands in for (foregrounds) or FTXUI's nearest palette
// color (backgrounds), and the selected row is inverted instead.
// Note is a mark's note (a quieter red); the Tints are row results' backgrounds.
enum class Pal { Fg, Dim, Faint, Cyan, Green, Yellow, Red, Magenta, Accent, Ink, Sel, Chip, ChipOn, Btn, Menu, MenuSel, Border,
                 Note, TintOk, TintErr, TintWarn };

inline bool truecolor() {
  static const bool t = ftxui::Terminal::ColorSupport() == ftxui::Terminal::TrueColor;
  return t;
}

inline ftxui::Color pal(Pal p) {
  using ftxui::Color;
  struct Entry {
    int hex;
    Color ansi;  // fallback; Default for backgrounds: they downsample
  };
  static const Entry table[] = {
      {0xd5d9e2, Color::Default}, {0x8a91a1, Color::GrayLight}, {0x3b4252, Color::GrayDark},
      {0x56c8e0, Color::Cyan},    {0x7cc98a, Color::Green},     {0xe3bd5f, Color::Yellow},
      {0xf2777a, Color::Red},     {0xc49bef, Color::Magenta},   {0x7aa7ff, Color::Blue},
      {0x0d1016, Color::Black},   {0x1c2638, Color::Default},   {0x1f2533, Color::Default},
      {0x12323b, Color::Default}, {0x232b3a, Color::Default},   {0x171c28, Color::Default},
      {0x25324a, Color::Default}, {0x4a5366, Color::GrayDark},  {0xc08a8c, Color::Red},
      {0x17291d, Color::Default}, {0x3a2225, Color::Default},   {0x332e19, Color::Default}};
  const Entry& e = table[static_cast<int>(p)];
  bool background = (p >= Pal::Sel && p <= Pal::MenuSel) || p >= Pal::TintOk;
  if (truecolor() || background) return Color::RGB(e.hex >> 16, (e.hex >> 8) & 0xff, e.hex & 0xff);
  return e.ansi;
}

inline ftxui::Element txt(const std::string& s, Pal p = Pal::Fg, bool b = false) {
  auto e = ftxui::text(s) | ftxui::color(pal(p));
  return b ? e | ftxui::bold : e;
}
inline ftxui::Decorator bg(Pal p) { return ftxui::bgcolor(pal(p)); }

// A button: " key label " on BTN, the key in accent. Either may be empty.
inline ftxui::Element button(const std::string& key, const std::string& label, Pal on = Pal::Btn,
                             Pal label_fg = Pal::Fg) {
  ftxui::Elements e = {ftxui::text(" ")};
  if (!key.empty()) e.push_back(txt(key + " ", Pal::Accent, true));
  if (!label.empty()) e.push_back(txt(label + " ", label_fg));
  return ftxui::hbox(std::move(e)) | bg(on);
}

inline ftxui::Element rule() { return ftxui::separator() | ftxui::color(pal(Pal::Faint)); }

// A run of text in a line that must not wrap: fit() lays them left to right
// and cuts at the width, the last one ending in "…".
struct Part {
  std::string s;
  Pal p = Pal::Fg;
  const Person* who = nullptr;  // painted in their color instead
};

// One line of text being edited: the query row, menu inputs, prompts.
struct LineEdit {
  std::string s;
  size_t cur = 0;  // byte offset, on a character boundary
  void set(std::string v) {
    s = std::move(v);
    cur = s.size();
  }
  bool key(const ftxui::Event& e);  // false: not an editing key
};

// A dropdown, the palette, or a prompt: rows to pick from, maybe a line to
// type in. A stack of them: "+ filter" opens its value menu beside it.
struct MenuItem {
  enum Kind { Normal, Header, Sep } kind = Normal;
  std::string label, hint;
  std::function<void()> run;  // empty on a Normal item: greyed out (hint says why)
  bool radio = false, on = false, danger = false, sub = false;  // sub: opens another menu
  bool selectable() const { return kind == Normal && run; }
};

struct Menu {
  std::string title, anchor;  // anchor: the chip or tab it hangs from, drawn highlighted
  int x = -1, y = 2, w = 40;  // x -1: centered
  bool input = false;
  LineEdit edit;
  std::string placeholder, error;
  std::function<std::vector<MenuItem>()> items;
  // ⏎ when no row is picked: takes the typed text. "" closes; else the error stays shown.
  std::function<std::string(const std::string&)> submit;
  std::vector<std::string> foot;
  int sel = -1, top = 0;
};

// A clickable region of the last frame: reflect() fills box as it's drawn.
struct Hit {
  ftxui::Box box{-1, -1, -1, -1};  // stays off screen if it's never drawn
  std::function<void()> click, right, dbl, hover;
  std::function<void(int)> wheel;  // +1 down, -1 up
  std::function<void(int)> drop;   // drag-to-reorder: a row of group `drag` was dragged onto this one
  int drag = -1, index = -1;
  bool backdrop = false;  // under an open menu: a click closes it and goes through
};

enum class Mode { Normal, Query, Note, Reader, Settings };

struct Ui : Core {
  ftxui::App screen = ftxui::App::Fullscreen();

  // -- screen state --------------------------------------------------------------
  Mode mode = Mode::Normal;
  int selected = 0, list_top = 0, list_rows = 1;
  bool chips_folded = false;  // the filter bar is one "filters N" chip (narrow)
  LineEdit qedit;                    // the query row, while editing
  int comp_sel = 0;                  // the completion picked
  std::vector<std::string> history;  // queries applied this session
  int hist_pos = -1;
  std::vector<Menu> menus;
  std::deque<Hit> hits;  // rebuilt every frame; deque: reflect keeps pointers
  std::map<std::string, ftxui::Box> anchors;  // chips and tabs, for menus to hang from
  std::chrono::steady_clock::time_point last_click{};
  int last_click_x = -1, last_click_y = -1, drag_group = -1, drag_index = -1;
  std::string note_url;  // the PR whose notes the Note overlay shows
  int note_scroll = 0;

  // -- reader --------------------------------------------------------------------
  std::map<std::string, int> reader_scroll;  // per tab: by path, "" for the description
  struct Rendered {  // markdown for one width; redone when text or width change
    std::string key;
    int width = -1;
    std::vector<md::Line> lines;
  };
  Rendered reader_md, note_md;
  int reader_hover = -1;                    // the display line of a test row under the mouse: its ▶ shows
  std::map<std::string, int> reader_focus;  // per file: the focused test table's directive line (J/K, r)

  // -- settings ------------------------------------------------------------------
  int set_page = 0, set_focus = 0, set_scroll = 0;
  size_t set_view = 0;  // the view the Views page edits
  struct Control {
    std::function<void()> act;
    std::function<void(int)> adj;   // -1/+1: h/l, -/+
    std::function<void(int)> move;  // J/K on a row of a reorderable list
    int row = 0;                    // in the page, to scroll it into view
  };
  std::vector<Control> controls;  // the page's, in focus order, rebuilt every frame

  // -- command columns ---------------------------------------------------------------
  struct ColumnCell {
    std::string text, error;
    std::time_t updated = 0;  // the PR's updatedAt it ran for
    int gen = -1;             // the refresh it ran in
    bool running = false, done = false;
  };
  static constexpr int kColumnJobs = 4;
  std::map<std::pair<std::string, std::string>, ColumnCell> column_cells;  // (column name, URL)
  bool column_workers = false;
  std::mutex col_mu;
  std::condition_variable col_cv;
  std::deque<std::function<void()>> col_jobs;

  explicit Ui(Config c);
  void post(std::function<void()> fn) override;
  void reset_selection() override { selected = list_top = 0; }

  // ui.cpp: people
  static ftxui::Element paint(ftxui::Element e, const Person* p);
  static ftxui::Element fit(const std::vector<Part>& parts, int w);

  // ui.cpp: views that ask first
  void save_view(size_t i, bool named = false);  // named: skip asking a never-saved view its name
  void save_as_prompt(size_t i);
  void new_view();

  // ui.cpp: hits and menus
  Hit& hit(ftxui::Element& e);
  ftxui::Element clickable(ftxui::Element e, std::function<void()> f);
  void close_menus() { menus.clear(); }
  void push_menu(Menu m);
  void open_chip_menu(const Chip& c);
  void open_add_filter();
  void open_add_value(const AddKind& k, int x);
  void open_sort_menu();
  void open_list_menu();
  void open_folded_menu();
  void open_tab_menu(size_t i);
  void open_palette();
  void open_picker(bool repo);
  void prompt(const std::string& title, const std::string& initial, const std::string& hint,
              std::function<std::string(const std::string&)> accept);
  void confirm(const std::string& title, const std::string& yes, std::function<void()> fn);
  std::vector<MenuItem> commands(const Pr* cur);
  // pick/hover: what a click on row i does; default: run the item of menus[depth].
  ftxui::Element render_menu(Menu& m, size_t depth, std::function<void(int)> pick = {},
                             std::function<void(int)> hover = {});
  bool menu_key(const ftxui::Event& e);
  void run_item(size_t depth, int idx);

  // ui.cpp: the PR list
  ftxui::Element render();
  ftxui::Element render_main(const std::vector<Pr>& rs, const std::string& view_err);
  ftxui::Element render_tabbar(int width, bool narrow);
  ftxui::Element render_filterbar(int width, bool narrow);
  ftxui::Element render_queryrow(int width);
  ftxui::Element render_table(const std::vector<Pr>& rs, int height, bool live);
  ftxui::Element state_cell(const Shown& s);
  ftxui::Element tags_cell(const std::vector<Tag>& tags, int w);
  ftxui::Element author_cell(const std::string& login);
  ftxui::Element command_cell(const Column& col, const Pr& p);
  void run_column(const Column& col, const Pr& p, ColumnCell& cc);
  void column_worker();
  void start_column_workers();
  ftxui::Element render_detail(const Pr& p, int width);
  ftxui::Element render_buttons(const Pr* cur, int width);
  ftxui::Element render_message(const std::string& view_err, int width);
  ftxui::Element render_empty(int width);
  ftxui::Element colored_query(const std::string& q, int cursor = -1);
  bool on_event(ftxui::Event e);
  bool on_mouse(ftxui::Mouse m);
  bool normal_key(const ftxui::Event& e);
  bool query_key(const ftxui::Event& e);
  void start_query();
  void act(const std::string& key, const Pr* cur);  // a list key, from a key press or a button

  // ui_reader.cpp
  void open_reader(const Pr& p);
  int reader_rows() const;
  void reader_jump(const std::string& path, int src);  // scrolls the tab to source line src
  void focus_table(int by);  // J/K: the next or previous test table
  ftxui::Element render_reader();
  ftxui::Element render_note();
  bool reader_key(const ftxui::Event& e);
  bool note_key(const ftxui::Event& e);

  // ui_settings.cpp
  void open_settings();
  ftxui::Element render_settings();
  bool settings_key(const ftxui::Event& e);
  int control(std::function<void()> act, std::function<void(int)> adj, int row);
  void edit_config();
  void apply_config(Config c);
};

// Markdown spans and lines as elements (ui_reader.cpp), and a page of them.
ftxui::Element md_line(const md::Line& l);
bool scroll_key(const ftxui::Event& e, int& scroll, int rows);
bool fuzzy(const std::string& hay, const std::string& needle);

}  // namespace anton
