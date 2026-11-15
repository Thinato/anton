// The Settings window (a floating window: drag its title bar, resize it by
// a corner) and the command palette (Ctrl/Cmd K). Settings write
// config.json as they change, through config_write like the TUI's.
#include <algorithm>
#include <filesystem>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "gui.hpp"
#include "update.hpp"

namespace anton::gui {

namespace {

// A plugin's own settings file, to open from Settings › Plugins: what its
// command passes as --config, else anton-llm's and anton-temoc's defaults.
std::string plugin_file(const PluginConfig& p) {
  for (size_t i = 0; i + 1 < p.command.size(); ++i)
    if (p.command[i] == "--config") return p.command[i + 1];
  std::string prog = p.command.empty() ? "" : std::filesystem::path(p.command[0]).filename().string();
  if (prog == "anton-llm") return config_dir() + "/llm.json";
  if (prog == "anton-temoc") return config_dir() + "/temoc.json";
  return "";
}

std::string seconds_label(int s) {
  if (s % 60 == 0) return std::to_string(s / 60) + (s == 60 ? " minute" : " minutes");
  return std::to_string(s) + " seconds";
}

std::string capital(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

}  // namespace

// -- settings -----------------------------------------------------------------------------

void Gui::settings_window() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(860 * k(), 620 * k());
  // the rest of the window dims behind it (the canvas's scrim), with a shadow
  bool appearing = !settings_shown;
  settings_shown = true;
  {
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.scrim));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    if (appearing) ImGui::SetNextWindowFocus();
    ImGui::Begin("##setscrim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoDocking);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) settings_open = false;  // a click outside closes it
    if (ImGuiWindow* sw = ImGui::FindWindowByName("Settings###settings"); sw && !appearing)
      for (int i = 1; i <= 8; ++i)
        ImGui::GetWindowDrawList()->AddRectFilled(sw->Pos - ImVec2(i * 2.f, i * 1.f - 10 * k()), sw->Pos + sw->Size + ImVec2(i * 2.f, i * 3.f),
                                                  col((t.shadow & 0xffffff00u) | ((t.shadow & 0xff) / 12)), 8 + i * 2.f);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }
  if (appearing) ImGui::SetNextWindowFocus();
  ImGui::SetNextWindowSize(size, screenshot ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2),
                          screenshot ? ImGuiCond_Always : ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(640 * k(), 420 * k()), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * k());
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.bg));
  ImGui::PushStyleColor(ImGuiCol_Border, col(t.border));
  bool shown = ImGui::Begin("Settings###settings", &settings_open,
                           ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoCollapse | (screenshot ? ImGuiWindowFlags_NoSavedSettings : 0));
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
  if (!shown) return ImGui::End();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  // the title bar: drag it to move the window
  float th = 36 * k();
  d->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + th), col(t.panel), 8 * k(), ImDrawFlags_RoundCornersTop);
  d->AddLine(ImVec2(wp.x, wp.y + th), ImVec2(wp.x + ws.x, wp.y + th), col(t.border));
  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::InvisibleButton("##drag", ImVec2(ws.x - 40 * k(), th));
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    ImGui::SetWindowPos(wp + ImGui::GetIO().MouseDelta);
  icon_sliders(d, ImVec2(wp.x + 14 * k(), wp.y + (th - 14 * k()) / 2), 14 * k(), col(t.muted));
  d->AddText(f.semibold, ImGui::GetFontSize(), ImVec2(wp.x + 36 * k(), wp.y + (th - ImGui::GetTextLineHeight()) / 2), col(t.fg), "Settings");
  {
    const char* hint = "drag the title bar to move · drag a corner to resize";
    ImVec2 hs = f.regular->CalcTextSizeA(12 * k(), FLT_MAX, 0, hint);
    d->AddText(f.regular, 12 * k(), ImVec2(wp.x + ws.x - 44 * k() - hs.x, wp.y + (th - hs.y) / 2), col(t.muted), hint);
  }
  ImGui::SetCursorPos(ImVec2(ws.x - 36 * k(), 4 * k()));
  if (icon_button("##close", icon_cross, 28, false, "Close (Esc)")) settings_open = false;

  // the sections, on the left
  float nav_w = 196 * k(), foot = 52 * k();
  ImVec2 body0(wp.x, wp.y + th + 1), body1(wp.x + ws.x, wp.y + ws.y - foot);
  d->AddRectFilled(body0, ImVec2(body0.x + nav_w, body1.y), col(t.panel));
  d->AddLine(ImVec2(body0.x + nav_w, body0.y), ImVec2(body0.x + nav_w, body1.y), col(t.border));
  const char* names[] = {"General", "Appearance", "Views", "Plugins"};
  std::string hints[] = {"", "", std::to_string(tabs.size()), std::to_string(cfg.plugins.size())};
  for (int i = 0; i < 4; ++i) {
    ImGui::SetCursorPos(ImVec2(8 * k(), th + 10 * k() + i * 34 * k()));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##nav", ImVec2(nav_w - 16 * k(), 32 * k()))) settings_page = i;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (settings_page == i) d->AddRectFilled(p, p + ImVec2(nav_w - 16 * k(), 32 * k()), col(t.sel), 6 * k());
    else if (hov) d->AddRectFilled(p, p + ImVec2(nav_w - 16 * k(), 32 * k()), col(t.dark ? t.border_muted : t.inset), 6 * k());
    float ty = p.y + (32 * k() - ImGui::GetTextLineHeight()) / 2;
    d->AddText(settings_page == i ? f.semibold : f.regular, ImGui::GetFontSize(), ImVec2(p.x + 10 * k(), ty), col(t.fg), names[i]);
    if (!hints[i].empty()) {
      ImVec2 hs = f.regular->CalcTextSizeA(12 * k(), FLT_MAX, 0, hints[i].c_str());
      d->AddText(f.regular, 12 * k(), ImVec2(p.x + nav_w - 26 * k() - hs.x, p.y + (32 * k() - hs.y) / 2), col(t.muted), hints[i].c_str());
    }
  }

  // the page
  ImGui::SetCursorPos(ImVec2(nav_w + 1, th + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * k(), 20 * k()));
  ImGui::BeginChild("##page", ImVec2(ws.x - nav_w - 1, ws.y - th - foot - 2), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 6 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k(), 6 * k()));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, col(t.btn));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col(t.dark ? t.border_muted : t.inset));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col(t.dark ? t.border_muted : t.inset));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, col(0xffffffffu));
  float W = ImGui::GetContentRegionAvail().x, half = (W - 24 * k()) / 2;
  auto heading = [&](const std::string& title, const std::string& sub) {
    text(title, t.fg, f.semibold, 16);
    if (!sub.empty()) text_wrapped(sub, t.muted, nullptr, 0, W);
    ImGui::Dummy(ImVec2(1, 4 * k()));
  };
  auto label = [&](const std::string& s) { text(s, t.fg, f.semibold); };
  auto check = [&](const char* id, bool v, const std::string& s) {  // a checkbox drawn accent when on
    ImVec2 p = ImGui::GetCursorScreenPos();
    float b = 16 * k();
    std::string lid = std::string("##") + id;
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    bool clicked = ImGui::InvisibleButton(lid.c_str(), ImVec2(b + 8 * k() + ts.x, std::max(b, ts.y)));
    float y = p.y + (std::max(b, ts.y) - b) / 2;
    if (v) {
      d = ImGui::GetWindowDrawList();
      d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + b, y + b), col(t.accent), 3 * k());
      d->AddLine(ImVec2(p.x + 3.5f * k(), y + 8 * k()), ImVec2(p.x + 6.5f * k(), y + 11 * k()), IM_COL32_WHITE, 1.8f * k());
      d->AddLine(ImVec2(p.x + 6.5f * k(), y + 11 * k()), ImVec2(p.x + 12.5f * k(), y + 4.5f * k()), IM_COL32_WHITE, 1.8f * k());
    } else {
      ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x, y), ImVec2(p.x + b, y + b), col(t.muted), 3 * k(), 1.2f * k());
    }
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + b + 8 * k(), p.y + (std::max(b, ts.y) - ts.y) / 2), col(t.fg), s.c_str());
    return clicked;
  };
  auto combo = [&](const char* id, const std::string& preview, float w, const std::function<void()>& items) {
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo(id, preview.c_str())) {
      items();
      ImGui::EndCombo();
    }
  };
  d = ImGui::GetWindowDrawList();

  if (settings_page == 0) {  // General
    heading("General", "What anton opens on and how often it looks again.");
    ImGui::BeginGroup();
    label("Open at start");
    std::string start = cfg.start_view.empty() && !tabs.empty() ? tabs[0].v.name : cfg.start_view;
    combo("##start", start, half, [&] {
      for (size_t i = 0; i < tabs.size(); ++i) {
        if (!tabs[i].saved) continue;
        if (ImGui::Selectable(tabs[i].v.name.c_str(), start == tabs[i].v.name)) {
          cfg.start_view = i == 0 ? "" : tabs[i].v.name;
          save("saved");
        }
      }
    });
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * k());
    ImGui::BeginGroup();
    label("Refresh every");
    combo("##refresh", seconds_label(cfg.refresh_seconds), half, [&] {
      for (int s : {30, 60, 120, 300, 600, 1800})
        if (ImGui::Selectable(seconds_label(s).c_str(), cfg.refresh_seconds == s)) {
          cfg.refresh_seconds = s;
          save("saved");
        }
    });
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 10 * k()));
    ImGui::BeginGroup();
    label("Check watched PRs every");
    combo("##watch", seconds_label(cfg.watch_seconds), half, [&] {
      for (int s : {5, 10, 15, 30, 60})
        if (ImGui::Selectable(seconds_label(s).c_str(), cfg.watch_seconds == s)) {
          cfg.watch_seconds = s;
          save("saved");
        }
    });
    text("PRs a plugin marks as watched, polled quietly", t.muted, nullptr, 12);
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * k());
    ImGui::BeginGroup();
    label("Updates");
    if (check("autoupdate", cfg.auto_update, "Install new releases of anton")) {
      cfg.auto_update = !cfg.auto_update;
      save("saved");
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 14 * k()));
    bool strip = !cfg.title_strip.empty();
    if (check("strip", strip, "Strip ticket prefixes from titles")) {
      cfg.title_strip = strip ? "" : "^\\[[A-Z]+-\\d+\\]\\s*";
      cfg.title_re = std::regex(cfg.title_strip.empty() ? "$^" : cfg.title_strip);
      save("saved");
    }
    if (!cfg.title_strip.empty()) {
      static std::string re_err;
      ImGui::Indent(24 * k());
      text("The pattern cut from every title (a regular expression):", t.muted, nullptr, 12);
      std::string buf = cfg.title_strip;
      ImGui::PushFont(f.mono, 12 * k());
      ImGui::SetNextItemWidth(W - 24 * k());
      if (input("##strip", buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        try {
          cfg.title_re = std::regex(buf);
          cfg.title_strip = buf;
          re_err.clear();
          save("saved");
        } catch (const std::regex_error& e) {
          re_err = std::string("bad regex: ") + e.what();
        }
      }
      ImGui::PopFont();
      if (!re_err.empty()) text(re_err, t.danger, nullptr, 12);
      for (auto& [url, pr] : cache)
        if (!pr.partial && shown_title(pr.title) != pr.title) {
          segments({{pr.title, t.muted}, {"  →  ", t.muted}, {shown_title(pr.title), t.fg}}, nullptr, 12);
          break;
        }
      ImGui::Unindent(24 * k());
    }
    ImGui::Dummy(ImVec2(1, 14 * k()));
    text_wrapped("The mouse setting and the columns' size colors are the terminal UI's; the rest of config.json "
                 "(plugins, people, command columns) is edited by hand: Open config file below.",
                 t.muted, nullptr, 12, W);
  } else if (settings_page == 1) {  // Appearance
    heading("Theme", "GitHub's light and dark colors. Match system follows your OS.");
    float cw = (W - 24 * k()) / 3;
    struct Card {
      const char* id;
      const char* name;
      std::vector<bool> halves;  // dark?
    };
    Card cards[] = {{"dark", "Dark", {true}}, {"light", "Light", {false}}, {"system", "Match system", {false, true}}};
    for (int ci = 0; ci < 3; ++ci) {
      if (ci) ImGui::SameLine(0, 12 * k());
      Card& c = cards[ci];
      bool on = cfg.gui.theme == c.id;
      ImVec2 p = ImGui::GetCursorScreenPos();
      float ch = 92 * k() + 16 * k() + 28 * k();
      ImGui::PushID(ci);
      if (ImGui::InvisibleButton("##card", ImVec2(cw, ch))) set_theme(c.id);
      bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      d->AddRectFilled(p, p + ImVec2(cw, ch), col(t.bg), 8 * k());
      d->AddRect(p, p + ImVec2(cw, ch), col(on ? t.accent : hov ? t.muted : t.border), 8 * k(), 2 * k());
      // a little window in that theme's colors
      ImVec2 m0 = p + ImVec2(8 * k(), 8 * k()), m1 = ImVec2(p.x + cw - 8 * k(), m0.y + 92 * k());
      float hw = (m1.x - m0.x) / c.halves.size();
      for (size_t h = 0; h < c.halves.size(); ++h) {
        Theme x = make_theme(c.halves[h]);
        ImVec2 a(m0.x + h * hw, m0.y), z(m0.x + (h + 1) * hw, m1.y);
        d->AddRectFilled(a, z, col(x.border));
        d->AddRectFilled(a, ImVec2(z.x, a.y + 14 * k()), col(x.panel));
        float sx = a.x + (z.x - a.x) * 0.26f;
        d->AddRectFilled(ImVec2(a.x, a.y + 15 * k()), ImVec2(sx, z.y), col(x.bg));
        d->AddRectFilled(ImVec2(sx + 1, a.y + 15 * k()), z, col(x.bg));
        auto bar = [&](float x0, float y, float w, uint32_t c2) {
          d->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + w, y + 5 * k()), col(c2), 3 * k());
        };
        float sw = sx - a.x - 8 * k();
        bar(a.x + 4 * k(), a.y + 21 * k(), sw, x.sel);
        bar(a.x + 4 * k(), a.y + 30 * k(), sw * 0.7f, x.border_muted);
        bar(a.x + 4 * k(), a.y + 39 * k(), sw * 0.8f, x.border_muted);
        float mw = z.x - sx - 13 * k();
        bar(sx + 7 * k(), a.y + 21 * k(), mw, x.border_muted);
        bar(sx + 7 * k(), a.y + 31 * k(), mw * 0.6f, x.success);
        bar(sx + 7 * k(), a.y + 41 * k(), mw, x.border_muted);
        bar(sx + 7 * k(), a.y + 51 * k(), mw * 0.4f, x.danger);
      }
      d->AddRect(m0, m1, col(t.border), 6 * k());
      ImVec2 dc(p.x + 15 * k(), m1.y + 8 * k() + 10 * k());
      d->AddCircle(dc, 7 * k(), col(on ? t.accent : t.muted), 0, 1.2f * k());
      if (on) d->AddCircleFilled(dc, 4 * k(), col(t.accent));
      d->AddText(f.medium, ImGui::GetFontSize(), ImVec2(dc.x + 14 * k(), dc.y - ImGui::GetTextLineHeight() / 2), col(t.fg), c.name);
    }
    ImGui::Dummy(ImVec2(1, 16 * k()));
    ImGui::BeginGroup();
    label("Interface size");
    int sz = cfg.gui.font_size;
    ImGui::SetNextItemWidth(half - 56 * k());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col(t.border_muted));
    if (ImGui::SliderInt("##size", &sz, 11, 18, "")) cfg.gui.font_size = sz;
    if (ImGui::IsItemDeactivatedAfterEdit()) save("saved");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    text(std::to_string(cfg.gui.font_size) + " px", t.muted, f.mono, 12);
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * k());
    ImGui::BeginGroup();
    label("Code font");
    combo("##codefont", cfg.gui.code_font, half, [&] {
      for (const char* fnt : {"JetBrains Mono", "System monospace"})
        if (ImGui::Selectable(fnt, cfg.gui.code_font == fnt)) {
          cfg.gui.code_font = fnt;
          save("saved");
        }
    });
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 10 * k()));
    label("Row density");
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      float x = p.x, h = 30 * k();
      for (int i = 0; i < 2; ++i) {
        const char* name = i == 0 ? "Compact" : "Comfortable";
        bool on = cfg.gui.density == (i == 0 ? "compact" : "comfortable");
        float w = ImGui::CalcTextSize(name).x + 28 * k();
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##density", ImVec2(w, h))) {
          cfg.gui.density = i == 0 ? "compact" : "comfortable";
          save("saved");
        }
        ImGui::PopID();
        d->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h), col(on ? t.sel : t.btn), 6 * k(),
                         i == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
        d->AddText(on ? f.semibold : f.regular, ImGui::GetFontSize(), ImVec2(x + 14 * k(), p.y + (h - ImGui::GetTextLineHeight()) / 2),
                   col(t.fg), name);
        if (i) d->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), col(t.border));
        x += w;
      }
      d->AddRect(p, ImVec2(x, p.y + h), col(t.border), 6 * k());
    }
    ImGui::Dummy(ImVec2(1, 18 * k()));
    ImVec2 lp = ImGui::GetCursorScreenPos();
    d->AddLine(lp, ImVec2(lp.x + W, lp.y), col(t.border_muted));
    ImGui::Dummy(ImVec2(1, 18 * k()));
    heading("Panes", "Drag a pane's tab to dock it somewhere else; drag the line between panes to resize.");
    struct PaneOpt {
      const char* name;
      bool* v;
    } panes[] = {{"Views", &cfg.gui.views},
                 {"Preview", &cfg.gui.preview},
                 {"Run log (in the reader)", &cfg.gui.run_log},
                 {"Status bar", &cfg.gui.status_bar}};
    for (int i = 0; i < 4; ++i) {
      if (i % 2) ImGui::SameLine(half + 24 * k() + ImGui::GetStyle().WindowPadding.x);
      if (check(panes[i].name, *panes[i].v, panes[i].name)) {
        *panes[i].v = !*panes[i].v;
        save("saved");
      }
    }
    ImGui::Dummy(ImVec2(1, 8 * k()));
    if (button("Reset layout", Btn::Default, 30)) {
      reset_main = reset_reader = true;
      flash("layout reset");
    }
  } else if (settings_page == 2) {  // Views
    heading("Views", "Each view is a tab in the PR list. Drag to reorder.");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float y = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImVec2 p = ImGui::GetCursorScreenPos();
      if (i) d->AddLine(p, ImVec2(p.x + W, p.y), col(t.border_muted));
      float h = 46 * k();
      ImGui::SetCursorScreenPos(p + ImVec2(10 * k(), 0));
      ImGui::InvisibleButton("##grip", ImVec2(20 * k(), h));
      if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        int idx = static_cast<int>(i);
        ImGui::SetDragDropPayload("view", &idx, sizeof idx);
        ImGui::EndDragDropSource();
      }
      icon_grip(d, ImVec2(p.x + 12 * k(), p.y + (h - 12 * k()) / 2), 12 * k(), col(t.muted));
      ImGui::SetCursorScreenPos(p);
      ImGui::InvisibleButton("##drop", ImVec2(W - 80 * k(), h));
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("view")) {
          int from = *static_cast<const int*>(pl->Data);
          while (from != static_cast<int>(i)) {
            move_view(static_cast<size_t>(from), from < static_cast<int>(i) ? 1 : -1);
            from += from < static_cast<int>(i) ? 1 : -1;
          }
        }
        ImGui::EndDragDropTarget();
      }
      float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
      ImGui::PushClipRect(p, ImVec2(p.x + W - 80 * k(), p.y + h), true);
      d->AddText(f.medium, ImGui::GetFontSize(), ImVec2(p.x + 40 * k(), ty), col(t.fg), tabs[i].v.name.c_str());
      std::string what = tabs[i].v.is_list() ? tabs[i].v.plugin + ":" + tabs[i].v.list + "  " + tabs[i].v.query : tabs[i].v.query;
      d->AddText(f.mono, 12 * k(), ImVec2(p.x + 190 * k(), p.y + (h - 12 * k() * 1.25f) / 2), col(t.muted), what.c_str());
      ImGui::PopClipRect();
      ImGui::SetCursorScreenPos(ImVec2(p.x + W - 64 * k(), p.y + (h - 26 * k()) / 2));
      ImGui::PushFont(nullptr, 12 * k());
      if (button(settings_view == static_cast<int>(i) ? "Close" : "Edit", Btn::Default, 26, "", 54))
        settings_view = settings_view == static_cast<int>(i) ? -1 : static_cast<int>(i);
      ImGui::PopFont();
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
      if (settings_view == static_cast<int>(i)) {  // the editor, under its row
        ImGui::Indent(40 * k());
        float fw = W - 56 * k();
        static std::string name_buf, query_buf, err;
        static int editing = -1;
        if (editing != static_cast<int>(i)) {
          name_buf = tabs[i].v.name;
          query_buf = tabs[i].v.query;
          err.clear();
          editing = static_cast<int>(i);
        }
        label("Name");
        ImGui::SetNextItemWidth(fw);
        if (input("##vname", name_buf, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit())
          err = rename_view(i, name_buf);
        label("Shows");
        bool is_list = tabs[i].v.is_list();
        combo("##kind", is_list ? "The PRs a plugin lists" : "A GitHub search", fw, [&] {
          if (ImGui::Selectable("A GitHub search", !is_list)) edit_view(i, [](View& v) { v.plugin.clear(), v.list.clear(), v.sort.reset(); });
          for (auto& pc : cfg.plugins)
            if (ImGui::Selectable(("The PRs " + pc.name + " lists").c_str(), is_list && tabs[i].v.plugin == pc.name)) {
              auto ls = plugin_lists(pc.name);
              std::string l = ls.empty() ? "default" : ls[0];
              edit_view(i, [&](View& v) { v.plugin = pc.name, v.list = l, v.sort.reset(); });
            }
        });
        if (is_list) {
          combo("##list", tabs[i].v.list, fw, [&] {
            for (auto& l : plugin_lists(tabs[i].v.plugin))
              if (ImGui::Selectable(l.c_str(), tabs[i].v.list == l)) edit_view(i, [&](View& v) { v.list = l; });
          });
        }
        label("Filter");
        ImGui::PushFont(f.mono, 12 * k());
        ImGui::SetNextItemWidth(fw);
        bool apply = input("##vquery", query_buf, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopFont();
        if (apply && query_buf != tabs[i].v.query) {
          std::string e;
          if (tabs[i].v.is_list() && !parse_local(query_buf, e)) err = e;
          else {
            err.clear();
            std::string q = query_buf;
            edit_view(i, [&](View& v) { v.query = q; });
          }
        }
        label("Sort");
        combo("##vsort", sort_name(tabs[i].v.sort_or_default()), fw, [&] {
          for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
            if (s == Sort::List && !tabs[i].v.is_list()) continue;
            if (ImGui::Selectable(sort_name(s), tabs[i].v.sort_or_default() == s)) edit_view(i, [&](View& v) { v.sort = s; });
          }
        });
        if (!err.empty()) text(err, t.danger, nullptr, 12);
        ImGui::Dummy(ImVec2(1, 4 * k()));
        if (button("Duplicate", Btn::Default, 28)) duplicate_view(i);
        ImGui::SameLine();
        if (tabs.size() > 1) {
          ImGui::PushStyleColor(ImGuiCol_Text, col(t.danger));
          if (button("Delete view", Btn::Default, 28)) confirm_delete = static_cast<int>(i);
          ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(1, 10 * k()));
        ImGui::Unindent(40 * k());
      }
      y = ImGui::GetCursorScreenPos().y;
      ImGui::PopID();
    }
    d->AddRect(p0, ImVec2(p0.x + W, y), col(t.border), 6 * k());
    ImGui::Dummy(ImVec2(1, 8 * k()));
    if (button("New view", Btn::Default, 30)) {
      size_t i = add_view();
      commit_view(i);
      settings_view = static_cast<int>(i);
    }
  } else {  // Plugins
    heading("Plugins", "Programs anton starts and talks to. Their own settings files open in your editor.");
    if (cfg.plugins.empty())
      text_wrapped("No plugins configured. Add them under \"plugins\" in config.json (specs/02-plugins.md).", t.muted, nullptr, 0, W);
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      const PluginConfig& pc = cfg.plugins[i];
      std::string problem = plugin_problem(i), file = plugin_file(pc);
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::SetCursorScreenPos(p + ImVec2(12 * k(), 12 * k()));
      ImGui::BeginGroup();
      float rowy = ImGui::GetCursorScreenPos().y;
      uint32_t dot = !problem.empty() ? t.danger : live[i].health == "ok" ? t.success : t.attention;
      d->AddCircleFilled(ImVec2(p.x + 16 * k(), rowy + 13 * k()), 4 * k(), col(dot));
      ImGui::SetCursorScreenPos(ImVec2(p.x + 30 * k(), rowy + (26 * k() - ImGui::GetTextLineHeight()) / 2));
      text(pc.name, t.fg, f.semibold);
      ImGui::SameLine(0, 10 * k());
      std::string status;
      if (!problem.empty()) status = live[i].health == "exited" ? "exited · " + live[i].health_text : "error";
      else status = (plugin_up(i) ? "running" : "starting") + std::string(" · ") + std::to_string(live[i].prs.size()) + " PRs";
      if (!live[i].actions.empty()) {
        status += " · keys";
        for (auto& a : live[i].actions) status += " " + a.key;
      }
      text(status, problem.empty() ? t.muted : t.danger, nullptr, 12);
      float bx = p.x + W - 12 * k();
      ImGui::PushFont(nullptr, 12 * k());
      std::string open_label = file.empty() ? "" : "Open " + std::filesystem::path(file).filename().string();
      float ow = open_label.empty() ? 0 : ImGui::CalcTextSize(open_label.c_str()).x + 26 * k();
      float rw = ImGui::CalcTextSize("Restart").x + 26 * k();
      ImGui::SetCursorScreenPos(ImVec2(bx - ow - (ow ? 8 * k() : 0) - rw, rowy));
      if (button("Restart", Btn::Default, 26) && plugins) {
        plugins->restart(i);
        flash("restarting " + pc.name);
      }
      if (!file.empty()) {
        ImGui::SameLine(0, 8 * k());
        if (button(open_label, Btn::Default, 26)) open_file(file);
      }
      ImGui::PopFont();
      ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * k(), rowy + 34 * k()));
      std::string cmd;
      for (auto& a : pc.command) cmd += (cmd.empty() ? "" : " ") + tilde(a);
      text_wrapped(cmd, t.muted, f.mono, 12, W - 24 * k());
      if (!problem.empty()) text_wrapped(problem, t.danger, f.mono, 12, W - 24 * k());
      else if (!live[i].health_text.empty()) text_wrapped(live[i].health_text, t.muted, nullptr, 12, W - 24 * k());
      if (!live[i].last_stderr.empty()) text_wrapped("stderr: " + live[i].last_stderr, t.muted, f.mono, 12, W - 24 * k());
      ImGui::EndGroup();
      float bottom = ImGui::GetItemRectMax().y + 12 * k();
      d->AddRect(p, ImVec2(p.x + W, bottom), col(t.border), 6 * k());
      ImGui::SetCursorScreenPos(ImVec2(p.x, bottom + 12 * k()));
      ImGui::PopID();
    }
  }
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(2);
  ImGui::EndChild();

  // the footer: where it's saved, the file itself, Done
  ImVec2 fp(wp.x, wp.y + ws.y - foot);
  d = ImGui::GetWindowDrawList();
  d->AddRectFilled(fp, wp + ws, col(t.panel), 8 * k(), ImDrawFlags_RoundCornersBottom);
  d->AddLine(fp, ImVec2(fp.x + ws.x, fp.y), col(t.border));
  ImGui::SetCursorScreenPos(ImVec2(fp.x + 16 * k(), fp.y + (foot - ImGui::GetTextLineHeight()) / 2));
  if (now() < notice_until && notice_bad) text(notice, t.danger);
  else segments({{"Saved as you go to ", t.muted}, {tilde(config_path()), t.muted}});
  ImGui::PushFont(f.medium, 0.f);
  float dw = ImGui::CalcTextSize("Done").x + 28 * k(), ow = ImGui::CalcTextSize("Open config file").x + 24 * k();
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(fp.x + ws.x - 16 * k() - dw - 8 * k() - ow, fp.y + (foot - 30 * k()) / 2));
  if (button("Open config file", Btn::Default, 30)) open_config();
  ImGui::SameLine(0, 8 * k());
  if (button("Done", Btn::Primary, 30)) settings_open = false;
  if (confirm_delete >= 0 && settings_page == 2) {
    ImGui::OpenPopup("##delete2");
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 14 * k()));
  if (ImGui::BeginPopupModal("##delete2", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
    size_t i = static_cast<size_t>(std::max(0, confirm_delete));
    if (i < tabs.size()) text("Delete " + tabs[i].v.name + "?", t.fg, f.semibold, 14);
    if (button("Cancel")) {
      confirm_delete = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button("Delete", Btn::Primary)) {
      if (i < tabs.size()) delete_view(i);
      settings_view = -1;
      confirm_delete = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
  ImGui::End();
}

// -- the command palette ----------------------------------------------------------------

void Gui::palette() {
  struct Item {
    std::string group, icon, label, hint, key;
    uint32_t icon_c;
    std::function<void()> run;
  };
  std::vector<Item> items;
  const Pr* cur = screen == Screen::Reader && cache.count(reader_url) ? &cache.at(reader_url) : selected_pr();
  if (cur) {
    std::string g = "Actions on " + short_repo(cur->repo) + "#" + std::to_string(cur->number);
    const FileTab* ft = screen == Screen::Reader ? reader_tab_file() : nullptr;
    std::string url = cur->url;
    for (auto& [i, a] : plugin_keys()) {
      std::string label = capital(strip_emoji(a.label.empty() ? a.id : a.label));
      items.push_back({g, a.line ? "▶" : "→", label, cfg.plugins[i].name, a.key, a.line ? t.success : t.muted,
                       [this, k_ = a.key, url] { plugin_action(k_, url); }});
      if (a.line && ft && ft->spec)
        items.push_back({g, "▶", capital(strip_emoji(a.label)) + " on this file only", ft->path, "", t.success,
                         [this, k_ = a.key, url, path = ft->path] { plugin_action(k_, url, path); }});
    }
    // the first row that did not pass, in the reader
    for (auto& st : live) {
      auto it = st.prs.find(url);
      if (it == st.prs.end()) continue;
      for (auto& m : it->second.marks)
        if (m.line > 0 && (m.level == "error") && !m.text.empty()) {
          items.push_back({g, "→", "Jump to the first row that did not pass", path_tail(m.file, 1) + ", line " + std::to_string(m.line),
                           "", t.muted, [this, c = *cur, m] {
                             if (screen != Screen::Reader || reader_url != c.url) open_reader(c);
                             jump(m.file, m.line);
                           }});
          break;
        }
      break;
    }
    if (screen != Screen::Reader) items.push_back({g, "→", "Open in reader", "description and files", "Enter", t.muted, [this, c = *cur] { open_reader(c); }});
    items.push_back({g, "→", "Open on GitHub", "", "w", t.muted, [this, url] { open(url); }});
    items.push_back({g, "→", "Copy URL", "", "y", t.muted, [this, url] {
                       ImGui::SetClipboardText(url.c_str());
                       flash("copied " + url);
                     }});
    if (has_note(url)) items.push_back({g, "→", "Read the note", "LLM review", "n", t.muted, [this, c = *cur] {
                                          open_reader(c);
                                          focus_llm = true;
                                        }});
  }
  // pull requests: every one fetched, the active view's first
  std::vector<const Pr*> prs;
  std::set<std::string> seen_url;
  for (auto& p : cur_rows) prs.push_back(&p), seen_url.insert(p.url);
  for (auto& [url, p] : cache)
    if (!p.partial && !seen_url.count(url)) prs.push_back(&p);
  for (auto* p : prs) {
    std::string said;  // what a plugin's run says: "· temoc failed"
    for (size_t i = 0; i < live.size(); ++i)
      if (auto it = live[i].prs.find(p->url); it != live[i].prs.end() && it->second.marks_summary)
        said += " · " + cfg.plugins[i].name + " " +
                (it->second.marks_summary->banner ? "failed" : short_result(it->second.marks_summary->text));
    items.push_back({"Pull requests", "#", short_repo(p->repo) + "#" + std::to_string(p->number) + " " + shown_title(p->title) + said,
                     who(p->author) + " · " + age(now() - p->created), "", t.muted, [this, pr = *p] {
                       sel_url = pr.url;
                       scroll_to_sel = true;
                       open_reader(pr);
                     }});
  }
  for (size_t i = 0; i < tabs.size(); ++i)
    items.push_back({"Views", "≡", "Go to " + tabs[i].v.name, tabs[i].v.query, i < 9 ? std::to_string(i + 1) : "", t.muted,
                     [this, i] {
                       screen = Screen::List;
                       switch_view(i);
                     }});
  items.push_back({"Views", "+", "New view", "a GitHub search", "+", t.muted, [this] { act("+"); }});
  items.push_back({"Views", "↻", "Refresh", "every " + std::to_string(cfg.refresh_seconds) + "s", "r", t.muted, [this] { refresh(); }});
  std::string g = "Settings";
  items.push_back({g, "⚙", "Open settings", "General, Appearance, Views, Plugins", "Ctrl ,", t.muted, [this] { settings_open = true; }});
  for (auto [id, name] : {std::pair{"dark", "Dark"}, {"light", "Light"}, {"system", "Match system"}})
    items.push_back({g, "⚙", std::string("Theme: ") + name, cfg.gui.theme == id ? "current" : "", "", t.muted,
                     [this, id = std::string(id)] { set_theme(id); }});
  auto pane = [&](const std::string& name, bool* v) {
    items.push_back({g, "⚙", (*v ? "Hide the " : "Show the ") + name + " pane", "View › Panes", "", t.muted, [this, v] {
                       *v = !*v;
                       save();
                     }});
  };
  pane("Views", &cfg.gui.views);
  pane("Preview", &cfg.gui.preview);
  pane("Run log", &cfg.gui.run_log);
  items.push_back({g, "⚙", "Reset layout", "the panes as they first were", "", t.muted, [this] { reset_main = reset_reader = true; }});
  items.push_back({g, "⚙", "Open config.json", tilde(config_path()), "", t.muted, [this] { open_config(); }});
  for (size_t i = 0; i < cfg.plugins.size(); ++i) {
    std::string name = cfg.plugins[i].name, file = plugin_file(cfg.plugins[i]);
    std::string st = plugin_problem(i).empty() ? (plugin_up(i) ? "running" : "starting") : "not running";
    items.push_back({g, "⚙", "Restart the " + name + " plugin", st, "", t.muted, [this, i] {
                       if (plugins) plugins->restart(i);
                     }});
    if (!file.empty())
      items.push_back({g, "⚙", "Open " + std::filesystem::path(file).filename().string(), name + "'s settings", "", t.muted,
                       [this, file] { open_file(file); }});
  }

  // filter: the label, fuzzily; the hint as a fallback, unhighlighted
  struct Hit {
    const Item* it;
    std::vector<size_t> at;
  };
  std::vector<Hit> hits;
  bool whole = false;  // something has the query as a run: scattered matches are noise then
  for (auto& it : items) {
    if (auto m = fuzzy_find(it.label, palette_q)) {
      hits.push_back({&it, *m});
      whole = whole || (palette_q.size() >= 3 && !m->empty() && m->back() - m->front() + 1 == m->size());
    } else if (!palette_q.empty() && lower(it.hint).find(lower(palette_q)) != std::string::npos) {
      hits.push_back({&it, {}});
    }
  }
  if (whole)
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [](const Hit& h) { return !h.at.empty() && h.at.back() - h.at.front() + 1 != h.at.size(); }),
               hits.end());
  {  // at most 8 PRs unless that's what's being searched
    size_t n = 0;
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const Hit& h) { return h.it->group == "Pull requests" && ++n > 8; }),
               hits.end());
  }
  palette_sel = hits.empty() ? 0 : std::clamp(palette_sel, 0, static_cast<int>(hits.size()) - 1);

  ImGuiViewport* vp = ImGui::GetMainViewport();
  // the scrim: a click on it closes the palette
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.scrim));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##scrim", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing);
  if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) palette_open = false;
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  float W = 640 * k();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - W) / 2, vp->Pos.y + 96 * k()));
  ImGui::SetNextWindowSize(ImVec2(W, 0));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.palette_bg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12 * k());
  ImGui::Begin("##palette", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  // the input
  float ih = 52 * k();
  icon_search(d, ImVec2(wp.x + 16 * k(), wp.y + (ih - 16 * k()) / 2), 16 * k(), col(t.muted));
  ImGui::SetCursorPos(ImVec2(42 * k(), (ih - 15 * k() * 1.2f - 4 * k()) / 2));
  ImGui::PushFont(nullptr, 15 * k());
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2 * k()));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
  ImGui::SetNextItemWidth(W - 42 * k() - 60 * k());
  if (palette_focus || !ImGui::IsAnyItemActive()) {
    ImGui::SetKeyboardFocusHere();
    palette_focus = false;
  }
  std::string before = palette_q;
  input("##pq", palette_q, 0, "Search or run a command");
  if (palette_q != before) palette_sel = 0;
  ImGui::PopStyleColor(3);
  ImGui::PopStyleVar(2);
  ImGui::PopFont();
  {
    ImGui::PushFont(f.mono, 11 * k());
    ImVec2 es = ImGui::CalcTextSize("Esc");
    ImVec2 ep(wp.x + W - 16 * k() - es.x - 12 * k(), wp.y + (ih - 18 * k()) / 2);
    d->AddRect(ep, ep + ImVec2(es.x + 12 * k(), 18 * k()), col(t.border), 4 * k());
    d->AddText(ImVec2(ep.x + 6 * k(), ep.y + (18 * k() - es.y) / 2), col(t.muted), "Esc");
    ImGui::PopFont();
  }
  d->AddLine(ImVec2(wp.x, wp.y + ih), ImVec2(wp.x + W, wp.y + ih), col(t.border));
  // keys
  bool run = false;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) palette_sel = std::min(palette_sel + 1, std::max(0, static_cast<int>(hits.size()) - 1));
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) palette_sel = std::max(palette_sel - 1, 0);
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) run = true;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) palette_open = false;
  // the results, grouped
  ImGui::SetCursorPos(ImVec2(0, ih + 1));
  float list_h = std::min(520 * k(), hits.empty() ? 60 * k() : (hits.size() * 38 + 40 * 4) * k());
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  ImGui::BeginChild("##results", ImVec2(W, list_h), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  ImDrawList* cd = ImGui::GetWindowDrawList();
  std::string group;
  if (hits.empty()) text("Nothing matches.", t.muted);
  static int last_sel = -1;
  for (size_t i = 0; i < hits.size(); ++i) {
    const Item& it = *hits[i].it;
    if (it.group != group) {
      group = it.group;
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10 * k());
      ImGui::Dummy(ImVec2(1, 2 * k()));
      ImGui::SetCursorPosX(16 * k());
      section(group);
    }
    ImGui::PushID(static_cast<int>(i));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 36 * k();
    if (ImGui::InvisibleButton("##item", ImVec2(w, h))) {
      palette_sel = static_cast<int>(i);
      run = true;
    }
    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) palette_sel = static_cast<int>(i);
    bool on = static_cast<int>(i) == palette_sel;
    if (on) {
      cd->AddRectFilled(p, p + ImVec2(w, h), col(t.palette_sel), 6 * k());
      cd->AddRectFilled(p, ImVec2(p.x + 2 * k(), p.y + h), col(t.sel_mark));
      if (last_sel != palette_sel) ImGui::SetScrollHereY();
    }
    float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
    ImVec2 is = f.mono->CalcTextSizeA(13 * k(), FLT_MAX, 0, it.icon.c_str());
    cd->AddText(f.mono, 13 * k(), ImVec2(p.x + 10 * k() + (18 * k() - is.x) / 2, p.y + (h - is.y) / 2), col(it.icon_c), it.icon.c_str());
    // the right side: hint, key
    float rx = p.x + w - 10 * k();
    if (!it.key.empty()) {
      ImVec2 ks = f.mono->CalcTextSizeA(11 * k(), FLT_MAX, 0, it.key.c_str());
      rx -= ks.x + 12 * k();
      cd->AddRect(ImVec2(rx, p.y + (h - 18 * k()) / 2), ImVec2(rx + ks.x + 12 * k(), p.y + (h + 18 * k()) / 2), col(t.border), 4 * k());
      cd->AddText(f.mono, 11 * k(), ImVec2(rx + 6 * k(), p.y + (h - ks.y) / 2), col(t.muted), it.key.c_str());
      rx -= 10 * k();
    }
    std::string hint = strip_emoji(it.hint);
    ImVec2 hs = f.regular->CalcTextSizeA(12 * k(), FLT_MAX, 0, hint.c_str());
    float hint_w = std::min(hs.x, 220 * k());
    if (!hint.empty()) {
      cd->PushClipRect(ImVec2(rx - hint_w, p.y), ImVec2(rx, p.y + h), true);
      cd->AddText(f.regular, 12 * k(), ImVec2(rx - hint_w, p.y + (h - hs.y) / 2), col(t.muted), hint.c_str());
      cd->PopClipRect();
      rx -= hint_w + 12 * k();
    }
    // the label, its matched characters highlighted
    float x = p.x + 38 * k();
    cd->PushClipRect(ImVec2(x, p.y), ImVec2(rx, p.y + h), true);
    const auto& at = hits[i].at;
    size_t a = 0;
    const std::string& label = it.label;
    while (a < label.size()) {
      bool m = std::find(at.begin(), at.end(), a) != at.end();
      size_t b = a + 1;
      while (b < label.size() && (std::find(at.begin(), at.end(), b) != at.end()) == m) ++b;
      std::string seg = label.substr(a, b - a);
      ImFont* fnt = m ? f.semibold : f.regular;
      ImVec2 ss = fnt->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, seg.c_str());
      cd->AddText(fnt, ImGui::GetFontSize(), ImVec2(x, ty), col(m ? t.accent : t.fg), seg.c_str());
      x += ss.x;
      a = b;
    }
    cd->PopClipRect();
    ImGui::PopID();
  }
  last_sel = palette_sel;
  ImGui::EndChild();
  // the footer
  ImVec2 fp = ImGui::GetCursorScreenPos();
  float fh = 34 * k();
  d->AddRectFilled(fp, fp + ImVec2(W, fh), col(t.palette_foot), 12 * k(), ImDrawFlags_RoundCornersBottom);
  d->AddLine(fp, ImVec2(fp.x + W, fp.y), col(t.border));
  ImGui::PushFont(nullptr, 12 * k());
  float fy = fp.y + (fh - ImGui::GetTextLineHeight()) / 2, fx = fp.x + 16 * k();
  for (const char* s : {"↑ ↓ to move", "Enter to run", "Esc to close"}) {
    d->AddText(ImVec2(fx, fy), col(t.muted), s);
    fx += ImGui::CalcTextSize(s).x + 16 * k();
  }
  std::string n = std::to_string(hits.size()) + (hits.size() == 1 ? " result" : " results");
  d->AddText(ImVec2(fp.x + W - 16 * k() - ImGui::CalcTextSize(n.c_str()).x, fy), col(t.muted), n.c_str());
  ImGui::PopFont();
  ImGui::Dummy(ImVec2(W, fh));
  ImGui::End();
  if (run && palette_sel < static_cast<int>(hits.size())) {
    auto fn = hits[static_cast<size_t>(palette_sel)].it->run;
    palette_open = false;
    fn();
  }
}

}  // namespace anton::gui
