// The frame: menu bar, dock spaces and their default layout, status bar,
// keys, theme; and the drawing helpers the panes share. See gui.hpp.
#include "gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <thread>

#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "config_write.hpp"
#include "proc.hpp"
#include "update.hpp"

namespace anton::gui {

std::string tilde(const std::string& path) {
  const char* home = std::getenv("HOME");
  return home && *home && path.starts_with(home) ? "~" + path.substr(std::strlen(home)) : path;
}

std::string short_repo(const std::string& repo) { return repo.substr(repo.find('/') + 1); }

ImU32 g_icon_bg = 0;  // what icon_moon cuts its crescent with: the button under it

bool input(const char* id, std::string& s, ImGuiInputTextFlags flags, const char* hint, ImGuiInputTextCallback cb,
           void* user) {
  return hint ? ImGui::InputTextWithHint(id, hint, &s, flags, cb, user) : ImGui::InputText(id, &s, flags, cb, user);
}

Gui::Gui(Config c) : Core(std::move(c)) {
  self_name = "anton-gui";
}

// -- posting -----------------------------------------------------------------------

void Gui::post(std::function<void()> fn) {
  {
    std::lock_guard lk(post_mu);
    posted.push_back(std::move(fn));
  }
  glfwPostEmptyEvent();
}

void Gui::drain() {
  std::vector<std::function<void()>> todo;
  {
    std::lock_guard lk(post_mu);
    todo.swap(posted);
  }
  for (auto& fn : todo) fn();
}

void Gui::reset_selection() {
  sel_url.clear();
  scroll_to_sel = true;
}

// -- the run log: plugins' progress as it streams -------------------------------

void Gui::log(LogEntry e) {
  if (!e.at) e.at = now();
  runlog.push_back(std::move(e));
  while (runlog.size() > 2000) runlog.pop_front();
}

void Gui::on_plugin_line(size_t i, const std::string& line, bool is_err) {
  const std::string& name = cfg.plugins[i].name;
  if (is_err) {
    log({0, name, "", "stderr", line, "warn"});
    return Core::on_plugin_line(i, line, is_err);
  }
  std::string url, health = live[i].health;
  if (auto j = nlohmann::json::parse(line, nullptr, false); j.is_object()) url = normalize_url(str(j, "pr"));
  PluginState& st = one_shot(i) ? pending[i] : live[i];
  std::optional<PrInfo> before;
  if (auto it = st.prs.find(url); !url.empty() && it != st.prs.end()) before = it->second;
  Core::on_plugin_line(i, line, is_err);
  if (live[i].health != health && !live[i].health.empty())
    log({0, name, "", "health", live[i].health + (live[i].health_text.empty() ? "" : ": " + live[i].health_text),
         live[i].health == "ok" ? "ok" : "error"});
  if (url.empty()) return;
  PluginState& now_st = one_shot(i) ? pending[i] : live[i];
  auto it = now_st.prs.find(url);
  if (it == now_st.prs.end()) return;
  for (auto& e : log_diff(before ? &*before : nullptr, it->second)) {
    e.plugin = name;
    e.url = url;
    log(std::move(e));
  }
}

void Gui::on_plugin_exit(size_t i, int code) {
  Core::on_plugin_exit(i, code);
  if (!one_shot(i)) log({0, cfg.plugins[i].name, "", "exit", plugin_problem(i), "error"});
}

void Gui::action_sent(size_t i, const nlohmann::json& msg, bool ok) {
  std::string what = str(msg, "action");
  if (msg.contains("file")) what += " " + str(msg, "file");
  if (msg.contains("line")) what += ":" + msg["line"].dump();
  log({0, cfg.plugins[i].name, str(msg, "pr"), "action", ok ? what : what + " (not running)", ok ? "dim" : "error"});
}

// -- theme -----------------------------------------------------------------------------

ImU32 Gui::col(uint32_t c) const { return IM_COL32(c >> 24, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff); }

ImVec4 Gui::vec(uint32_t c) const {
  return ImVec4((c >> 24) / 255.f, ((c >> 16) & 0xff) / 255.f, ((c >> 8) & 0xff) / 255.f, (c & 0xff) / 255.f);
}

uint32_t Gui::tone_fg(Tone tn) const {
  switch (tn) {
    case Tone::Accent: return t.accent;
    case Tone::Success: return t.success;
    case Tone::Danger: return t.danger;
    case Tone::Attention: return t.attention;
    case Tone::Done: return t.done;
    case Tone::Neutral: return t.neutral;
  }
  return t.fg;
}

uint32_t Gui::tone_bg(Tone tn) const {
  switch (tn) {
    case Tone::Accent: return t.accent_muted;
    case Tone::Success: return t.success_muted;
    case Tone::Danger: return t.danger_muted;
    case Tone::Attention: return t.attention_muted;
    case Tone::Done: return t.done_muted;
    case Tone::Neutral: return t.neutral_muted;
  }
  return t.neutral_muted;
}

void Gui::apply_theme() {
  t = make_theme(resolve_dark(cfg.gui.theme, os_dark));
  ImGuiStyle& s = ImGui::GetStyle();
  s.FontSizeBase = static_cast<float>(cfg.gui.font_size);
  s.WindowPadding = ImVec2(0, 0);
  s.FramePadding = ImVec2(12 * k(), 8 * k());  // a pane's tab bar: 12px text on 28px
  s.ItemSpacing = ImVec2(8 * k(), 6 * k());
  s.ItemInnerSpacing = ImVec2(6 * k(), 4 * k());
  s.CellPadding = ImVec2(8 * k(), 0);
  s.WindowRounding = 0;  // a floating pane may be an OS window of its own: square, as the OS draws it
  s.ChildRounding = 6;
  s.FrameRounding = 6;
  s.PopupRounding = 8;
  s.GrabRounding = 6;
  s.GrabMinSize = 12 * k();
  s.ScrollbarRounding = 6;
  s.ScrollbarSize = 12 * k();
  s.TabRounding = 0;
  s.TabBorderSize = 0;
  s.TabBarBorderSize = 1;
  s.TabBarOverlineSize = 2;
  s.WindowBorderSize = 1;
  s.PopupBorderSize = 1;
  s.FrameBorderSize = 1;
  s.ChildBorderSize = 1;
  s.DockingSeparatorSize = 1;
  s.WindowMenuButtonPosition = ImGuiDir_Right;
  s.WindowTitleAlign = ImVec2(0, 0.5f);
  s.SeparatorTextBorderSize = 1;
  ImVec4* c = s.Colors;
  auto v = [&](uint32_t x) { return vec(x); };
  c[ImGuiCol_Text] = v(t.fg);
  c[ImGuiCol_TextDisabled] = v(t.muted);
  c[ImGuiCol_WindowBg] = v(t.bg);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = v(t.dark ? t.panel : t.bg);
  c[ImGuiCol_Border] = v(t.border);
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = v(t.input_bg);
  c[ImGuiCol_FrameBgHovered] = v(t.input_bg);
  c[ImGuiCol_FrameBgActive] = v(t.input_bg);
  c[ImGuiCol_TitleBg] = v(t.inset);
  c[ImGuiCol_TitleBgActive] = v(t.inset);
  c[ImGuiCol_TitleBgCollapsed] = v(t.inset);
  c[ImGuiCol_MenuBarBg] = v(t.panel);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = v(t.border);
  c[ImGuiCol_ScrollbarGrabHovered] = v(t.muted);
  c[ImGuiCol_ScrollbarGrabActive] = v(t.muted);
  c[ImGuiCol_CheckMark] = v(t.dark ? 0xffffffffu : 0xffffffffu);
  c[ImGuiCol_SliderGrab] = v(t.accent);
  c[ImGuiCol_SliderGrabActive] = v(t.accent);
  c[ImGuiCol_Button] = v(t.btn);
  c[ImGuiCol_ButtonHovered] = v(t.dark ? t.border_muted : t.inset);
  c[ImGuiCol_ButtonActive] = v(t.dark ? t.border_muted : t.inset);
  c[ImGuiCol_Header] = v(t.sel);
  c[ImGuiCol_HeaderHovered] = v(t.dark ? t.border_muted : t.inset);
  c[ImGuiCol_HeaderActive] = v(t.sel);
  c[ImGuiCol_Separator] = v(t.border);
  c[ImGuiCol_SeparatorHovered] = v(t.accent);
  c[ImGuiCol_SeparatorActive] = v(t.accent);
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = v(t.accent);
  c[ImGuiCol_ResizeGripActive] = v(t.accent);
  c[ImGuiCol_InputTextCursor] = v(t.fg);
  c[ImGuiCol_Tab] = v(t.inset);
  c[ImGuiCol_TabHovered] = v(t.panel);
  c[ImGuiCol_TabSelected] = v(t.bg);
  c[ImGuiCol_TabSelectedOverline] = v(t.tab_line);
  c[ImGuiCol_TabDimmed] = v(t.inset);
  c[ImGuiCol_TabDimmedSelected] = v(t.bg);
  c[ImGuiCol_TabDimmedSelectedOverline] = v(t.tab_line);
  c[ImGuiCol_DockingPreview] = v(t.accent_muted);
  c[ImGuiCol_DockingEmptyBg] = v(t.bg);
  c[ImGuiCol_TableHeaderBg] = v(t.panel);
  c[ImGuiCol_TableBorderStrong] = v(t.border);
  c[ImGuiCol_TableBorderLight] = v(t.border_muted);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TextSelectedBg] = v(t.accent_muted);
  c[ImGuiCol_DragDropTarget] = v(t.accent);
  c[ImGuiCol_NavCursor] = v(t.accent);
  c[ImGuiCol_ModalWindowDimBg] = v(t.scrim);
  c[ImGuiCol_PlotHistogram] = v(t.accent);
}

void Gui::set_theme(const std::string& pref) {
  cfg.gui.theme = pref;
  save();
}

// The OS appearance, off the UI thread every few seconds while the theme
// follows it: macOS's AppleInterfaceStyle; on Linux GNOME's color-scheme,
// else a GTK theme named dark, else dark.
void Gui::check_os_appearance() {
  if (cfg.gui.theme != "system" || now() - last_os_check < 5) return;
  last_os_check = now();
  std::thread([this] {
#ifdef __APPLE__
    bool dark = run({"defaults", "read", "-g", "AppleInterfaceStyle"}).out.starts_with("Dark");
#else
    anton::Result r = run({"gsettings", "get", "org.gnome.desktop.interface", "color-scheme"});
    bool dark = true;
    if (r.code == 0 && r.out.find("prefer-light") != std::string::npos) dark = false;
    else if (r.code == 0 && r.out.find("default") != std::string::npos) {
      anton::Result g = run({"gsettings", "get", "org.gnome.desktop.interface", "gtk-theme"});
      dark = g.code != 0 || lower(g.out).find("dark") != std::string::npos;
    } else if (r.code != 0) {
      const char* gtk = std::getenv("GTK_THEME");
      dark = !gtk || lower(gtk).find("light") == std::string::npos;
    }
#endif
    post([this, dark] { os_dark = dark; });
  }).detach();
}

// GitHub's GraphQL budget (anton's calls are GraphQL), every minute.
void Gui::poll_rate_limit() {
  if (now() - last_rate < 60) return;
  last_rate = now();
  std::thread([this] {
    anton::Result r = run({"gh", "api", "rate_limit"});
    auto j = nlohmann::json::parse(r.out, nullptr, false);
    std::string s;
    if (r.code == 0 && j.is_object()) {
      auto g = j.value("resources", nlohmann::json::object()).value("graphql", nlohmann::json::object());
      auto commas = [](long n) {
        std::string d = std::to_string(n);
        for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3) d.insert(static_cast<size_t>(i), ",");
        return d;
      };
      if (g.contains("limit")) s = commas(g.value("remaining", 0L)) + " / " + commas(g.value("limit", 0L));
    }
    post([this, s] { rate_limit = s; });
  }).detach();
}

void Gui::save(const std::string& done) {
  if (screenshot) return;  // --screenshot never writes config.json
  write_config(done);
}

// A file in an editor: $VISUAL or $EDITOR when it's a graphical one (there's
// no terminal to run vi in), else the OS's: `open -t` on macOS, xdg-open.
void Gui::open_file(const std::string& path) {
  static const std::set<std::string> terminal = {"vi",   "vim", "nvim", "nano", "emacs", "micro", "hx",
                                                 "helix", "kak", "ne",   "joe",  "mg",    "ed",    "pico"};
  std::vector<std::string> argv;
  for (const char* var : {"VISUAL", "EDITOR"})
    if (const char* e = std::getenv(var); e && *e && argv.empty()) {
      auto words = tokenize(e);
      std::string prog = std::filesystem::path(words[0]).filename().string();
      if (!terminal.count(prog)) argv = words;
    }
  if (argv.empty()) {
#ifdef __APPLE__
    argv = {"open", "-t"};
#else
    argv = {"xdg-open"};
#endif
  }
  argv.push_back(path);
  std::thread([this, argv] {
    anton::Result r = run(argv);
    if (r.code != 0) post([this, argv, r] { flash(argv[0] + ": " + (r.err.empty() ? "couldn't open it" : first_line(r.err)), 6, true); });
  }).detach();
  flash("opening " + tilde(path));
}

void Gui::open_config() {
  std::string path = config_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    save_config(cfg);
  }
  open_file(path);
}

// -- drawing helpers -----------------------------------------------------------------

void Gui::text(const std::string& s, uint32_t color, ImFont* font, float size) {
  ImGui::PushFont(font, size ? size * k() : 0.f);
  ImGui::PushStyleColor(ImGuiCol_Text, col(color));
  ImGui::TextUnformatted(s.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void Gui::text_wrapped(const std::string& s, uint32_t color, ImFont* font, float size, float wrap) {
  ImGui::PushFont(font, size ? size * k() : 0.f);
  ImGui::PushStyleColor(ImGuiCol_Text, col(color));
  ImGui::PushTextWrapPos(wrap > 0 ? ImGui::GetCursorPosX() + wrap : 0.f);
  ImGui::TextUnformatted(s.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();
}

void Gui::segments(const std::vector<std::pair<std::string, uint32_t>>& parts, ImFont* font, float size) {
  ImGui::PushFont(font, size ? size * k() : 0.f);
  bool first = true;
  for (auto& [s, c] : parts) {
    if (s.empty()) continue;
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    ImGui::PushStyleColor(ImGuiCol_Text, col(c));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::PopFont();
}

// A rounded pill, 18px: a dot (or a glyph) and 11px text, in a tone.
void Gui::pill(const std::string& s, Tone tn, float h, const char* glyph, bool dot) {
  float H = h * k(), pad = 8 * k(), gap = 6 * k(), fs = 11 * k();
  ImVec2 ts = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str());
  float lead = 0;
  ImVec2 gs;
  if (glyph) {
    gs = f.mono->CalcTextSizeA(fs, FLT_MAX, 0, glyph);
    lead = gs.x + 5 * k();
  } else if (dot) {
    lead = 6 * k() + gap;
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 size(pad * 2 + lead + ts.x, H);
  ImDrawList* d = ImGui::GetWindowDrawList();
  d->AddRectFilled(p, p + size, col(tone_bg(tn)), H / 2);
  float x = p.x + pad;
  if (glyph) d->AddText(f.mono, fs, ImVec2(x, p.y + (H - gs.y) / 2), col(tone_fg(tn)), glyph);
  else if (dot) d->AddCircleFilled(ImVec2(x + 3 * k(), p.y + H / 2), 3 * k(), col(tone_fg(tn)));
  d->AddText(f.regular, fs, ImVec2(x + lead, p.y + (H - ts.y) / 2), col(tone_fg(tn)), s.c_str());
  ImGui::Dummy(size);
}

// A PR's state as the canvas's rows and meta lines draw it: a 6px dot in
// its tone and 11px text.
void Gui::state_text(const Pill& pl, uint32_t text_color, float h) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float fs = 11 * k(), H = h * k();
  std::string s = strip_emoji(pl.text);
  ImVec2 ts = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str());
  d->AddCircleFilled(ImVec2(p.x + 3 * k(), p.y + H / 2), 3 * k(), col(tone_fg(pl.tone)));
  d->AddText(f.regular, fs, ImVec2(p.x + 11 * k(), p.y + (H - ts.y) / 2), col(text_color ? text_color : tone_fg(pl.tone)), s.c_str());
  ImGui::Dummy(ImVec2(11 * k() + ts.x, H));
}

// A PR's state as gui.state_style says: a dot and the label (dot), the
// label in the state's colour (text), or a pill (pill). h: the line, px.
void Gui::state_cell(const Pill& pl, float h) {
  if (cfg.gui.state_style == "pill") {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - 18) * k() / 2);
    pill(strip_emoji(pl.text), pl.tone);
  } else if (cfg.gui.state_style == "text") {
    std::string s = strip_emoji(pl.text);
    ImVec2 ts = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, s.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(f.regular, 11 * k(), ImVec2(p.x, p.y + (h * k() - ts.y) / 2), col(tone_fg(pl.tone)), s.c_str());
    ImGui::Dummy(ImVec2(ts.x, h * k()));
  } else {
    state_text(pl, t.fg2, h);
  }
}

// An author as gui.author_style says: the avatar and the name (avatar), or
// the name alone in weight 500 and the person's colour (color), clipped to
// width; h: the line, px. Returns the width drawn.
float Gui::author_cell(const std::string& login, float width, float h, float fs) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float H = h * k(), x = p.x, F = fs * k();
  bool color = cfg.gui.author_style == "color";
  if (!color) {
    ImGui::SetCursorScreenPos(ImVec2(x, p.y + (H - 14 * k()) / 2));
    avatar(login);
    x += 20 * k();
  }
  std::string name = who(login);
  ImFont* fnt = color ? f.medium : f.regular;
  float right = width > 0 ? p.x + width : x + fnt->CalcTextSizeA(F, FLT_MAX, 0, name.c_str()).x + 1;  // +1: no ellipsis on a rounding
  ImGui::PushFont(fnt, F);
  ImGui::PushStyleColor(ImGuiCol_Text, col(color ? name_color(login, t.dark) : t.fg2));
  ImGui::RenderTextEllipsis(d, ImVec2(x, p.y + (H - F) / 2), ImVec2(right, p.y + H), right, name.c_str(), nullptr, nullptr);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(right - p.x, H));
  return right - p.x;
}

// A person's avatar: their first letter on their configured color.
void Gui::avatar(const std::string& login, float size) {
  const Person* p = person(login);
  int rgb = p && p->rgb >= 0 ? p->rgb : 0x8b949e;
  std::string name = who(login);
  std::string letter = name.empty() ? "?" : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
  float S = size * k();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 c(pos.x + S / 2, pos.y + S / 2);
  d->AddCircleFilled(c, S / 2, IM_COL32(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff, 255));
  float fs = S * 8 / 14;  // the canvas: 8px on 14
  ImVec2 ts = f.semibold->CalcTextSizeA(fs, FLT_MAX, 0, letter.c_str());
  d->AddText(f.semibold, fs, ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), dark_text_on(rgb) ? col(0x0d1117ffu) : IM_COL32_WHITE,
             letter.c_str());
  ImGui::Dummy(ImVec2(S, S));
}

// GitHub's five blocks, 7px with 2px between.
void Gui::diffstat_blocks(long add, long del, float h) {
  auto b = diffstat(add, del);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float s = 7 * k(), gap = 2 * k(), H = h > 0 ? h * k() : ImGui::GetTextLineHeight();
  float y = p.y + (H - s) / 2;
  ImDrawList* d = ImGui::GetWindowDrawList();
  for (int i = 0; i < 5; ++i) {
    uint32_t c = b[static_cast<size_t>(i)] == 1 ? t.success : b[static_cast<size_t>(i)] == 2 ? t.danger : t.empty;
    float x = p.x + i * (s + gap);
    d->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), col(c), 1 * k());
  }
  ImGui::Dummy(ImVec2(5 * s + 4 * gap, H));
}

// A button as the canvas draws them: default (bordered, btn), primary
// (green, white text) or ghost (no border); 24px with 12px text, or 20px
// with 11px inside a pane. key: a dim mono hint after the label.
static float btn_text(float h) { return h <= 20 ? 11.f : 12.f; }
static float btn_pad(float h, Btn kind) { return h <= 20 ? 7.f : kind == Btn::Primary ? 10.f : 8.f; }

float Gui::button_width(const std::string& label, Btn kind, const std::string& key, Icon icon, float h) {
  const char* end = ImGui::FindRenderedTextEnd(label.c_str());
  float w = f.medium->CalcTextSizeA(btn_text(h) * k(), FLT_MAX, 0, label.c_str(), end).x + 2 * btn_pad(h, kind) * k();
  if (!key.empty()) w += f.mono->CalcTextSizeA(10 * k(), FLT_MAX, 0, key.c_str()).x + 6 * k();
  if (icon) w += 17 * k();
  return w;
}

bool Gui::button(const std::string& label, Btn kind, float h, const std::string& key, float w, Icon icon) {
  float H = h * k(), fs = btn_text(h) * k();
  const char* end = ImGui::FindRenderedTextEnd(label.c_str());
  ImVec2 ts = f.medium->CalcTextSizeA(fs, FLT_MAX, 0, label.c_str(), end);
  ImVec2 ks = key.empty() ? ImVec2(0, 0) : f.mono->CalcTextSizeA(10 * k(), FLT_MAX, 0, key.c_str());
  float kw = key.empty() ? 0 : ks.x + 6 * k(), iw = icon ? 17 * k() : 0;
  ImVec2 size(w > 0 ? w * k() : button_width(label, kind, key, icon, h), H);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(label.c_str(), size);
  bool hov = ImGui::IsItemHovered();
  ImDrawList* d = ImGui::GetWindowDrawList();
  uint32_t bg = kind == Btn::Primary ? (hov ? t.primary_hover : t.primary)
                : kind == Btn::Ghost ? (hov ? (t.dark ? t.border_muted : t.inset) : 0u)
                                     : (hov ? (t.dark ? t.border_muted : t.inset) : t.btn);
  if (bg) d->AddRectFilled(p, p + size, col(bg), 6 * k());
  if (kind != Btn::Ghost) d->AddRect(p, p + size, col(kind == Btn::Primary ? t.primary_border : t.border), 6 * k());
  uint32_t fg = kind == Btn::Primary ? 0xffffffffu : t.fg;
  float x = p.x + (size.x - ts.x - kw - iw) / 2;
  if (icon) {
    float is = icon == icon_play ? 9 * k() : 11 * k();
    icon(d, ImVec2(x + (11 * k() - is) / 2, p.y + (H - is) / 2), is, col(icon == icon_play && kind != Btn::Primary ? t.success : kind == Btn::Primary ? fg : t.muted));
    x += iw;
  }
  d->AddText(f.medium, fs, ImVec2(x, p.y + (H - ts.y) / 2), col(fg), label.c_str(), end);
  if (!key.empty())
    d->AddText(f.mono, 10 * k(), ImVec2(x + ts.x + 6 * k(), p.y + (H - ks.y) / 2), col(kind == Btn::Primary ? 0xffffffbfu : t.muted),
               key.c_str());
  return clicked;
}

bool Gui::icon_button(const char* id, void (*icon)(ImDrawList*, ImVec2, float, ImU32), float size, bool bordered,
                      const char* hint) {
  float S = size * k();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id, ImVec2(S, S));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* d = ImGui::GetWindowDrawList();
  uint32_t bg = hov ? (t.dark ? t.border_muted : t.inset) : bordered ? t.btn : 0u;
  if (bg) d->AddRectFilled(p, ImVec2(p.x + S, p.y + S), col(bg), 6 * k());
  if (bordered) d->AddRect(p, ImVec2(p.x + S, p.y + S), col(t.border), 6 * k());
  float is = S / 2;
  g_icon_bg = col(bg ? bg : t.panel);
  icon(d, ImVec2(p.x + (S - is) / 2, p.y + (S - is) / 2), is, col(t.muted));
  if (hint && hov) tip(hint);
  return clicked;
}

// A pane's section: an 11px semibold muted label on a 22px row, a line under it.
void Gui::section(const std::string& title) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x, h = 22 * k();
  ImVec2 ts = f.semibold->CalcTextSizeA(11 * k(), FLT_MAX, 0, title.c_str());
  d->AddText(f.semibold, 11 * k(), ImVec2(p.x, p.y + (h - ts.y) / 2), col(t.muted), title.c_str());
  d->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), col(t.border_muted));
  ImGui::Dummy(ImVec2(w, h));
}

bool Gui::link_row(const std::string& id, float h) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(w, h * k()));
  if (ImGui::IsItemHovered())
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + h * k()), col(t.dark ? t.border_muted : t.inset), 6 * k());
  ImGui::SetCursorScreenPos(p);
  return clicked;
}

// -- icons (16-unit grids, as the canvas's SVGs) -----------------------------------------

static ImVec2 at(ImVec2 p, float s, float x, float y) { return ImVec2(p.x + x * s / 16, p.y + y * s / 16); }

void icon_logo(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float u = s / 18, w = 1.6f * u;
  auto P = [&](float x, float y) { return ImVec2(p.x + x * u, p.y + y * u); };
  d->AddCircle(P(4.5f, 4.5f), 2 * u, c, 0, w);
  d->AddCircle(P(4.5f, 13.5f), 2 * u, c, 0, w);
  d->AddCircle(P(13.5f, 13.5f), 2 * u, c, 0, w);
  d->AddLine(P(4.5f, 6.5f), P(4.5f, 11.5f), c, w);
  d->PathLineTo(P(13.5f, 11.5f));
  d->PathLineTo(P(13.5f, 8));
  d->PathBezierCubicCurveTo(P(13.5f, 6.3f), P(12.2f, 5), P(10.5f, 5));
  d->PathLineTo(P(8, 5));
  d->PathStroke(c, w);
  d->PathLineTo(P(9.5f, 3));
  d->PathLineTo(P(7.5f, 5));
  d->PathLineTo(P(9.5f, 7));
  d->PathStroke(c, w);
}

void icon_search(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  d->AddCircle(at(p, s, 7, 7), 4.5f * s / 16, c, 0, 1.5f * s / 16);
  d->AddLine(at(p, s, 10.5f, 10.5f), at(p, s, 13.5f, 13.5f), c, 1.5f * s / 16);
}

void icon_refresh(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  ImVec2 ctr = at(p, s, 8, 8);
  d->PathArcTo(ctr, 5.5f * s / 16, 0.2f, 5.6f);
  d->PathStroke(c, 1.5f * s / 16);
  d->AddLine(at(p, s, 13.5f, 2.5f), at(p, s, 13.5f, 5.5f), c, 1.5f * s / 16);
  d->AddLine(at(p, s, 13.5f, 5.5f), at(p, s, 10.5f, 5.5f), c, 1.5f * s / 16);
}

void icon_sun(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddCircle(at(p, s, 8, 8), 3 * s / 16, c, 0, w);
  for (int i = 0; i < 8; ++i) {
    float a = i * IM_PI / 4;
    d->AddLine(at(p, s, 8 + std::cos(a) * 5.2f, 8 + std::sin(a) * 5.2f), at(p, s, 8 + std::cos(a) * 6.6f, 8 + std::sin(a) * 6.6f), c, w);
  }
}

void icon_moon(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  d->AddCircleFilled(at(p, s, 8, 8.5f), 5.8f * s / 16, c);
  d->AddCircleFilled(at(p, s, 11, 5.5f), 5.0f * s / 16, g_icon_bg);
}

void icon_external(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddLine(at(p, s, 9, 2.5f), at(p, s, 13.5f, 2.5f), c, w);
  d->AddLine(at(p, s, 13.5f, 2.5f), at(p, s, 13.5f, 7), c, w);
  d->AddLine(at(p, s, 13.5f, 2.5f), at(p, s, 7.5f, 8.5f), c, w);
  d->PathLineTo(at(p, s, 12, 9.5f));
  d->PathLineTo(at(p, s, 12, 13.5f));
  d->PathLineTo(at(p, s, 2.5f, 13.5f));
  d->PathLineTo(at(p, s, 2.5f, 4));
  d->PathLineTo(at(p, s, 6.5f, 4));
  d->PathStroke(c, w);
}

void icon_copy(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddRect(at(p, s, 5.5f, 5.5f), at(p, s, 13.5f, 13.5f), c, 1.5f * s / 16 , w);
  d->PathLineTo(at(p, s, 10.5f, 3.5f));
  d->PathLineTo(at(p, s, 10.5f, 2));
  d->PathLineTo(at(p, s, 2.5f, 2));
  d->PathLineTo(at(p, s, 2.5f, 10));
  d->PathLineTo(at(p, s, 4, 10));
  d->PathStroke(c, w);
}

void icon_play(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  d->AddTriangleFilled(at(p, s, 4, 2.4f), at(p, s, 4, 13.6f), at(p, s, 13.6f, 8), c);
}

void icon_plus(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.6f * s / 16;
  d->AddLine(at(p, s, 8, 3), at(p, s, 8, 13), c, w);
  d->AddLine(at(p, s, 3, 8), at(p, s, 13, 8), c, w);
}

void icon_filter(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddLine(at(p, s, 2, 3.5f), at(p, s, 14, 3.5f), c, w);
  d->AddLine(at(p, s, 4.5f, 8), at(p, s, 11.5f, 8), c, w);
  d->AddLine(at(p, s, 7, 12.5f), at(p, s, 9, 12.5f), c, w);
}

void icon_columns(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddRect(at(p, s, 2, 3), at(p, s, 14, 13), c, 1.5f * s / 16 , w);
  d->AddLine(at(p, s, 6, 3), at(p, s, 6, 13), c, w);
  d->AddLine(at(p, s, 10, 3), at(p, s, 10, 13), c, w);
}

void icon_cross(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddLine(at(p, s, 4, 4), at(p, s, 12, 12), c, w);
  d->AddLine(at(p, s, 12, 4), at(p, s, 4, 12), c, w);
}

void icon_chevron(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddLine(at(p, s, 4, 6.4f), at(p, s, 8, 10.4f), c, w);
  d->AddLine(at(p, s, 8, 10.4f), at(p, s, 12, 6.4f), c, w);
}

void icon_grip(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  for (float x : {5.f, 11.f})
    for (float y : {4.f, 8.f, 12.f}) d->AddCircleFilled(at(p, s, x, y), 1.3f * s / 16, c);
}

// -- the frame -----------------------------------------------------------------------------

void Gui::frame() {
  check_os_appearance();
  poll_rate_limit();
  apply_theme();
  tick();
  view_err.clear();
  cur_rows = rows(active, &view_err).value_or(std::vector<Pr>{});
  if (sel_url.empty() && !cur_rows.empty()) sel_url = cur_rows[0].url;
  if (reader_open && !cache.count(reader_url)) reader_open = false;
  // The reader's keys while it, or a pane about its PR, has the focus.
  reader_focused = false;
  if (ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow; nav && reader_open)
    for (const char* w : {"###reader", "###results", "###outline", "###llm", "###runlog", "###problems"})
      reader_focused = reader_focused || nav->RootWindow->ID == ImHashStr(w);

  ImGui::PushFont(f.regular, 0.f);
#ifndef __APPLE__
  menu_bar();
#endif
  toolbar();
  if (cfg.gui.status_bar) status_bar();
  dock_spaces();
  if (settings_open) settings_window();
  else settings_shown = false;
  if (palette_open) palette();
  modals();
  keys();
  ImGui::PopFont();
}

const Pr* Gui::current_pr() const {
  if (reader_focused && cache.count(reader_url)) return &cache.at(reader_url);
  return selected_pr();
}

void Gui::quit() { glfwSetWindowShouldClose(window, 1); }

void Gui::restart_plugin(size_t i) {
  if (plugins) plugins->restart(i);
  flash("restarting " + cfg.plugins[i].name);
}

void Gui::zoom(int by) {
  int to = by ? std::clamp(cfg.gui.font_size + by, 9, 20) : GuiPrefs{}.font_size;
  if (to == cfg.gui.font_size) return;
  cfg.gui.font_size = to;
  save();
}

// Mod held unless it already is: a menu click has no key down, a shortcut does.
void Gui::send_key(int key) {
  ImGuiIO& io = ImGui::GetIO();
  // held now, on the keyboard: io.KeyCtrl is last frame's, and a quick ⌘V
  // would lose its paste to a Mod up sent while Cmd is still down
#ifdef __APPLE__
  ImGuiKey mod = ImGuiMod_Super;  // what the GLFW backend sends for Cmd; ImGui swaps it
  bool held = glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS;
#else
  ImGuiKey mod = ImGuiMod_Ctrl;
  bool held = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
#endif
  if (!held) io.AddKeyEvent(mod, true);
  io.AddKeyEvent(static_cast<ImGuiKey>(key), true);
  io.AddKeyEvent(static_cast<ImGuiKey>(key), false);
  if (!held) io.AddKeyEvent(mod, false);
}

std::vector<Menu> Gui::menus() {
  const Pr* cur = current_pr();
  bool typing = ImGui::GetIO().WantTextInput;
  Cmd sep;
  auto flag = [this](bool* v) { return [this, v] { *v = !*v; save(); }; };
  auto pick = [this](std::string* v, std::string to) { return [this, v, to] { *v = to; save(); }; };
  std::vector<Menu> m;
#ifdef __APPLE__
  m.push_back({"anton",
               {{"About anton", "", [] { mac_about(); }},
                sep,
                {"Settings…", "Mod+,", [this] { settings_open = true; }},
                sep,
                {"Hide anton", "Mod+H", [] { mac_hide(); }},
                sep,
                {"Quit anton", "Mod+Q", [this] { quit(); }}}});
#endif
  {
    std::vector<Cmd> file;
#ifndef __APPLE__
    file.push_back({"Settings…", "Mod+,", [this] { settings_open = true; }});
#endif
    file.push_back({"Open config.json", "", [this] { open_config(); }});
    file.push_back(sep);
    file.push_back({"Close Reader", "Mod+W", [this] { close_reader(); }, reader_open});
#ifndef __APPLE__
    file.push_back(sep);
    file.push_back({"Quit", "Mod+Q", [this] { quit(); }});
#endif
    m.push_back({"File", file});
  }
  m.push_back({"Edit",
               {{"Cut", "Mod+X", [this] { send_key(ImGuiKey_X); }, typing, -1, {}, true},
                {"Copy", "Mod+C", [this, typing] { typing ? send_key(ImGuiKey_C) : act("y"); }, typing || cur, -1, {}, true},
                {"Paste", "Mod+V", [this] { send_key(ImGuiKey_V); }, typing, -1, {}, true},
                {"Select All", "Mod+A", [this] { send_key(ImGuiKey_A); }, typing, -1, {}, true},
                sep,
                {"Copy URL", "Y", [this] { act("y"); }, cur != nullptr},
                {"Edit the Query", "/", [this] { act("/"); }},
                {"Clear org/repo", "X", [this] { act("x"); }, !scope.empty()},
                sep,
                {"Command Palette", "Mod+K", [this] {
                   palette_open = !palette_open;
                   palette_focus = palette_open;
                   palette_q.clear();
                   palette_sel = 0;
                 }}}});
  {
    GuiPrefs& g = cfg.gui;
    std::vector<Cmd> cols;
    static const std::pair<const char*, const char*> names[] = {
        {"number", "Number"}, {"repo", "Repo"}, {"size", "Size (+/−)"}, {"blocks", "Size blocks"}, {"state", "State"},
        {"age", "Age"},       {"author", "Author"}};
    for (auto [id, name] : names)
      for (auto& c : g.columns)
        if (c.name == id) cols.push_back({name, "", flag(&c.shown), true, c.shown});
    cols.push_back({"Avatars", "", [this] { cfg.gui.author_style = cfg.gui.author_style == "avatar" ? "color" : "avatar"; save(); },
                    true, g.author_style == "avatar"});
    for (auto& c : g.columns)
      if (c.name == "labels" || c.name == "tags") cols.push_back({c.name == "labels" ? "Labels" : "Plugin tags", "", flag(&c.shown), true, c.shown});
    cols.push_back(sep);
    cols.push_back({"Customize…", "", [this] {  // Mod+, is Settings…'s
                      settings_open = true;
                      settings_page = 2;
                    }});
    std::vector<Cmd> rows, states, themes;
    for (auto [id, name] : {std::pair{"compact", "Compact 24"}, {"cozy", "Cozy 28"}, {"roomy", "Roomy 34"}})
      rows.push_back({name, "", pick(&g.density, id), true, g.density == id});
    for (auto [id, name] : {std::pair{"dot", "Circle and label"}, {"text", "Coloured text"}, {"pill", "Pill"}})
      states.push_back({name, "", pick(&g.state_style, id), true, g.state_style == id});
    for (auto [id, name] : {std::pair{"dark", "Dark"}, {"light", "Light"}, {"system", "Match System"}})
      themes.push_back({name, "", [this, id = std::string(id)] { set_theme(id); }, true, g.theme == id});
#ifdef __APPLE__
    bool full = mac_fullscreen(window);
#else
    bool full = glfwGetWindowMonitor(window) != nullptr;
#endif
    m.push_back({"View",
                 {{"Views", "Mod+1", flag(&g.views), true, g.views},
                  {"Preview", "Mod+2", flag(&g.preview), true, g.preview},
                  {"Results", "Shift+Mod+R", flag(&g.results), true, g.results},
                  {"Run log", "Mod+J", flag(&g.run_log), true, g.run_log},
                  sep,
                  {"Columns", "", nullptr, true, -1, cols},
                  {"Row height", "", nullptr, true, -1, rows},
                  {"State style", "", nullptr, true, -1, states},
                  sep,
                  {"Theme", "", nullptr, true, -1, themes},
                  {"Zoom In", "Mod++", [this] { zoom(1); }, g.font_size < 20},
                  {"Zoom Out", "Mod+-", [this] { zoom(-1); }, g.font_size > 9},
                  {"Actual Size", "Mod+0", [this] { zoom(0); }},
                  sep,
                  {"Reset Layout", "", [this] { reset_layout = true; }},
#ifdef __APPLE__
                  {full ? "Exit Full Screen" : "Enter Full Screen", "Ctrl+Mod+F", [this] { mac_toggle_fullscreen(window); }}}});
#else
                  {full ? "Exit Full Screen" : "Enter Full Screen", "F11", [this] { toggle_fullscreen(); }}}});
#endif
  }
  {
    std::vector<Cmd> prs = {{"Open in Reader", "Enter", [this] { act("⏎"); }, selected_pr() != nullptr},
                            {"Open on GitHub", "W", [this] { act("w"); }, cur != nullptr},
                            {"Copy URL", "Y", [this] { act("y"); }, cur != nullptr},
                            sep,
                            {"Refresh", "R", [this] { refresh(); }}};
    std::vector<Cmd> sorts;
    for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
      if (s == Sort::List && !view().is_list()) continue;
      sorts.push_back({sort_label(s), "", [this, s] { set_sort(s); }, true,
                       view().sort_or_default() == s});
    }
    prs.push_back({"Sort", "", nullptr, true, -1, sorts});
    prs.push_back(sep);
    for (size_t i = 0; i < tabs.size(); ++i)
      prs.push_back({tabs[i].v.name, i < 9 ? std::to_string(i + 1) : "", [this, i] {
                       switch_view(i);
                       focus_list = true;
                     },
                     true, i == active});
    prs.push_back({"New View", "+", [this] { act("+"); }});
    m.push_back({"Pull requests", prs});
  }
  {
    std::vector<Cmd> pl;
    if (cfg.plugins.empty()) pl.push_back({"No plugins configured", "", nullptr, false});
    auto keys = plugin_keys();
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      std::vector<Cmd> acts;
      for (auto& [pi, a] : keys)
        if (pi == i) {
          std::string url = cur ? cur->url : "";
          acts.push_back({button_label(a.button, a.label, a.id), a.key,
                          [this, key = a.key, url] { plugin_action(key, url); }, cur != nullptr});
        }
      if (!acts.empty()) acts.push_back(sep);
      acts.push_back({"Restart", "", [this, i] { restart_plugin(i); }});
      pl.push_back({cfg.plugins[i].name, "", nullptr, true, -1, acts});
    }
    pl.push_back(sep);
    pl.push_back({"Plugin Settings…", "", [this] {
                    settings_open = true;
                    settings_page = 4;
                  }});
    m.push_back({"Plugins", pl});
  }
  m.push_back({"Window",
               {{"Minimize", "Mod+M", [this] { glfwIconifyWindow(window); }},
                {"Zoom", "", [this] {
                   if (glfwGetWindowAttrib(window, GLFW_MAXIMIZED)) glfwRestoreWindow(window);
                   else glfwMaximizeWindow(window);
                 }},
                sep,
                {"Outline", "", flag(&show_outline), reader_open, show_outline},
                {"LLM review", "", flag(&show_llm), reader_open, show_llm},
                {"Status bar", "", flag(&cfg.gui.status_bar), true, cfg.gui.status_bar}}});
  m.push_back({"Help",
               {{"Search or Run a Command", "", [this] { palette_open = palette_focus = true; }},
                {"anton on GitHub", "", [this] { open_file("https://github.com/Thinato/anton"); }},
                sep,
                {std::string("anton-gui ") + kVersion, "", nullptr, false}}});
  return m;
}

#ifndef __APPLE__
// F11: the monitor the window is mostly on, at its mode; again, back where it was.
void Gui::toggle_fullscreen() {
  static int x, y, w, h;
  if (glfwGetWindowMonitor(window)) {
    glfwSetWindowMonitor(window, nullptr, x, y, w, h, 0);
    return;
  }
  glfwGetWindowPos(window, &x, &y);
  glfwGetWindowSize(window, &w, &h);
  GLFWmonitor* best = glfwGetPrimaryMonitor();
  int count = 0, most = -1;
  GLFWmonitor** ms = glfwGetMonitors(&count);
  for (int i = 0; i < count; ++i) {
    int mx, my;
    const GLFWvidmode* md = glfwGetVideoMode(ms[i]);
    glfwGetMonitorPos(ms[i], &mx, &my);
    int ow = std::max(0, std::min(x + w, mx + md->width) - std::max(x, mx));
    int oh = std::max(0, std::min(y + h, my + md->height) - std::max(y, my));
    if (ow * oh > most) most = ow * oh, best = ms[i];
  }
  if (!best) return;
  const GLFWvidmode* md = glfwGetVideoMode(best);
  glfwSetWindowMonitor(window, best, 0, 0, md->width, md->height, md->refreshRate);
}
#endif

// "Shift+Mod+R" as the platform writes it: ⇧⌘R on macOS, Shift+Ctrl+R
// elsewhere. The UI font has the symbols (Apple Symbols is merged in).
static std::string key_text(const std::string& key) {
  std::string s = key;
#ifdef __APPLE__
  for (auto [from, to] : {std::pair{"Ctrl+", "⌃"}, {"Shift+", "⇧"}, {"Mod+", "⌘"}})
#else
  for (auto [from, to] : {std::pair{"Mod+", "Ctrl+"}})
#endif
    for (size_t at = s.find(from); at != std::string::npos && s.size() > std::strlen(from); at = s.find(from, at))
      s.replace(at, std::strlen(from), to);
  return s;
}

// A key with Mod in it (or an F key) pressed this frame, exactly those modifiers held.
static bool chord_pressed(const std::string& key) {
  std::string k = key;
  int mods = 0;
  bool shift_free = false;
#ifdef __APPLE__
  const int ctrl = ImGuiMod_Super;  // the real Ctrl: ImGui swaps it with Cmd
#else
  const int ctrl = ImGuiMod_Ctrl;
#endif
  for (bool more = true; more;) {
    more = false;
    for (auto [prefix, mod] : {std::pair{"Shift+", int(ImGuiMod_Shift)}, {"Ctrl+", ctrl}, {"Mod+", int(ImGuiMod_Ctrl)}})
      if (k.size() > std::strlen(prefix) && k.starts_with(prefix)) {
        mods |= mod;
        k = k.substr(std::strlen(prefix));
        more = true;
      }
  }
  std::vector<ImGuiKey> keys;
  if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') keys = {static_cast<ImGuiKey>(ImGuiKey_A + (k[0] - 'A'))};
  else if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') keys = {static_cast<ImGuiKey>(ImGuiKey_0 + (k[0] - '0'))};
  else if (k == ",") keys = {ImGuiKey_Comma};
  else if (k == "+") keys = {ImGuiKey_Equal, ImGuiKey_KeypadAdd}, shift_free = true;  // ⌘= and ⌘+ both
  else if (k == "-") keys = {ImGuiKey_Minus, ImGuiKey_KeypadSubtract};
  else if (k == "F11") keys = {ImGuiKey_F11};
  if (keys.empty() || (!mods && k != "F11")) return false;
  int held = ImGui::GetIO().KeyMods & (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super);
  if (shift_free) held &= ~ImGuiMod_Shift;
  if (held != mods) return false;
  return std::any_of(keys.begin(), keys.end(), [](ImGuiKey x) { return ImGui::IsKeyPressed(x, false); });
}

// Menu entries as ImGui draws them: Linux's menu bar (macOS has the
// system's), and the list's Columns button on both.
void draw_menu(const std::vector<Cmd>& items) {
  for (auto& c : items) {
    if (c.label.empty()) ImGui::Separator();
    else if (!c.run && !c.sub.empty()) {
      if (ImGui::BeginMenu(c.label.c_str(), c.enabled)) {
        draw_menu(c.sub);
        ImGui::EndMenu();
      }
    } else if (ImGui::MenuItem(c.label.c_str(), c.key.empty() ? nullptr : key_text(c.key).c_str(), c.check == 1,
                               c.enabled && c.run) &&
               c.run) {
      c.run();
    }
  }
}

void Gui::menu_bar() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k(), 4 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 6 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  if (ImGui::BeginMainMenuBar()) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float H = ImGui::GetWindowHeight();
    d->AddLine(ImVec2(wp.x, wp.y + H - 1), ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + H - 1), col(t.border));
    for (auto& mn : menus())
      if (ImGui::BeginMenu(mn.title.c_str())) {
        draw_menu(mn.items);
        ImGui::EndMenu();
      }
    ImGui::EndMainMenuBar();
  }
  ImGui::PopStyleVar(3);
}

// Under the menus: the logo, the search box that opens the palette, the
// refresh state and the theme button.
void Gui::toolbar() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float H = 38 * k();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.panel));
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  bool shown = ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, H, fl);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  if (!shown) return ImGui::End();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  float W = ImGui::GetWindowWidth();
  d->AddLine(ImVec2(wp.x, wp.y + H - 1), ImVec2(wp.x + W, wp.y + H - 1), col(t.border));
  icon_logo(d, ImVec2(wp.x + 12 * k(), wp.y + (H - 16 * k()) / 2), 16 * k(), col(t.accent));
  d->AddText(f.semibold, ImGui::GetFontSize(), ImVec2(wp.x + 36 * k(), wp.y + (H - ImGui::GetFontSize()) / 2), col(t.fg), "anton");
  // the search box, centered
  float sw = 380 * k(), sh = 24 * k();
  ImGui::SetCursorPos(ImVec2(std::max(90 * k(), (W - sw) / 2), (H - sh) / 2));
  ImVec2 sp = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##search", ImVec2(sw, sh))) palette_open = palette_focus = true;
  bool hov = ImGui::IsItemHovered();
  d->AddRectFilled(sp, sp + ImVec2(sw, sh), col(t.bg), 6 * k());
  d->AddRect(sp, sp + ImVec2(sw, sh), col(hov ? t.muted : t.border), 6 * k());
  icon_search(d, ImVec2(sp.x + 8 * k(), sp.y + (sh - 12 * k()) / 2), 12 * k(), col(t.muted));
  d->AddText(ImVec2(sp.x + 28 * k(), sp.y + (sh - ImGui::GetFontSize()) / 2), col(t.muted), "Search or run a command");
#ifdef __APPLE__
  const char* kb = "⌘K";
#else
  const char* kb = "Ctrl K";
#endif
  ImVec2 ks = f.mono->CalcTextSizeA(10 * k(), FLT_MAX, 0, kb);
  d->AddText(f.mono, 10 * k(), ImVec2(sp.x + sw - 8 * k() - ks.x, sp.y + (sh - ks.y) / 2), col(t.muted), kb);
  // the refresh state, the refresh and theme buttons
  std::string status = loading() ? "Refreshing…" : last_fetch ? "Refreshed " + age(now() - last_fetch) + " ago" : "Loading…";
  if (status == "Refreshed 0m ago") status = "Refreshed just now";
  float bw = 24 * k(), x = W - 12 * k() - bw;
  ImGui::SetCursorPos(ImVec2(x, (H - bw) / 2));
  bool dark = t.dark;
  if (icon_button("##theme", dark ? icon_sun : icon_moon, 24, true, dark ? "Switch to light mode" : "Switch to dark mode"))
    set_theme(dark ? "light" : "dark");
  x -= 4 * k() + bw;
  ImGui::SetCursorPos(ImVec2(x, (H - bw) / 2));
  std::string tip = "Refresh now (r) · every " + std::to_string(cfg.refresh_seconds) + "s";
  if (icon_button("##refresh", icon_refresh, 24, false, tip.c_str())) refresh();
  ImVec2 st = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, status.c_str());
  d->AddText(f.regular, 11 * k(), ImVec2(wp.x + x - 8 * k() - st.x, wp.y + (H - st.y) / 2), col(t.muted), status.c_str());
  ImGui::End();
}

void Gui::status_bar() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.panel));
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, 22 * k(), fl)) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    d->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), col(t.border));
    ImGui::PushFont(f.regular, 11 * k());
    float y = (22 * k() - ImGui::GetTextLineHeight()) / 2;
    ImGui::SetCursorPos(ImVec2(12 * k(), y));
    auto item = [&](const std::string& s, uint32_t c, uint32_t dot = 0) {
      if (dot) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        d->AddCircleFilled(ImVec2(p.x + 3 * k(), p.y + ImGui::GetTextLineHeight() / 2), 3 * k(), col(dot));
        ImGui::Dummy(ImVec2(6 * k(), 1));
        ImGui::SameLine(0, 5 * k());
      }
      text(s, c);
      ImGui::SameLine(0, 14 * k());
    };
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      std::string problem = plugin_problem(i), name = cfg.plugins[i].name;
      uint32_t dot = !problem.empty() ? t.danger : live[i].health == "ok" ? t.success : t.attention;
      for (auto& [url, info] : live[i].prs)
        if (info.marks_summary && result_of(*info.marks_summary) == Result::Running) {
          dot = t.accent;
          if (url == reader_url || !reader_focused) name += " · running";
          break;
        }
      item(name, t.muted, dot);
    }
    if (reader_focused) {
      auto ds = docs();
      if (reader_tab < ds.size() && ds[reader_tab].text) {
        int lines = static_cast<int>(std::count(ds[reader_tab].text->begin(), ds[reader_tab].text->end(), '\n'));
        item((ds[reader_tab].path.empty() ? "Description" : ds[reader_tab].path) + " · " + std::to_string(lines) + " lines", t.muted);
      }
    } else {
      item(std::to_string(cur_rows.size()) + " pull requests", t.muted);
    }
    if (!error.empty()) item(error, t.danger);
    else if (!view_err.empty()) item(view_err, t.danger);
    // right: the message, the rate limit, the refresh, the version
    std::vector<std::pair<std::string, uint32_t>> right;
    if (now() < notice_until && !notice.empty()) right.push_back({strip_emoji(notice), notice_bad ? t.danger : t.accent});
    if (!update_note.empty()) right.push_back({update_note, t.attention});
    if (!rate_limit.empty()) right.push_back({"GitHub API " + rate_limit, t.muted});
    right.push_back({"Refreshes every " + std::to_string(cfg.refresh_seconds) + "s", t.muted});
    right.push_back({std::string("anton ") + kVersion, t.muted});
    float w = 0;
    for (auto& [s, c] : right) w += ImGui::CalcTextSize(s.c_str()).x + 14 * k();
    float x = std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w + 4 * k());
    ImGui::SetCursorPos(ImVec2(x, y));
    for (auto& [s, c] : right) item(s, c);
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// Was the dock space laid out before (by us, or by the user in the layout
// file)? A window's remembered DockId says so.
static bool laid_out(ImGuiID id, std::initializer_list<const char*> windows) {
  ImGuiDockNode* n = ImGui::DockBuilderGetNode(id);
  if (n && n->IsSplitNode()) return true;
  for (auto w : windows)
    if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr(w)); s && s->DockId) return true;
  return false;
}

// The dock node a window is in, or was last in (a closed tab's stays).
static ImGuiID dock_of(const char* name) {
  ImGuiID id = 0;
  if (ImGuiWindow* w = ImGui::FindWindowByName(name)) id = w->DockId;
  else if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr(name))) id = s->DockId;
  return id && ImGui::DockBuilderGetNode(id) ? id : 0;
}

// The canvas's: Views 200px | Pull requests | Preview 520px. The reader
// joins Preview's node when it opens (dock_reader).
void Gui::build_layout(ImGuiID id, ImVec2 size) {
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, size);
  ImGuiID center = id;
  float left_w = 200 * k(), right_w = 520 * k();
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, std::min(0.3f, left_w / size.x), nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, std::min(0.6f, right_w / (size.x - left_w)), nullptr, &center);
  ImGui::DockBuilderDockWindow("###views", left);
  ImGui::DockBuilderDockWindow("###preview", right);
  ImGui::DockBuilderDockWindow("###prs", center);
  ImGui::DockBuilderFinish(id);
  relayout = true;
}

// When the reader opens: Results, Run log and Problems go in a node split
// off under Preview's (the first time, and after a layout is rebuilt),
// then the reader where gui.reader_placement says: a tab in Preview's
// node, where ImGui last had it, or floating in a window of its own.
void Gui::dock_reader(ImGuiID dockspace) {
  ImGuiID node = dock_of("###preview");
  if (!node) node = dock_of("###prs");
  bool fresh = !ImGui::FindWindowByName("###results") && !ImGui::FindWindowSettingsByID(ImHashStr("###results"));
  bool rebuilt = relayout;  // where the reader was is gone with the old layout
  if (node && (fresh || rebuilt)) {
    ImGuiDockNode* n = ImGui::DockBuilderGetNode(node);
    float h = n && n->Size.y > 0 ? n->Size.y : ImGui::GetMainViewport()->WorkSize.y;
    ImGuiID top = 0;
    ImGuiID below = ImGui::DockBuilderSplitNode(node, ImGuiDir_Down, std::clamp(196 * k() / h, 0.1f, 0.5f), nullptr, &top);
    for (const char* w : {"###results", "###runlog", "###problems"}) ImGui::DockBuilderDockWindow(w, below);
    ImGui::DockBuilderFinish(dockspace);
    node = top;
  }
  relayout = false;
  const std::string& where = cfg.gui.reader_placement;
  bool known = ImGui::FindWindowByName("###reader") || ImGui::FindWindowSettingsByID(ImHashStr("###reader"));
  if (where == "last" && known && !rebuilt) return;
  if (where == "window") reader_float = true;
  else reader_dock = node;
}

void Gui::dock_spaces() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.border));
  ImGuiWindowFlags host = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("##dockhost", nullptr, host);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  ImGuiID id = ImGui::GetID("MainDock");
  ImVec2 size = ImGui::GetContentRegionAvail();
  // A layout from before the reader was a tab has its panes in a dock space
  // of their own, never submitted now: it goes, and they get a node again.
  // Their settings go too (frame 1, before any of them exists), so the
  // next reader finds them fresh even in a later session: the layout file
  // is saved without ReaderDock, and with their DockIds zeroed.
  if (ImGuiID old = ImGui::GetID("ReaderDock"); ImGui::DockBuilderGetNode(old)) {
    ImGui::DockBuilderRemoveNode(old);
    for (const char* w : {"###reader", "###results", "###outline", "###llm", "###runlog", "###problems"})
      ImGui::ClearWindowSettings(w);
    relayout = true;
  }
  if (reset_layout || !laid_out(id, {"###views", "###prs", "###preview"})) {
    build_layout(id, size);
    reset_layout = false;
    if (reader_open) place_reader = true;
  }
  if (place_reader) {
    place_reader = false;
    pick_tabs = 8;  // the panes may come a frame or two late
    dock_reader(id);
  }
  ImGui::DockSpace(id, size);
  ImGui::End();

  if (cfg.gui.views) views_pane();
  prs_pane();
  if (cfg.gui.preview) preview_pane();
  if (reader_open) {
    reader_pane();
    if (cfg.gui.results) results_pane();
    if (cfg.gui.run_log) {
      runlog_pane();
      problems_pane();
    }
    if (show_outline) outline_pane();
    if (show_llm) llm_pane();
    // just placed: its tab and Results' are the ones shown in their nodes,
    // whatever has the focus (the newest tab in a node would be)
    if (pick_tabs > 0) {
      --pick_tabs;
      for (const char* name : {"###results", "###reader"})
        if (ImGuiWindow* w = ImGui::FindWindowByName(name); w && w->DockNode && w->DockNode->TabBar)
          w->DockNode->TabBar->NextSelectedTabId = w->TabId;
    }
  }
}

// -- keys ---------------------------------------------------------------------------------

// A menu entry's shortcut runs it, on every platform: on macOS the menu
// bar takes those keys first, so they get here only while it's not there
// (--screenshot) or the entry is disabled, and then nothing runs twice.
static const Cmd* shortcut(const std::vector<Cmd>& items) {
  for (auto& c : items) {
    if (!c.sub.empty())
      if (const Cmd* s = shortcut(c.sub)) return s;
    if (c.run && c.enabled && !c.passthrough && !c.key.empty() && chord_pressed(c.key)) return &c;
  }
  return nullptr;
}

void Gui::keys() {
  ImGuiIO& io = ImGui::GetIO();
  bool mod = io.KeyCtrl || io.KeySuper;
  for (auto& mn : menus())
    if (const Cmd* c = shortcut(mn.items)) {
      c->run();
      return;
    }
  if (palette_open || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (settings_open) settings_open = false;
    else if (reader_focused) close_reader();
    return;
  }
  if (io.WantTextInput || mod || settings_open) return;
  bool reader = reader_focused;
  auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
  if (!reader) {
    if (pressed(ImGuiKey_DownArrow)) select(1);
    if (pressed(ImGuiKey_UpArrow)) select(-1);
    if (pressed(ImGuiKey_PageDown)) select(15);
    if (pressed(ImGuiKey_PageUp)) select(-15);
    if (pressed(ImGuiKey_Home)) select(-1 << 20);
    if (pressed(ImGuiKey_End)) select(1 << 20);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) act("⏎");
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
      switch_view(io.KeyShift ? (active + tabs.size() - 1) % tabs.size() : (active + 1) % tabs.size());
  } else {
    auto ds = docs();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) && !ds.empty())
      reader_tab = io.KeyShift ? (reader_tab + ds.size() - 1) % ds.size() : (reader_tab + 1) % ds.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) open(reader_url);
    if (pressed(ImGuiKey_DownArrow)) scroll_by += 60 * k();
    if (pressed(ImGuiKey_UpArrow)) scroll_by -= 60 * k();
    if (pressed(ImGuiKey_PageDown)) scroll_by += 1e6f;
    if (pressed(ImGuiKey_PageUp)) scroll_by -= 1e6f;
    if (pressed(ImGuiKey_Home)) scroll_by = -1e9f;
    if (pressed(ImGuiKey_End)) scroll_by = 1e9f;
  }
  for (ImWchar c : io.InputQueueCharacters) {
    if (c > 127) continue;
    std::string ch(1, static_cast<char>(c));
    if (reader) {
      auto ds = docs();
      if (ch == "q") close_reader();
      else if (ch == "j") scroll_by += 60 * k();
      else if (ch == "k") scroll_by -= 60 * k();
      else if (ch == " ") scroll_by += 1e6f;
      else if (ch == "g") scroll_by = -1e9f;
      else if (ch == "G") scroll_by = 1e9f;
      else if (ch == "]" && !ds.empty()) reader_tab = (reader_tab + 1) % ds.size();
      else if (ch == "[" && !ds.empty()) reader_tab = (reader_tab + ds.size() - 1) % ds.size();
      else if (ch == "J") focus_table(1);
      else if (ch == "K") focus_table(-1);
      else if (ch == "r") {
        const FileTab* ft = reader_tab_file();
        if (ft && ft->spec) {
          if (!focus.count(ft->path)) focus_table(0);
          if (focus.count(ft->path)) run_line(ft->path, focus[ft->path]);
        }
      } else if (ch == "w") open(reader_url);
      else if (ch == "y") act("y");
      else if (ch == "n") show_llm = focus_llm = true;
      else if (ch == "?") palette_open = palette_focus = true;
      else if (ch == ",") settings_open = true;
      else {
        const FileTab* ft = reader_tab_file();
        for (auto& [i, a] : plugin_keys())
          if (a.key == ch) {
            plugin_action(ch, reader_url, a.line && ft && ft->spec ? ft->path : "");
            break;
          }
      }
      continue;
    }
    if (ch == "j") select(1);
    else if (ch == "k") select(-1);
    else if (ch == "g") select(-1 << 20);
    else if (ch == "G") select(1 << 20);
    else if (ch >= "1" && ch <= "9") switch_view(static_cast<size_t>(c - '1'));
    else act(ch);
  }
}

}  // namespace anton::gui
