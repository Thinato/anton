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
  ImVec2 size(860 * k(), 640 * k());
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * k());
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
  float th = 32 * k();
  d->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + th), col(t.panel), 10 * k(), ImDrawFlags_RoundCornersTop);
  d->AddLine(ImVec2(wp.x, wp.y + th), ImVec2(wp.x + ws.x, wp.y + th), col(t.border));
  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::InvisibleButton("##drag", ImVec2(ws.x - 40 * k(), th));  // drag the title bar to move it
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    ImGui::SetWindowPos(wp + ImGui::GetIO().MouseDelta);
  {
    ImVec2 ts = f.semibold->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, "Settings");
    d->AddText(f.semibold, ImGui::GetFontSize(), ImVec2(wp.x + (ws.x - ts.x) / 2, wp.y + (th - ts.y) / 2), col(t.fg), "Settings");
  }
  ImGui::SetCursorPos(ImVec2(ws.x - 32 * k(), 4 * k()));
  if (icon_button("##close", icon_cross, 24, false, "Close (Esc)")) settings_open = false;

  // the sections, on the left
  float nav_w = 168 * k(), foot = 40 * k();
  ImVec2 body0(wp.x, wp.y + th + 1), body1(wp.x + ws.x, wp.y + ws.y - foot);
  d->AddRectFilled(body0, ImVec2(body0.x + nav_w, body1.y), col(t.panel));
  d->AddLine(ImVec2(body0.x + nav_w, body0.y), ImVec2(body0.x + nav_w, body1.y), col(t.border));
  const char* names[] = {"General", "Appearance", "PR list", "Views", "Plugins"};
  std::string hints[] = {"", "", "", std::to_string(tabs.size()), std::to_string(cfg.plugins.size())};
  for (int i = 0; i < 5; ++i) {
    float rw = nav_w - 12 * k(), rh = 24 * k();
    ImGui::SetCursorPos(ImVec2(6 * k(), th + 8 * k() + i * (rh + 1 * k())));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##nav", ImVec2(rw, rh))) settings_page = i;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (settings_page == i) d->AddRectFilled(p, p + ImVec2(rw, rh), col(t.sel), 6 * k());
    else if (hov) d->AddRectFilled(p, p + ImVec2(rw, rh), col(t.dark ? t.border_muted : t.inset), 6 * k());
    float ty = p.y + (rh - ImGui::GetTextLineHeight()) / 2;
    d->AddText(settings_page == i ? f.semibold : f.regular, ImGui::GetFontSize(), ImVec2(p.x + 8 * k(), ty), col(t.fg), names[i]);
    if (!hints[i].empty()) {
      ImVec2 hs = f.mono->CalcTextSizeA(code() * k(), FLT_MAX, 0, hints[i].c_str());
      d->AddText(f.mono, code() * k(), ImVec2(p.x + rw - 8 * k() - hs.x, p.y + (rh - hs.y) / 2), col(t.muted), hints[i].c_str());
    }
  }

  // the page
  ImGui::SetCursorPos(ImVec2(nav_w + 1, th + 1));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * k(), 14 * k()));
  ImGui::BeginChild("##page", ImVec2(ws.x - nav_w - 1, ws.y - th - foot - 2), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 6 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * k(), 6 * k()));  // 24px fields
  ImGui::PushStyleColor(ImGuiCol_FrameBg, col(t.btn));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col(t.dark ? t.border_muted : t.inset));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col(t.dark ? t.border_muted : t.inset));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, col(0xffffffffu));
  float W = ImGui::GetContentRegionAvail().x, half = (W - 24 * k()) / 2;
  auto heading = [&](const std::string& title, const std::string& sub) {
    text(title, t.fg, f.semibold, 14);
    if (!sub.empty()) text_wrapped(sub, t.muted, nullptr, 11, W);
    ImGui::Dummy(ImVec2(1, 6 * k()));
  };
  auto label = [&](const std::string& s) { text(s, t.muted, f.semibold, 11); };
  auto check = [&](const char* id, bool v, const std::string& s) {  // a checkbox drawn accent when on
    ImVec2 p = ImGui::GetCursorScreenPos();
    float b = 13 * k();
    std::string lid = std::string("##") + id;
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    bool clicked = ImGui::InvisibleButton(lid.c_str(), ImVec2(b + 8 * k() + ts.x, std::max(b, ts.y)));
    float y = p.y + (std::max(b, ts.y) - b) / 2;
    if (v) {
      d = ImGui::GetWindowDrawList();
      d->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + b, y + b), col(t.accent), 3 * k());
      d->AddLine(ImVec2(p.x + 3 * k(), y + 6.5f * k()), ImVec2(p.x + 5.5f * k(), y + 9 * k()), IM_COL32_WHITE, 1.6f * k());
      d->AddLine(ImVec2(p.x + 5.5f * k(), y + 9 * k()), ImVec2(p.x + 10 * k(), y + 4 * k()), IM_COL32_WHITE, 1.6f * k());
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
    text("PRs a plugin marks as watched, polled quietly", t.muted, nullptr, 11);
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
      text("The pattern cut from every title (a regular expression):", t.muted, nullptr, 11);
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
      if (!re_err.empty()) text(re_err, t.danger, nullptr, 11);
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
                 t.muted, nullptr, 11, W);
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
    label("Code font");
    combo("##codefont", cfg.gui.code_font, half, [&] {
      for (const char* fnt : {"JetBrains Mono", "System monospace"})
        if (ImGui::Selectable(fnt, cfg.gui.code_font == fnt)) {
          cfg.gui.code_font = fnt;
          save("saved");
        }
    });
    ImGui::EndGroup();
    ImGui::SameLine(0, 24 * k());
    ImGui::BeginGroup();
    label("Hover tips");
    ImGui::Dummy(ImVec2(1, 4 * k()));
    if (check("tooltips", cfg.gui.tooltips, "Show tooltips, the actions' long labels among them")) {
      cfg.gui.tooltips = !cfg.gui.tooltips;
      save("saved");
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(1, 10 * k()));
    label("Row height, columns and text sizes are under PR list.");
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
                 {"Results (with the reader)", &cfg.gui.results},
                 {"Run log and Problems (with the reader)", &cfg.gui.run_log},
                 {"Status bar", &cfg.gui.status_bar}};
    for (int i = 0; i < 5; ++i) {
      if (i % 2) ImGui::SameLine(half + 24 * k() + ImGui::GetStyle().WindowPadding.x);
      if (check(panes[i].name, *panes[i].v, panes[i].name)) {
        *panes[i].v = !*panes[i].v;
        save("saved");
      }
    }
    ImGui::Dummy(ImVec2(1, 8 * k()));
    if (button("Reset layout")) {
      reset_layout = true;
      flash("layout reset");
    }
  } else if (settings_page == 2) {  // PR list: what each row shows (the canvas's ListSettings)
    GuiPrefs& g = cfg.gui;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 0));  // the canvas's gaps, below
    text("PR list", t.fg, f.semibold, 14);
    ImGui::Dummy(ImVec2(1, 2 * k()));
    text("What each row shows. The View menu has the same switches.", t.muted, nullptr, 11);
    ImGui::Dummy(ImVec2(1, 14 * k()));
    // a live row: the selected PR as the list draws it now
    text("Preview", t.muted, f.semibold, 11);
    ImGui::Dummy(ImVec2(1, 4 * k()));
    {
      std::vector<std::string> cols;
      for (auto& c : g.columns)
        if (c.shown && c.name != "labels" && c.name != "tags") cols.push_back(c.name);
      cols.push_back("title");
      float rh = row_height(g.density) * k();
      ImVec2 p = ImGui::GetCursorScreenPos();
      const Pr* pr = selected_pr();
      if (!pr) {
        ImGui::Dummy(ImVec2(W, rh));
        d->AddText(f.regular, 11 * k(), ImVec2(p.x + 10 * k(), p.y + (rh - 11 * k()) / 2), col(t.muted), "No pull request to show yet.");
      } else {
        d->AddRectFilled(p, p + ImVec2(W, rh), col(t.sel), 6 * k());
        d->AddRectFilled(p, ImVec2(p.x + 2 * k(), p.y + rh), col(t.sel_mark));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4 * k(), 0));
        if (ImGui::BeginTable("##rowpreview", static_cast<int>(cols.size()), ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_SizingFixedFit,
                              ImVec2(W, rh))) {
          for (size_t i = 0; i < cols.size(); ++i) {
            const std::string& c = cols[i];
            float w = c == "number" ? 36 : c == "repo" ? 34 : c == "size" ? 68 : c == "blocks" ? 42 : c == "state" ? 120
                      : c == "age" ? 22 : c == "author" ? 64 : 0;
            if (c == "title") ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthStretch);
            else ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthFixed, (w + (i == 0 ? 6 : 0)) * k());
          }
          ImGui::TableNextRow(0, rh);
          ImGui::TableSetColumnIndex(0);
          pr_cells(*pr, cols, ImGui::GetCursorPosY(), rh, false);
          ImGui::EndTable();
        }
        ImGui::PopStyleVar();
      }
      d->AddRect(p, p + ImVec2(W, rh), col(t.border), 6 * k());
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rh));
      ImGui::Dummy(ImVec2(W, 0));
    }
    ImGui::Dummy(ImVec2(1, 14 * k()));
    float colw = (W - 24 * k()) / 2;
    auto header = [&](const std::string& s, const std::string& right = "") {
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x, h = 22 * k(), fs = 11 * k();
      d->AddText(f.semibold, fs, ImVec2(p.x, p.y + (h - fs) / 2), col(t.muted), s.c_str());
      if (!right.empty()) {
        float rw = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str()).x;
        d->AddText(f.regular, fs, ImVec2(p.x + w - rw, p.y + (h - fs) / 2), col(t.muted), right.c_str());
      }
      d->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), col(t.border_muted));
      ImGui::Dummy(ImVec2(w, h));
    };
    // the columns: drag a row to reorder, tick to show; the title always shows
    ImGui::BeginChild("##cols", ImVec2(colw, 0), ImGuiChildFlags_AutoResizeY);
    d = ImGui::GetWindowDrawList();
    header("Columns", "drag to reorder");
    ImGui::Dummy(ImVec2(1, 2 * k()));
    static const std::map<std::string, std::pair<const char*, const char*>> about = {
        {"number", {"Number", "#4812"}},
        {"repo", {"Repo", "the repo without its owner"}},
        {"size", {"Size (+/−)", "lines added and removed"}},
        {"blocks", {"Size blocks", "five green and red squares"}},
        {"state", {"State", "review, checks, merged"}},
        {"age", {"Age", "since the last update"}},
        {"author", {"Author", "login or name"}},
        {"labels", {"Labels", "GitHub labels"}},
        {"tags", {"Plugin tags", "temoc ✗, slack queued"}},
        {"title", {"Title", "always shown"}}};
    float cw = ImGui::GetContentRegionAvail().x;
    for (size_t i = 0; i <= g.columns.size(); ++i) {
      bool title = i == g.columns.size();
      std::string id = title ? "title" : g.columns[i].name;
      bool on = title || g.columns[i].shown;
      ImGui::PushID(static_cast<int>(i));
      ImVec2 p = ImGui::GetCursorScreenPos();
      float h = 26 * k(), fs = ImGui::GetFontSize();
      bool clicked = ImGui::InvisibleButton("##col", ImVec2(cw, h));
      if (!title) {
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          int from = static_cast<int>(i);
          ImGui::SetDragDropPayload("column", &from, sizeof from);
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("column")) {
            size_t from = static_cast<size_t>(*static_cast<const int*>(pl->Data));
            GuiColumn moved = g.columns[from];
            g.columns.erase(g.columns.begin() + static_cast<long>(from));
            g.columns.insert(g.columns.begin() + static_cast<long>(i), moved);
            save("saved");
          }
          ImGui::EndDragDropTarget();
        }
        if (clicked && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
          g.columns[i].shown = !g.columns[i].shown;
          save("saved");
        }
        icon_grip(d, ImVec2(p.x - 2 * k(), p.y + (h - 12 * k()) / 2), 12 * k(), col(t.muted));
      }
      // the checkbox, the name, what it is
      float b = 13 * k(), bx = p.x + 16 * k(), by = p.y + (h - b) / 2;
      if (on) {
        d->AddRectFilled(ImVec2(bx, by), ImVec2(bx + b, by + b), col(title ? t.muted : t.accent), 3 * k());
        d->AddLine(ImVec2(bx + 3 * k(), by + 6.5f * k()), ImVec2(bx + 5.5f * k(), by + 9 * k()), IM_COL32_WHITE, 1.6f * k());
        d->AddLine(ImVec2(bx + 5.5f * k(), by + 9 * k()), ImVec2(bx + 10 * k(), by + 4 * k()), IM_COL32_WHITE, 1.6f * k());
      } else {
        d->AddRect(ImVec2(bx, by), ImVec2(bx + b, by + b), col(t.muted), 3 * k(), 1.2f * k());
      }
      auto it = about.find(id);
      const char* name = it != about.end() ? it->second.first : id.c_str();
      const char* hint = it != about.end() ? it->second.second : "";
      d->AddText(f.medium, fs, ImVec2(bx + b + 8 * k(), p.y + (h - fs) / 2), col(title ? t.muted : t.fg), name);
      float hx = bx + b + 8 * k() + 92 * k();
      ImGui::PushFont(f.regular, 11 * k());
      ImGui::PushStyleColor(ImGuiCol_Text, col(t.muted));
      ImGui::RenderTextEllipsis(d, ImVec2(hx, p.y + (h - 11 * k()) / 2), ImVec2(p.x + cw, p.y + h), p.x + cw, hint, nullptr, nullptr);
      ImGui::PopStyleColor();
      ImGui::PopFont();
      ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 24 * k());
    // the styles, the text sizes, where the reader opens
    ImGui::BeginChild("##styles", ImVec2(colw, 0), ImGuiChildFlags_AutoResizeY);
    d = ImGui::GetWindowDrawList();
    auto segs = [&](const char* name, std::string& v, std::vector<std::pair<const char*, const char*>> opts) {
      header(name);
      ImGui::Dummy(ImVec2(1, 6 * k()));
      ImVec2 p = ImGui::GetCursorScreenPos();
      float x = p.x, h = 24 * k(), fs = ImGui::GetFontSize();
      for (size_t i = 0; i < opts.size(); ++i) {
        bool on = v == opts[i].first;
        ImFont* fnt = on ? f.semibold : f.regular;
        float w = fnt->CalcTextSizeA(fs, FLT_MAX, 0, opts[i].second).x + 20 * k();
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(name);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##seg", ImVec2(w, h)) && !on) {
          v = opts[i].first;
          save("saved");
        }
        ImGui::PopID();
        ImGui::PopID();
        ImDrawFlags corners = i == 0 ? ImDrawFlags_RoundCornersLeft : i + 1 == opts.size() ? ImDrawFlags_RoundCornersRight
                                                                                         : ImDrawFlags_RoundCornersNone;
        if (on) d->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h), col(t.dark ? t.btn : t.inset), 6 * k(), corners);
        if (i) d->AddLine(ImVec2(x, p.y), ImVec2(x, p.y + h), col(t.border));
        d->AddText(fnt, fs, ImVec2(x + 10 * k(), p.y + (h - fs) / 2), col(on ? t.fg : t.muted), opts[i].second);
        x += w;
      }
      d->AddRect(p, ImVec2(x, p.y + h), col(t.border), 6 * k());
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
      ImGui::Dummy(ImVec2(x - p.x, 14 * k()));
    };
    segs("Author", g.author_style, {{"avatar", "Avatar circle"}, {"color", "Name in colour"}});
    segs("State", g.state_style, {{"dot", "Circle and label"}, {"text", "Coloured text"}, {"pill", "Pill"}});
    segs("Row height", g.density, {{"compact", "Compact 24"}, {"cozy", "Cozy 28"}, {"roomy", "Roomy 34"}});
    header("Text size");
    ImGui::Dummy(ImVec2(1, 4 * k()));
    struct Size {
      const char* name;
      const char* hint;
      int* v;
    } sizes[] = {{"Interface", "lists, panes, buttons", &g.font_size},
                 {"Reading", "descriptions and specs", &g.reading_size},
                 {"Code", "numbers, paths, logs", &g.code_size}};
    float sw = ImGui::GetContentRegionAvail().x;
    for (auto& z : sizes) {
      ImGui::PushID(z.name);
      ImVec2 p = ImGui::GetCursorScreenPos();
      float h = 26 * k(), fs = ImGui::GetFontSize(), bw = 22 * k(), vw = 34 * k(), sx = p.x + sw - bw * 2 - vw;
      d->AddText(f.medium, fs, ImVec2(p.x, p.y + (h - fs) / 2), col(t.fg), z.name);
      d->AddText(f.regular, 11 * k(), ImVec2(p.x + 70 * k(), p.y + (h - 11 * k()) / 2), col(t.muted), z.hint);
      float sy = p.y + (h - 22 * k()) / 2;
      auto step = [&](const char* id, const char* glyph, float x, int by) {
        ImGui::SetCursorScreenPos(ImVec2(x, sy));
        bool ok = (by < 0 && *z.v > 9) || (by > 0 && *z.v < 20);
        if (ImGui::InvisibleButton(id, ImVec2(bw, 22 * k())) && ok) {
          *z.v += by;
          save("saved");
        }
        if (ImGui::IsItemHovered() && ok) d->AddRectFilled(ImVec2(x, sy), ImVec2(x + bw, sy + 22 * k()), col(t.dark ? t.border_muted : t.inset));
        ImVec2 gs = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, glyph);
        d->AddText(f.regular, fs, ImVec2(x + (bw - gs.x) / 2, sy + (22 * k() - gs.y) / 2), col(ok ? t.muted : t.border), glyph);
      };
      step("##dec", "−", sx, -1);
      step("##inc", "+", sx + bw + vw, 1);
      std::string val = std::to_string(*z.v) + " px";
      ImVec2 vs = f.mono->CalcTextSizeA(code() * k(), FLT_MAX, 0, val.c_str());
      d->AddText(f.mono, code() * k(), ImVec2(sx + bw + (vw - vs.x) / 2, sy + (22 * k() - vs.y) / 2), col(t.fg), val.c_str());
      d->AddRect(ImVec2(sx, sy), ImVec2(sx + bw * 2 + vw, sy + 22 * k()), col(t.border), 6 * k());
      d->AddLine(ImVec2(sx + bw, sy), ImVec2(sx + bw, sy + 22 * k()), col(t.border));
      d->AddLine(ImVec2(sx + bw + vw, sy), ImVec2(sx + bw + vw, sy + 22 * k()), col(t.border));
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
      ImGui::Dummy(ImVec2(sw, 0));
      ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, 14 * k()));
    header("Open in reader puts the PR");
    ImGui::Dummy(ImVec2(1, 4 * k()));
    for (auto [id, name, hint] : {std::tuple{"beside_preview", "In a tab beside Preview", "default"},
                                  {"last", "Where the last reader tab was", ""},
                                  {"window", "In its own window", ""}}) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      float h = 24 * k(), fs = ImGui::GetFontSize(), r = 6.5f * k();
      bool on = g.reader_placement == id;
      if (ImGui::InvisibleButton(id, ImVec2(sw, h)) && !on) {
        g.reader_placement = id;
        save("saved");
      }
      ImVec2 c(p.x + r, p.y + h / 2);
      d->AddCircle(c, r, col(on ? t.accent : t.muted), 0, 1.2f * k());
      if (on) d->AddCircleFilled(c, r - 3 * k(), col(t.accent));
      d->AddText(f.regular, fs, ImVec2(p.x + 21 * k(), p.y + (h - fs) / 2), col(t.fg), name);
      if (*hint) {
        float nw = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, name).x;
        d->AddText(f.regular, 11 * k(), ImVec2(p.x + 29 * k() + nw, p.y + (h - 11 * k()) / 2), col(t.muted), hint);
      }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    d = ImGui::GetWindowDrawList();
  } else if (settings_page == 3) {  // Views
    heading("Views", "Each view is a tab in the PR list. Drag to reorder.");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float y = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImVec2 p = ImGui::GetCursorScreenPos();
      if (i) d->AddLine(p, ImVec2(p.x + W, p.y), col(t.border_muted));
      float h = 36 * k();
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
      d->AddText(f.mono, code() * k(), ImVec2(p.x + 190 * k(), p.y + (h - code() * k()) / 2), col(t.muted), what.c_str());
      ImGui::PopClipRect();
      ImGui::SetCursorScreenPos(ImVec2(p.x + W - 58 * k(), p.y + (h - 20 * k()) / 2));
      if (button(settings_view == static_cast<int>(i) ? "Close" : "Edit", Btn::Default, 20, "", 48))
        settings_view = settings_view == static_cast<int>(i) ? -1 : static_cast<int>(i);
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
        ImGui::PushFont(f.mono, code() * k());
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
        if (!err.empty()) text(err, t.danger, nullptr, 11);
        ImGui::Dummy(ImVec2(1, 4 * k()));
        if (button("Duplicate")) duplicate_view(i);
        ImGui::SameLine();
        if (tabs.size() > 1) {
          ImGui::PushStyleColor(ImGuiCol_Text, col(t.danger));
          if (button("Delete view")) confirm_delete = static_cast<int>(i);
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
    if (button("New view")) {
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
      d->AddCircleFilled(ImVec2(p.x + 15 * k(), rowy + 12 * k()), 3 * k(), col(dot));
      ImGui::SetCursorScreenPos(ImVec2(p.x + 26 * k(), rowy + (24 * k() - ImGui::GetTextLineHeight()) / 2));
      text(pc.name, t.fg, f.semibold);
      ImGui::SameLine(0, 10 * k());
      std::string status;
      if (!problem.empty()) status = live[i].health == "exited" ? "exited · " + live[i].health_text : "error";
      else status = (plugin_up(i) ? "running" : "starting") + std::string(" · ") + std::to_string(live[i].prs.size()) + " PRs";
      if (!live[i].actions.empty()) {
        status += " · keys";
        for (auto& a : live[i].actions) status += " " + a.key;
      }
      text(status, problem.empty() ? t.muted : t.danger, nullptr, 11);
      float bx = p.x + W - 12 * k();
      std::string open_label = file.empty() ? "" : "Open " + std::filesystem::path(file).filename().string();
      float ow = open_label.empty() ? 0 : button_width(open_label, Btn::Default, "", nullptr, 20);
      float rw = button_width("Restart", Btn::Default, "", nullptr, 20);
      ImGui::SetCursorScreenPos(ImVec2(bx - ow - (ow ? 4 * k() : 0) - rw, rowy + 2 * k()));
      if (button("Restart", Btn::Default, 20) && plugins) restart_plugin(i);
      if (!file.empty()) {
        ImGui::SameLine(0, 4 * k());
        if (button(open_label, Btn::Default, 20)) open_file(file);
      }
      ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * k(), rowy + 28 * k()));
      std::string cmd;
      for (auto& a : pc.command) cmd += (cmd.empty() ? "" : " ") + tilde(a);
      text_wrapped(cmd, t.muted, f.mono, code(), W - 24 * k());
      if (!problem.empty()) text_wrapped(problem, t.danger, f.mono, code(), W - 24 * k());
      else if (!live[i].health_text.empty()) text_wrapped(live[i].health_text, t.muted, nullptr, 11, W - 24 * k());
      if (!live[i].last_stderr.empty()) text_wrapped("stderr: " + live[i].last_stderr, t.muted, f.mono, code(), W - 24 * k());
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
  d->AddRectFilled(fp, wp + ws, col(t.panel), 10 * k(), ImDrawFlags_RoundCornersBottom);
  d->AddLine(fp, ImVec2(fp.x + ws.x, fp.y), col(t.border));
  ImGui::SetCursorScreenPos(ImVec2(fp.x + 18 * k(), fp.y + (foot - 11 * k()) / 2));
  if (now() < notice_until && notice_bad) text(notice, t.danger, nullptr, 11);
  else {
    text("Saved as you go to ", t.muted, nullptr, 11);
    ImGui::SameLine(0, 0);
    text(tilde(config_path()), t.muted, f.mono, code());
  }
  // Reset to defaults: the page's own gui keys (Appearance, PR list); elsewhere config.json itself
  bool resets = settings_page == 1 || settings_page == 2;
  std::string other = resets ? "Reset to defaults" : "Open config file";
  float dw = button_width("Done", Btn::Primary), ow = button_width(other);
  ImGui::SetCursorScreenPos(ImVec2(fp.x + ws.x - 12 * k() - dw - 8 * k() - ow, fp.y + (foot - 24 * k()) / 2));
  if (button(other)) {
    const GuiPrefs dg;
    GuiPrefs& g = cfg.gui;
    if (settings_page == 1) {
      g.theme = dg.theme, g.code_font = dg.code_font, g.tooltips = dg.tooltips;
      g.views = dg.views, g.preview = dg.preview, g.results = dg.results, g.run_log = dg.run_log, g.status_bar = dg.status_bar;
    } else if (settings_page == 2) {
      g.columns = dg.columns, g.author_style = dg.author_style, g.state_style = dg.state_style, g.density = dg.density;
      g.font_size = dg.font_size, g.reading_size = dg.reading_size, g.code_size = dg.code_size;
      g.reader_placement = dg.reader_placement;
    }
    if (resets) save("back to the defaults");
    else open_config();
  }
  ImGui::SameLine(0, 8 * k());
  if (button("Done", Btn::Primary)) settings_open = false;
  if (confirm_delete >= 0 && settings_page == 3) {
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
  const Pr* cur = current_pr();
  if (cur) {
    std::string g = "Actions on " + short_repo(cur->repo) + "#" + std::to_string(cur->number);
    const FileTab* ft = reader_focused ? reader_tab_file() : nullptr;
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
                             if (!reader_open || reader_url != c.url) open_reader(c);
                             jump(m.file, m.line);
                           }});
          break;
        }
      break;
    }
    if (!reader_focused) items.push_back({g, "→", "Open in reader", "description and files", "↵", t.muted, [this, c = *cur] { open_reader(c); }});
    items.push_back({g, "→", "Open on GitHub", "", "w", t.muted, [this, url] { open(url); }});
    items.push_back({g, "→", "Copy URL", "", "y", t.muted, [this, url] {
                       ImGui::SetClipboardText(url.c_str());
                       flash("copied " + url);
                     }});
    if (has_note(url)) items.push_back({g, "→", "Read the note", "LLM review", "n", t.muted, [this, c = *cur] {
                                          open_reader(c);
                                          show_llm = focus_llm = true;
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
                       switch_view(i);
                       focus_list = true;
                     }});
  items.push_back({"Views", "+", "New view", "a GitHub search", "+", t.muted, [this] { act("+"); }});
  items.push_back({"Views", "↻", "Refresh", "every " + std::to_string(cfg.refresh_seconds) + "s", "r", t.muted, [this] { refresh(); }});
  std::string g = "Settings";
#ifdef __APPLE__
  const char* settings_key = "⌘,";
#else
  const char* settings_key = "Ctrl ,";
#endif
  items.push_back({g, "⚙", "Open settings", "General, Appearance, Views, Plugins", settings_key, t.muted, [this] { settings_open = true; }});
  for (auto [id, name] : {std::pair{"dark", "Dark"}, {"light", "Light"}, {"system", "Match system"}})
    items.push_back({g, "⚙", std::string("Theme: ") + name, cfg.gui.theme == id ? "current" : "", "", t.muted,
                     [this, id = std::string(id)] { set_theme(id); }});
  for (auto& mn : menus())  // the panes, as View toggles them
    if (mn.title == "View")
      for (auto& c : mn.items)
        if (c.run && c.check >= 0)
          items.push_back({g, "⚙", (c.check ? "Hide the " : "Show the ") + c.label + " pane", "View › Panes", "", t.muted, c.run});
  items.push_back({g, "⚙", "Reset layout", "the panes as they first were", "", t.muted, [this] { reset_layout = true; }});
  items.push_back({g, "⚙", "Open config.json", tilde(config_path()), "", t.muted, [this] { open_config(); }});
  for (size_t i = 0; i < cfg.plugins.size(); ++i) {
    std::string name = cfg.plugins[i].name, file = plugin_file(cfg.plugins[i]);
    std::string st = plugin_problem(i).empty() ? (plugin_up(i) ? "running" : "starting") : "not running";
    items.push_back({g, "⚙", "Restart the " + name + " plugin", st, "", t.muted, [this, i] { restart_plugin(i); }});
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

  float W = 600 * k();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - W) / 2, vp->Pos.y + 80 * k()));
  ImGui::SetNextWindowSize(ImVec2(W, 0));
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col(t.palette_bg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * k());
  ImGui::Begin("##palette", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  // the input
  float ih = 40 * k();
  icon_search(d, ImVec2(wp.x + 14 * k(), wp.y + (ih - 14 * k()) / 2), 14 * k(), col(t.muted));
  ImGui::SetCursorPos(ImVec2(38 * k(), (ih - 13 * k() - 4 * k()) / 2));
  ImGui::PushFont(nullptr, 13 * k());
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
    ImGui::PushFont(f.mono, 10 * k());
    ImVec2 es = ImGui::CalcTextSize("Esc");
    ImVec2 ep(wp.x + W - 14 * k() - es.x - 10 * k(), wp.y + (ih - 16 * k()) / 2);
    d->AddRect(ep, ep + ImVec2(es.x + 10 * k(), 16 * k()), col(t.border), 4 * k());
    d->AddText(ImVec2(ep.x + 5 * k(), ep.y + (16 * k() - es.y) / 2), col(t.muted), "Esc");
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
  float list_h = std::min(460 * k(), hits.empty() ? 48 * k() : (hits.size() * 29 + 28 * 4) * k());
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
      ImGui::Dummy(ImVec2(1, 4 * k()));
      ImGui::SetCursorPosX(14 * k());
      text(group, t.muted, f.semibold, 11);
      ImGui::Dummy(ImVec2(1, 1 * k()));
    }
    ImGui::PushID(static_cast<int>(i));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 28 * k();
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
    ImVec2 is = f.mono->CalcTextSizeA(12 * k(), FLT_MAX, 0, it.icon.c_str());
    cd->AddText(f.mono, 12 * k(), ImVec2(p.x + 8 * k() + (16 * k() - is.x) / 2, p.y + (h - is.y) / 2), col(it.icon_c), it.icon.c_str());
    // the right side: hint, key
    float rx = p.x + w - 10 * k();
    if (!it.key.empty()) {
      ImVec2 ks = f.mono->CalcTextSizeA(10 * k(), FLT_MAX, 0, it.key.c_str());
      rx -= ks.x + 10 * k();
      cd->AddRect(ImVec2(rx, p.y + (h - 16 * k()) / 2), ImVec2(rx + ks.x + 10 * k(), p.y + (h + 16 * k()) / 2), col(t.border), 4 * k());
      cd->AddText(f.mono, 10 * k(), ImVec2(rx + 5 * k(), p.y + (h - ks.y) / 2), col(t.muted), it.key.c_str());
      rx -= 10 * k();
    }
    std::string hint = strip_emoji(it.hint);
    ImVec2 hs = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, hint.c_str());
    float hint_w = std::min(hs.x, 220 * k());
    if (!hint.empty()) {
      cd->PushClipRect(ImVec2(rx - hint_w, p.y), ImVec2(rx, p.y + h), true);
      cd->AddText(f.regular, 11 * k(), ImVec2(rx - hint_w, p.y + (h - hs.y) / 2), col(t.muted), hint.c_str());
      cd->PopClipRect();
      rx -= hint_w + 12 * k();
    }
    // the label, its matched characters highlighted
    float x = p.x + 32 * k();
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
  float fh = 28 * k();
  d->AddRectFilled(fp, fp + ImVec2(W, fh), col(t.palette_foot), 10 * k(), ImDrawFlags_RoundCornersBottom);
  d->AddLine(fp, ImVec2(fp.x + W, fp.y), col(t.border));
  ImGui::PushFont(nullptr, 11 * k());
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
