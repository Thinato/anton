#pragma once
// anton-gui (06-gui.md): the desktop UI, Dear ImGui on GLFW and OpenGL 3.
// Gui derives from Core, the logic it shares with the TUI, and draws it:
//   gui.cpp           the frame: the menus (one table for both menu bars),
//                     the toolbar, dock spaces and their default layout,
//                     the status bar, keys, theme, drawing helpers
//   gui_menu_mac.mm   the macOS menu bar, NSMenus built from that table
//   gui_list.cpp      the PR list's panes: Views, Pull requests, Preview
//   gui_reader.cpp    the reader: a PR's documents with temoc results,
//                     Results, Outline, LLM review, Run log, Problems
//   gui_settings.cpp  the Settings window and the command palette
//   gui_main.cpp      flags, the window and loop, --screenshot
// Everything runs on the UI thread; Core's workers post() back to it.

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "core.hpp"
#include "gui_logic.hpp"

struct GLFWwindow;

namespace anton::gui {

struct Fonts {
  ImFont* regular = nullptr;   // Inter 400
  ImFont* medium = nullptr;    // Inter 500
  ImFont* semibold = nullptr;  // Inter 600
  ImFont* mono = nullptr;      // JetBrains Mono (or the system's monospace)
};

enum class Screen { List, Reader };
enum class Btn { Default, Primary, Ghost };

// One entry of the menus. The macOS menu bar (gui_menu_mac.mm) and the
// in-window one (Linux) are both built from Gui::menus(), and keys() runs
// an entry whose key has Mod in it: one code path per command.
struct Cmd {
  std::string label = "";     // "" = a separator
  std::string key;            // "Mod+K", "Shift+Mod+R", "Ctrl+Mod+F" (Mod: Cmd on macOS, Ctrl elsewhere),
                              // "F11"; or a plain key, only shown: "H", "Enter"
  std::function<void()> run = nullptr;  // none: a submenu (sub)
  bool enabled = true;
  int check = -1;             // -1: no check mark; 0 or 1
  std::vector<Cmd> sub = {};
  bool passthrough = false;   // its key is ImGui's own (Cut, Copy, ...): keys() leaves it be
};
struct Menu {
  std::string title;
  std::vector<Cmd> items;
};

// One of the reader's documents: the description (path "") or a file.
struct Doc {
  std::string path, name;  // name: the tab's
  const std::string* text = nullptr;
  std::string error;
  bool spec = false, loading = false;
};

struct Gui : Core {
  explicit Gui(Config c);

  GLFWwindow* window = nullptr;
  Fonts f;
  Theme t = make_theme(true);
  bool os_dark = true;          // the OS appearance, for "system"
  bool screenshot = false;      // --screenshot: no layout file, nothing saved
  std::string start_screen;     // --screen
  std::string loaded_code_font; // the code font the atlas has

  // -- posting from workers --------------------------------------------------------
  std::mutex post_mu;
  std::vector<std::function<void()>> posted;
  void post(std::function<void()> fn) override;
  void drain();  // runs what workers posted; UI thread

  // -- hooks into Core -------------------------------------------------------------
  void reset_selection() override;
  void action_sent(size_t i, const nlohmann::json& msg, bool ok) override;
  void on_plugin_line(size_t i, const std::string& line, bool is_err) override;
  void on_plugin_exit(size_t i, int code) override;

  // -- screen state ------------------------------------------------------------------
  Screen screen = Screen::List;
  std::string sel_url;  // the selected PR; kept across refreshes
  bool scroll_to_sel = false;
  std::string qbuf;     // the filter input
  bool q_dirty = false; // typed but not applied
  bool focus_query = false;
  bool settings_open = false, settings_shown = false;
  int settings_page = 0;
  int settings_view = -1;  // the view being edited in Settings › Views
  bool palette_open = false, palette_focus = false;
  std::string palette_q;
  int palette_sel = 0;
  bool reset_main = false, reset_reader = false;  // rebuild a dock space's default layout
  int pick_tabs = 0;  // frames left to select Results and Run log in their nodes, after a rebuild
  std::string rate_limit;                         // "4,812 / 5,000"
  std::time_t last_rate = 0;
  std::time_t last_os_check = 0;
  bool open_add_filter = false;  // f: the "+ Filter" menu opens
  // A view's name asked for: rename, save as new, or a never-saved view's save.
  enum class Ask { None, Rename, SaveAs, Save } ask = Ask::None;
  size_t ask_view = 0;
  std::string ask_buf, ask_err;
  int confirm_delete = -1;  // the view a "Delete?" asks about
  void ask_name(Ask what, size_t view);
  void name_modal();
  void view_menu(size_t i);  // a view's context menu items
  void modals();

  // -- the reader ----------------------------------------------------------------------
  std::string jump_path;  // scroll this doc to source line jump_src on the next frame
  int jump_src = -1;
  std::map<std::string, int> focus;  // per file: the focused test table's directive line
  float scroll_by = 0;  // j/k, space, g/G: the open document scrolls by this (pages when |x| >= 1e6)
  std::deque<LogEntry> runlog;
  bool focus_llm = false;

  // -- frame ---------------------------------------------------------------------------
  void frame();  // the whole UI, between ImGui::NewFrame and Render
  void apply_theme();
  std::vector<Menu> menus();  // the menu bar's menus, as things are now
  void menu_bar();            // the in-window one (not on macOS)
  void toolbar();
  void quit();
#ifndef __APPLE__
  void toggle_fullscreen();  // F11
#endif
  void zoom(int by);          // the interface size; 0: back to the default
  void send_key(int key);     // an ImGuiKey with Mod held, to ImGui (Edit's Cut, Copy, Paste, Select All)
  const Pr* current_pr() const;  // the reader's PR while it has the focus, else the list's
  void status_bar();
  void dock_spaces();
  void build_main_layout(ImGuiID id, ImVec2 size);
  void build_reader_layout(ImGuiID id, ImVec2 size);
  void keys();
  void check_os_appearance();
  void poll_rate_limit();
  void set_theme(const std::string& pref);
  void save(const std::string& done = "");  // config.json, gui keys included
  void open_file(const std::string& path);  // $VISUAL/$EDITOR if graphical, else the OS's opener
  void open_config();
  float k() const { return cfg.gui.font_size / 13.f; }  // the interface size, relative to the canvas's 13px

  // -- the PR list (gui_list.cpp) ---------------------------------------------------
  std::vector<Pr> cur_rows;  // the active view's rows this frame
  std::string view_err;
  const Pr* selected_pr() const;
  void select(int by);  // j/k
  void views_pane();
  void prs_pane();
  void preview_pane();
  void filter_bar();
  void pr_table();
  void open_reader(const Pr& p);
  void plugins_box(const Pr& p, bool compact);
  void act(const std::string& key);  // a list key, from a key, a button or the palette

  // -- the reader (gui_reader.cpp) ---------------------------------------------------
  std::vector<Doc> docs() const;  // the description, then the files
  void reader_pane();
  void results_pane();
  void outline_pane();
  void llm_pane();
  void runlog_pane();
  void problems_pane();
  // Markdown, GitHub-style; on a file (marks), with every plugin's marks laid on it.
  void markdown(const std::string& text, const std::string& path, bool spec, bool marks, bool compact = false);
  void reader_header(const Pr& p);
  void reader_tabs(const std::vector<Doc>& ds);
  void banner(const Mark& m, const std::string& path, bool spec);
  void focus_table(int by);
  void log(LogEntry e);
  void jump(const std::string& path, int src);  // to source line src of a doc, its tab opened
  // Spans laid out word by word in width (size: canvas px), from the cursor;
  // the height used. measure: lay out only.
  float rich(const std::vector<md::Span>& spans, float width, float size, uint32_t color, bool measure = false);
  float rich_width(const std::vector<md::Span>& spans, float size);
  std::map<size_t, std::vector<md::Block>> parsed;  // by the text's hash
  const std::vector<md::Block>& blocks_of(const std::string& text);

  // -- settings and palette (gui_settings.cpp) ------------------------------------
  void settings_window();
  void palette();

  // -- drawing helpers (gui.cpp) ---------------------------------------------------------
  ImU32 col(uint32_t rgba) const;
  ImVec4 vec(uint32_t rgba) const;
  uint32_t tone_fg(Tone tn) const;
  uint32_t tone_bg(Tone tn) const;
  void text(const std::string& s, uint32_t color, ImFont* font = nullptr, float size = 0);
  void text_wrapped(const std::string& s, uint32_t color, ImFont* font = nullptr, float size = 0, float wrap = 0);
  void pill(const std::string& s, Tone tn, float h = 22, const char* glyph = nullptr, bool dot = true);
  void avatar(const std::string& login, float size = 20);
  void diffstat_blocks(long add, long del);
  using Icon = void (*)(ImDrawList*, ImVec2, float, ImU32);
  bool button(const std::string& label, Btn kind = Btn::Default, float h = 28, const std::string& key = "",
              float w = 0, Icon icon = nullptr);
  float button_width(const std::string& label, Btn kind = Btn::Default, const std::string& key = "", Icon icon = nullptr);
  bool icon_button(const char* id, void (*icon)(ImDrawList*, ImVec2, float, ImU32), float size = 28,
                   bool bordered = true, const char* tip = nullptr);
  void section(const std::string& title);  // a muted 12px label
  bool link_row(const std::string& id, float h);  // a full-width hover row; true on click
  void segments(const std::vector<std::pair<std::string, uint32_t>>& parts, ImFont* font = nullptr, float size = 0);
};

// Line icons, drawn at a position in a square of side s.
void icon_logo(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_search(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_refresh(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_sun(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_moon(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_external(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_copy(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_play(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_plus(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_filter(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_columns(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_cross(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_chevron(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_back(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_sliders(ImDrawList* d, ImVec2 p, float s, ImU32 c);
void icon_grip(ImDrawList* d, ImVec2 p, float s, ImU32 c);

// ImGui's text input on a std::string.
bool input(const char* id, std::string& s, ImGuiInputTextFlags flags = 0, const char* hint = nullptr,
           ImGuiInputTextCallback cb = nullptr, void* user = nullptr);

std::string tilde(const std::string& path);
std::string short_repo(const std::string& repo);
std::string sort_label(Sort s);  // "Recently updated"

#ifdef __APPLE__
// gui_menu_mac.mm
void mac_install_menu(Gui* g);  // the menu bar, from g->menus()
void mac_about();
void mac_hide();
void mac_toggle_fullscreen(GLFWwindow* w);
bool mac_fullscreen(GLFWwindow* w);
#endif

}  // namespace anton::gui
