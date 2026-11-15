// The frame: menu bar, dock spaces and their default layout, status bar,
// keys, theme; and the drawing helpers the panes share. See gui.hpp.
#include "gui.hpp"

#include <cmath>
#include <cstdlib>
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
  s.FramePadding = ImVec2(10 * k(), 7 * k());
  s.ItemSpacing = ImVec2(8 * k(), 6 * k());
  s.ItemInnerSpacing = ImVec2(6 * k(), 4 * k());
  s.CellPadding = ImVec2(8 * k(), 0);
  s.WindowRounding = 8;
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

// A rounded pill: a dot (or a glyph) and the text, in a tone.
void Gui::pill(const std::string& s, Tone tn, float h, const char* glyph, bool dot) {
  ImGui::PushFont(f.medium, 12 * k());
  float H = h * k(), pad = (h > 22 ? 10 : 8) * k(), gap = 5 * k();
  ImVec2 ts = ImGui::CalcTextSize(s.c_str());
  float lead = 0;
  ImVec2 gs;
  if (glyph) {
    ImGui::PushFont(f.mono, 12 * k());
    gs = ImGui::CalcTextSize(glyph);
    ImGui::PopFont();
    lead = gs.x + gap;
  } else if (dot) {
    lead = 6 * k() + gap;
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 size(pad * 2 + lead + ts.x, H);
  ImDrawList* d = ImGui::GetWindowDrawList();
  d->AddRectFilled(p, p + size, col(tone_bg(tn)), H / 2);
  float x = p.x + pad;
  if (glyph) {
    ImGui::PushFont(f.mono, 12 * k());
    d->AddText(ImVec2(x, p.y + (H - gs.y) / 2), col(tone_fg(tn)), glyph);
    ImGui::PopFont();
  } else if (dot) {
    d->AddCircleFilled(ImVec2(x + 3 * k(), p.y + H / 2), 3 * k(), col(tone_fg(tn)));
  }
  x += lead;
  d->AddText(ImVec2(x, p.y + (H - ts.y) / 2), col(tone_fg(tn)), s.c_str());
  ImGui::Dummy(size);
  ImGui::PopFont();
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
  ImGui::PushFont(f.semibold, 11 * k() * size / 20);
  ImVec2 ts = ImGui::CalcTextSize(letter.c_str());
  d->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), dark_text_on(rgb) ? col(0x0d1117ffu) : IM_COL32_WHITE, letter.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(S, S));
}

void Gui::diffstat_blocks(long add, long del) {
  auto b = diffstat(add, del);
  ImVec2 p = ImGui::GetCursorScreenPos();
  float s = 7 * k(), gap = 1 * k();
  float y = p.y + (ImGui::GetTextLineHeight() - s) / 2;
  ImDrawList* d = ImGui::GetWindowDrawList();
  for (int i = 0; i < 5; ++i) {
    uint32_t c = b[static_cast<size_t>(i)] == 1 ? t.success : b[static_cast<size_t>(i)] == 2 ? t.danger : t.empty;
    float x = p.x + i * (s + gap);
    d->AddRectFilled(ImVec2(x, y), ImVec2(x + s, y + s), col(c), 1 * k());
  }
  ImGui::Dummy(ImVec2(5 * s + 4 * gap, ImGui::GetTextLineHeight()));
}

// A button as the canvas draws them: default (bordered, btn), primary
// (green, white text) or ghost (no border). key: a dim hint after the label.
float Gui::button_width(const std::string& label, Btn kind, const std::string& key, Icon icon) {
  ImGui::PushFont(f.medium, 0.f);
  float w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + (kind == Btn::Primary ? 28 : 24) * k();
  ImGui::PopFont();
  if (!key.empty()) w += f.mono->CalcTextSizeA(11 * k(), FLT_MAX, 0, key.c_str()).x + 6 * k();
  if (icon) w += 20 * k();
  return w;
}

bool Gui::button(const std::string& label, Btn kind, float h, const std::string& key, float w, Icon icon) {
  ImGui::PushFont(f.medium, 0.f);
  ImVec2 ts = ImGui::CalcTextSize(label.c_str(), nullptr, true);
  ImVec2 ks(0, 0);
  if (!key.empty()) {
    ImGui::PushFont(f.mono, 11 * k());
    ks = ImGui::CalcTextSize(key.c_str());
    ImGui::PopFont();
    ks.x += 6 * k();
  }
  float H = h * k(), pad = (kind == Btn::Primary ? 14 : 12) * k(), iw = icon ? 20 * k() : 0;
  ImVec2 size(w > 0 ? w * k() : ts.x + ks.x + iw + pad * 2, H);
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
  float x = p.x + (size.x - ts.x - ks.x - iw) / 2;
  if (icon) {
    float is = icon == icon_play ? 10 * k() : 14 * k();
    icon(d, ImVec2(x + (14 * k() - is) / 2, p.y + (H - is) / 2), is, col(icon == icon_play && kind != Btn::Primary ? t.success : fg));
    x += iw;
  }
  const char* end = ImGui::FindRenderedTextEnd(label.c_str());
  d->AddText(ImVec2(x, p.y + (H - ts.y) / 2), col(fg), label.c_str(), end);
  if (!key.empty()) {
    ImGui::PushFont(f.mono, 11 * k());
    d->AddText(ImVec2(x + ts.x + 6 * k(), p.y + (H - ks.y) / 2), col(kind == Btn::Primary ? 0xffffffccu : t.muted),
               key.c_str());
    ImGui::PopFont();
  }
  ImGui::PopFont();
  return clicked;
}

bool Gui::icon_button(const char* id, void (*icon)(ImDrawList*, ImVec2, float, ImU32), float size, bool bordered,
                      const char* tip) {
  float S = size * k();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool clicked = ImGui::InvisibleButton(id, ImVec2(S, S));
  bool hov = ImGui::IsItemHovered();
  ImDrawList* d = ImGui::GetWindowDrawList();
  if (bordered || hov) d->AddRectFilled(p, ImVec2(p.x + S, p.y + S), col(hov ? (t.dark ? t.border_muted : t.inset) : t.btn), 6 * k());
  if (bordered) d->AddRect(p, ImVec2(p.x + S, p.y + S), col(t.border), 6 * k());
  float is = 14 * k();
  g_icon_bg = col(bordered || hov ? (hov ? (t.dark ? t.border_muted : t.inset) : t.btn) : t.bg);
  icon(d, ImVec2(p.x + (S - is) / 2, p.y + (S - is) / 2), is, col(bordered ? t.fg : t.muted));
  if (tip && hov) ImGui::SetTooltip("%s", tip);
  return clicked;
}

void Gui::section(const std::string& title) { text(title, t.muted, f.semibold, 12); }

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

void icon_back(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.6f * s / 16;
  d->AddLine(at(p, s, 10, 3), at(p, s, 5, 8), c, w);
  d->AddLine(at(p, s, 5, 8), at(p, s, 10, 13), c, w);
}

void icon_sliders(ImDrawList* d, ImVec2 p, float s, ImU32 c) {
  float w = 1.5f * s / 16;
  d->AddLine(at(p, s, 2, 4), at(p, s, 9, 4), c, w);
  d->AddLine(at(p, s, 12, 4), at(p, s, 14, 4), c, w);
  d->AddLine(at(p, s, 2, 12), at(p, s, 4, 12), c, w);
  d->AddLine(at(p, s, 7, 12), at(p, s, 14, 12), c, w);
  d->AddCircle(at(p, s, 10.5f, 4), 1.5f * s / 16, c, 0, w);
  d->AddCircle(at(p, s, 5.5f, 12), 1.5f * s / 16, c, 0, w);
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
  if (screen == Screen::Reader && !cache.count(reader_url)) screen = Screen::List;

  ImGui::PushFont(f.regular, 0.f);
  menu_bar();
  if (cfg.gui.status_bar) status_bar();
  dock_spaces();
  if (settings_open) settings_window();
  else settings_shown = false;
  if (palette_open) palette();
  keys();
  ImGui::PopFont();
}

void Gui::menu_bar() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k(), 10 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2 * k(), 6 * k()));
  ImGui::PushStyleColor(ImGuiCol_Border, col(t.border));
  if (!ImGui::BeginMainMenuBar()) {
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return;
  }
  ImGui::PopStyleVar(2);
  ImDrawList* d = ImGui::GetWindowDrawList();
  float H = ImGui::GetWindowHeight();
  ImVec2 wp = ImGui::GetWindowPos();
  d->AddLine(ImVec2(wp.x, wp.y + H - 1), ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + H - 1), col(t.border));
  // the logo and the name
  ImGui::SetCursorPosX(12 * k());
  ImVec2 p = ImGui::GetCursorScreenPos();
  icon_logo(d, ImVec2(p.x, wp.y + (H - 18 * k()) / 2), 18 * k(), col(t.accent));
  ImGui::SetCursorPosX(12 * k() + 26 * k());
  ImGui::PushFont(f.semibold, 14 * k());
  ImGui::TextUnformatted("anton");
  ImGui::PopFont();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10 * k());
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 8 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k(), 4 * k()));
  const Pr* cur = screen == Screen::Reader && cache.count(reader_url) ? &cache.at(reader_url) : selected_pr();
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Settings…", "Ctrl ,")) settings_open = true;
    if (ImGui::MenuItem("Open config.json")) open_config();
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Ctrl Q")) glfwSetWindowShouldClose(window, 1);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Edit")) {
    if (ImGui::MenuItem("Copy URL", "y", false, cur)) act("y");
    if (ImGui::MenuItem("Edit the query", "/")) act("/");
    if (ImGui::MenuItem("Clear org/repo", "x", false, !scope.empty())) act("x");
    ImGui::Separator();
    if (ImGui::MenuItem("Command palette", "Ctrl K")) palette_open = palette_focus = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    if (ImGui::MenuItem("Pull requests", nullptr, screen == Screen::List)) screen = Screen::List;
    if (ImGui::MenuItem("Reader", "Enter", screen == Screen::Reader, cur != nullptr) && cur) open_reader(*cur);
    ImGui::Separator();
    if (ImGui::BeginMenu("Panes")) {
      bool changed = ImGui::MenuItem("Views", nullptr, &cfg.gui.views);
      changed |= ImGui::MenuItem("Preview", nullptr, &cfg.gui.preview);
      changed |= ImGui::MenuItem("Run log (in the reader)", nullptr, &cfg.gui.run_log);
      changed |= ImGui::MenuItem("Status bar", nullptr, &cfg.gui.status_bar);
      if (changed) save();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Theme")) {
      for (auto [id, name] : {std::pair{"dark", "Dark"}, {"light", "Light"}, {"system", "Match system"}})
        if (ImGui::MenuItem(name, nullptr, cfg.gui.theme == id)) set_theme(id);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Bigger text", "Ctrl +", false, cfg.gui.font_size < 18)) {
      ++cfg.gui.font_size;
      save();
    }
    if (ImGui::MenuItem("Smaller text", "Ctrl -", false, cfg.gui.font_size > 11)) {
      --cfg.gui.font_size;
      save();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset layout")) reset_main = reset_reader = true;
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Pull requests")) {
    if (ImGui::MenuItem("Open in reader", "Enter", false, cur) && cur) open_reader(*cur);
    if (ImGui::MenuItem("Open on GitHub", "w", false, cur)) act("w");
    if (ImGui::MenuItem("Copy URL", "y", false, cur)) act("y");
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh", "r")) refresh();
    if (ImGui::BeginMenu("Sort")) {
      for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
        if (s == Sort::List && !view().is_list()) continue;
        if (ImGui::MenuItem(s == Sort::List ? "list order" : sort_name(s), nullptr, view().sort_or_default() == s)) set_sort(s);
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    for (size_t i = 0; i < tabs.size(); ++i) {
      std::string key = i < 9 ? std::to_string(i + 1) : "";
      if (ImGui::MenuItem(tabs[i].v.name.c_str(), key.c_str(), i == active)) switch_view(i);
    }
    if (ImGui::MenuItem("New view", "+")) act("+");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Plugins")) {
    if (cfg.plugins.empty()) ImGui::MenuItem("No plugins configured", nullptr, false, false);
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      if (ImGui::BeginMenu(cfg.plugins[i].name.c_str())) {
        for (auto& [pi, a] : plugin_keys())
          if (pi == i && ImGui::MenuItem(strip_emoji(a.button.empty() ? a.label : a.button).c_str(), a.key.c_str(), false, cur) && cur)
            plugin_action(a.key, cur->url);
        if (ImGui::MenuItem("Restart") && plugins) plugins->restart(i);
        ImGui::EndMenu();
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Plugin settings…")) {
      settings_open = true;
      settings_page = 3;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("Command palette", "Ctrl K")) palette_open = palette_focus = true;
    if (ImGui::MenuItem("anton on GitHub")) open_file("https://github.com/Thinato/anton");
    ImGui::Separator();
    ImGui::MenuItem((std::string("anton-gui ") + kVersion).c_str(), nullptr, false, false);
    ImGui::EndMenu();
  }
  ImGui::PopStyleVar(3);

  // the search box, centered; then the refresh status and the theme button
  float W = ImGui::GetWindowWidth(), sw = 300 * k(), sh = 26 * k();
  float sx = std::max(ImGui::GetCursorPosX() + 16 * k(), (W - sw) / 2);
  ImGui::SetCursorPos(ImVec2(sx, (H - sh) / 2));
  ImVec2 sp = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##search", ImVec2(sw, sh))) palette_open = palette_focus = true;
  bool hov = ImGui::IsItemHovered();
  d->AddRectFilled(sp, sp + ImVec2(sw, sh), col(t.bg), 6 * k());
  d->AddRect(sp, sp + ImVec2(sw, sh), col(hov ? t.muted : t.border), 6 * k());
  icon_search(d, ImVec2(sp.x + 8 * k(), sp.y + (sh - 14 * k()) / 2), 14 * k(), col(t.muted));
  float ty = sp.y + (sh - ImGui::GetTextLineHeight()) / 2;
  d->AddText(ImVec2(sp.x + 30 * k(), ty), col(t.muted), "Search or run a command");
  {
    ImGui::PushFont(f.mono, 11 * k());
    const char* kb = "Ctrl K";
    ImVec2 ks = ImGui::CalcTextSize(kb);
    ImVec2 kp(sp.x + sw - ks.x - 18 * k(), sp.y + (sh - 16 * k()) / 2);
    d->AddRect(kp, kp + ImVec2(ks.x + 10 * k(), 16 * k()), col(t.border), 4 * k());
    d->AddText(ImVec2(kp.x + 5 * k(), kp.y + (16 * k() - ks.y) / 2), col(t.muted), kb);
    ImGui::PopFont();
  }
  std::string status = loading() ? "Refreshing…" : last_fetch ? "Refreshed " + age(now() - last_fetch) + " ago" : "Loading…";
  if (status == "Refreshed 0m ago") status = "Refreshed just now";
  ImVec2 st = ImGui::CalcTextSize(status.c_str());
  float bw = 28 * k(), rw = st.x + 16 * k() + 22 * k();
  ImGui::SetCursorPos(ImVec2(W - 8 * k() - bw - 6 * k() - rw, (H - 26 * k()) / 2));
  ImVec2 rp = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##refresh", ImVec2(rw, 26 * k()))) refresh();
  if (ImGui::IsItemHovered()) {
    d->AddRectFilled(rp, rp + ImVec2(rw, 26 * k()), col(t.dark ? t.border_muted : t.inset), 6 * k());
    ImGui::SetTooltip("Refresh now (r) · every %ds", cfg.refresh_seconds);
  }
  icon_refresh(d, ImVec2(rp.x + 8 * k(), rp.y + 6 * k()), 14 * k(), col(t.muted));
  d->AddText(ImVec2(rp.x + 28 * k(), rp.y + (26 * k() - st.y) / 2), col(t.muted), status.c_str());
  ImGui::SetCursorPos(ImVec2(W - 8 * k() - bw, (H - bw) / 2));
  bool dark = t.dark;
  if (icon_button("##theme", dark ? icon_sun : icon_moon, 28, true, dark ? "Switch to light mode" : "Switch to dark mode"))
    set_theme(dark ? "light" : "dark");
  ImGui::EndMainMenuBar();
  ImGui::PopStyleColor();
}

void Gui::status_bar() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.panel));
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
  if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, 26 * k(), fl)) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    d->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), col(t.border));
    ImGui::PushFont(f.regular, 12 * k());
    float y = (26 * k() - ImGui::GetTextLineHeight()) / 2;
    ImGui::SetCursorPos(ImVec2(12 * k(), y));
    auto item = [&](const std::string& s, uint32_t c, uint32_t dot = 0) {
      if (dot) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        d->AddCircleFilled(ImVec2(p.x + 3.5f * k(), p.y + ImGui::GetTextLineHeight() / 2), 3.5f * k(), col(dot));
        ImGui::Dummy(ImVec2(7 * k(), 1));
        ImGui::SameLine(0, 6 * k());
      }
      text(s, c);
      ImGui::SameLine(0, 16 * k());
    };
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      std::string problem = plugin_problem(i), name = cfg.plugins[i].name;
      uint32_t dot = !problem.empty() ? t.danger : live[i].health == "ok" ? t.success : t.attention;
      for (auto& [url, info] : live[i].prs)
        if (info.marks_summary && result_of(*info.marks_summary) == Result::Running) {
          dot = t.accent;
          if (url == reader_url || screen == Screen::List) name += " · running";
          break;
        }
      item(name, t.muted, dot);
    }
    if (screen == Screen::Reader) {
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
    for (auto& [s, c] : right) w += ImGui::CalcTextSize(s.c_str()).x + 16 * k();
    float x = std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w + 4 * k());
    ImGui::SetCursorPos(ImVec2(x, y));
    for (auto& [s, c] : right) item(s, c);
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// Was this dock space laid out before (by us, or by the user in the layout
// file)? A window's remembered DockId says so.
static bool laid_out(ImGuiID id, std::initializer_list<const char*> windows) {
  ImGuiDockNode* n = ImGui::DockBuilderGetNode(id);
  if (n && n->IsSplitNode()) return true;
  for (auto w : windows)
    if (ImGuiWindowSettings* s = ImGui::FindWindowSettingsByID(ImHashStr(w)); s && s->DockId) return true;
  return false;
}

void Gui::build_main_layout(ImGuiID id, ImVec2 size) {
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, size);
  ImGuiID center = id;
  ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 224 * k() / size.x, nullptr, &center);
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 392 * k() / (size.x - 224 * k()), nullptr, &center);
  ImGui::DockBuilderDockWindow("###views", left);
  ImGui::DockBuilderDockWindow("###preview", right);
  ImGui::DockBuilderDockWindow("###prs", center);
  ImGui::DockBuilderFinish(id);
}

void Gui::build_reader_layout(ImGuiID id, ImVec2 size) {
  ImGui::DockBuilderRemoveNode(id);
  ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(id, size);
  ImGuiID center = id;
  ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 360 * k() / size.x, nullptr, &center);
  ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 172 * k() / size.y, nullptr, &center);
  ImGui::DockBuilderDockWindow("###reader", center);
  ImGui::DockBuilderDockWindow("###results", right);
  ImGui::DockBuilderDockWindow("###outline", right);
  ImGui::DockBuilderDockWindow("###llm", right);
  ImGui::DockBuilderDockWindow("###runlog", bottom);
  ImGui::DockBuilderDockWindow("###problems", bottom);
  ImGui::DockBuilderFinish(id);
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
  ImGuiID main_id = ImGui::GetID("MainDock"), reader_id = ImGui::GetID("ReaderDock");
  ImVec2 size = ImGui::GetContentRegionAvail();
  bool list = screen == Screen::List;
  if (list && (reset_main || !laid_out(main_id, {"###views", "###prs", "###preview"}))) {
    build_main_layout(main_id, size);
    reset_main = false;
  }
  if (!list && (reset_reader || !laid_out(reader_id, {"###reader", "###results", "###runlog"}))) {
    build_reader_layout(reader_id, size);
    reset_reader = false;
    pick_tabs = 3;
  }
  ImGui::DockSpace(main_id, size, list ? 0 : ImGuiDockNodeFlags_KeepAliveOnly);
  ImGui::DockSpace(reader_id, size, list ? ImGuiDockNodeFlags_KeepAliveOnly : 0);
  ImGui::End();

  if (list) {
    if (cfg.gui.views) views_pane();
    prs_pane();
    if (cfg.gui.preview) preview_pane();
  } else {
    reader_pane();
    results_pane();
    outline_pane();
    llm_pane();
    if (cfg.gui.run_log) {
      runlog_pane();
      problems_pane();
    }
    // a rebuilt layout opens on Results and Run log, the canvas's tabs
    if (pick_tabs > 0) {
      --pick_tabs;
      for (const char* name : {"Results###results", "Run log###runlog", "Reader###reader"}) ImGui::SetWindowFocus(name);
    }
  }
}

// -- keys ---------------------------------------------------------------------------------

void Gui::keys() {
  ImGuiIO& io = ImGui::GetIO();
  bool mod = io.KeyCtrl || io.KeySuper;
  if (mod && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
    palette_open = !palette_open;
    palette_focus = palette_open;
    palette_q.clear();
    palette_sel = 0;
    return;
  }
  if (mod && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) settings_open = true;
  if (mod && ImGui::IsKeyPressed(ImGuiKey_Q, false)) glfwSetWindowShouldClose(window, 1);
  if (mod && (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) &&
      cfg.gui.font_size < 18) {
    ++cfg.gui.font_size;
    save();
  }
  if (mod && (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) &&
      cfg.gui.font_size > 11) {
    --cfg.gui.font_size;
    save();
  }
  if (palette_open || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    if (settings_open) settings_open = false;
    else if (screen == Screen::Reader) screen = Screen::List;
    return;
  }
  if (io.WantTextInput || mod || settings_open) return;
  bool reader = screen == Screen::Reader;
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
      if (ch == "q") screen = Screen::List;
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
      else if (ch == "n") focus_llm = true;
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
