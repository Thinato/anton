// The PR list's panes: Views (the tabs, as a list), Pull requests (the
// filter, its chips, the table) and Preview (the selected PR). See gui.hpp.
#include <algorithm>

#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "gui.hpp"

namespace anton::gui {

const Pr* Gui::selected_pr() const {
  for (auto& p : cur_rows)
    if (p.url == sel_url) return &p;
  return cur_rows.empty() ? nullptr : &cur_rows[0];
}

void Gui::select(int by) {
  if (cur_rows.empty()) return;
  int at = 0;
  for (size_t i = 0; i < cur_rows.size(); ++i)
    if (cur_rows[i].url == sel_url) at = static_cast<int>(i);
  long next = std::clamp<long>(static_cast<long>(at) + by, 0, static_cast<long>(cur_rows.size()) - 1);
  sel_url = cur_rows[static_cast<size_t>(next)].url;
  scroll_to_sel = true;
}

// The reader's tab opens on p, or turns to it if it's open already.
void Gui::open_reader(const Pr& p) {
  prepare_reader(p);
  if (!reader_open) place_reader = true;
  reader_open = focus_reader = true;
}

void Gui::close_reader() {
  reader_open = false;
  focus_list = true;
}

void Gui::act(const std::string& key) {
  const Pr* cur = selected_pr();
  if (key == "⏎") {
    if (cur) open_reader(*cur);
  } else if (key == "w") {
    if (cur) open(cur->url);
  } else if (key == "y") {
    std::string url = current_pr() ? current_pr()->url : "";
    if (!url.empty()) {
      ImGui::SetClipboardText(url.c_str());
      flash("copied " + url);
    }
  } else if (key == "r") {
    refresh();
  } else if (key == "s") {  // list order only exists for list views
    Sort s = view().sort_or_default();
    do s = static_cast<Sort>((static_cast<int>(s) + 1) % 5);
    while (s == Sort::List && !view().is_list());
    set_sort(s);
  } else if (key == "x") {
    clear_scope();
  } else if (key == "/") {
    focus_query = true;
  } else if (key == "f") {
    open_add_filter = true;
  } else if (key == "+") {
    add_view();
    focus_query = true;
  } else if (key == "n") {
    if (cur && has_note(cur->url)) {
      open_reader(*cur);
      show_llm = focus_llm = true;
    } else if (cur) {
      flash("no note on this PR");
    }
  } else if (key == "?") {
    palette_open = palette_focus = true;
  } else if (key == ",") {
    settings_open = true;
  } else if (key == "S") {
    if (tabs[active].saved) commit_view(active);
    else ask_name(Ask::Save, active);
  } else if (key == "<" || key == ">") {
    move_view(active, key == "<" ? -1 : 1);
  } else if (key == "e") {
    open_config();
  } else if (cur) {
    plugin_action(key, cur->url);
  }
}

// -- views -------------------------------------------------------------------------

void Gui::ask_name(Ask what, size_t i) {
  ask = what;
  ask_view = i;
  ask_err.clear();
  ask_buf = what == Ask::SaveAs ? tabs[i].v.name + " copy" : tabs[i].v.name;
}

void Gui::name_modal() {
  ImGui::SetNextWindowSize(ImVec2(420 * k(), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 14 * k()));
  if (ImGui::BeginPopupModal("##name", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* title = ask == Ask::Rename ? "Rename view" : ask == Ask::SaveAs ? "Save as new view" : "Save view as";
    text(title, t.fg, f.semibold, 14);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = input("##viewname", ask_buf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (!ask_err.empty()) text(ask_err, t.danger, nullptr, 12);
    if (button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ask = Ask::None;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (button(ask == Ask::Rename ? "Rename" : "Save", Btn::Primary) || enter) {
      std::string name = printable(ask_buf);
      ask_err.clear();
      if (ask == Ask::SaveAs) {
        if (name.empty()) ask_err = "a view needs a name";
        for (auto& tb : tabs)
          if (tb.v.name == name) ask_err = "there's a view named " + name + " already";
        if (ask_err.empty()) save_as_new(ask_view, name);
      } else {
        ask_err = rename_view(ask_view, name);
        if (ask_err.empty() && ask == Ask::Save) commit_view(ask_view);
      }
      if (ask_err.empty()) {
        ask = Ask::None;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

void Gui::view_menu(size_t i) {
  bool mod = modified(i), saved = tabs[i].saved.has_value();
  if (ImGui::MenuItem("Save filters", "S", false, mod)) saved ? commit_view(i) : ask_name(Ask::Save, i);
  if (ImGui::MenuItem("Save as new view…")) ask_name(Ask::SaveAs, i);
  if (ImGui::MenuItem("Revert to saved", nullptr, false, mod && saved)) revert_view(i);
  ImGui::Separator();
  if (ImGui::MenuItem("Rename…")) ask_name(Ask::Rename, i);
  if (ImGui::MenuItem("Duplicate")) duplicate_view(i);
  if (ImGui::MenuItem("Move up", "<", false, i > 0)) move_view(i, -1);
  if (ImGui::MenuItem("Move down", ">", false, i + 1 < tabs.size())) move_view(i, 1);
  ImGui::Separator();
  ImGui::PushStyleColor(ImGuiCol_Text, col(t.danger));
  if (ImGui::MenuItem("Delete view…", nullptr, false, tabs.size() > 1)) confirm_delete = static_cast<int>(i);
  ImGui::PopStyleColor();
}

static uint32_t repo_dot(const Theme& t, size_t i) {
  uint32_t c[] = {t.accent, t.done, t.attention, t.success, t.danger};
  return c[i % 5];
}

void Gui::views_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  bool shown = ImGui::Begin("Views###views", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImDrawList* d = ImGui::GetWindowDrawList();
  uint32_t hover = t.dark ? t.border_muted : t.inset;
  float h = 24 * k(), fs = ImGui::GetFontSize();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 1 * k()));
  for (size_t i = 0; i < tabs.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##view", ImVec2(w, h))) switch_view(i);
    bool hov = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##viewmenu")) {
      view_menu(i);
      ImGui::EndPopup();
    }
    if (hov && tabs[i].saved) ImGui::SetTooltip("%s", tabs[i].v.query.c_str());
    if (i == active) d->AddRectFilled(p, p + ImVec2(w, h), col(t.sel), 6 * k());
    else if (hov) d->AddRectFilled(p, p + ImVec2(w, h), col(hover), 6 * k());
    // the count on the right (mono, muted), then the plugin, then the name
    auto rs = rows(i);
    std::string count = rs ? std::to_string(rs->size()) : "…";
    ImVec2 cs = f.mono->CalcTextSizeA(code() * k(), FLT_MAX, 0, count.c_str());
    float right = p.x + w - 8 * k() - cs.x;
    d->AddText(f.mono, code() * k(), ImVec2(right, p.y + (h - cs.y) / 2), col(t.muted), count.c_str());
    right -= 6 * k();
    if (tabs[i].v.is_list()) {
      ImVec2 ps = f.regular->CalcTextSizeA(10 * k(), FLT_MAX, 0, tabs[i].v.plugin.c_str());
      right -= ps.x;
      d->AddText(f.regular, 10 * k(), ImVec2(right, p.y + (h - ps.y) / 2), col(t.muted), tabs[i].v.plugin.c_str());
      right -= 6 * k();
    }
    ImFont* nf = i == active ? f.semibold : f.regular;
    const std::string& name = tabs[i].v.name;
    float x = p.x + 8 * k(), nw = nf->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str()).x;
    float dot = modified(i) ? 10 * k() : 0;
    ImGui::RenderTextEllipsis(d, ImVec2(x, p.y + (h - fs) / 2), ImVec2(right - dot, p.y + h), right - dot, name.c_str(), nullptr, nullptr);
    if (modified(i))  // filters changed since saved: the yellow dot
      d->AddCircleFilled(ImVec2(std::min(x + nw + 6 * k(), right - 4 * k()), p.y + h / 2), 3 * k(), col(t.attention));
    ImGui::PopID();
  }
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("##newview", ImVec2(w, h))) act("+");
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(hover), 6 * k());
    icon_plus(d, ImVec2(p.x + 8 * k(), p.y + (h - 10 * k()) / 2), 10 * k(), col(t.muted));
    d->AddText(ImVec2(p.x + 24 * k(), p.y + (h - fs) / 2), col(t.muted), "New view");
  }
  // the repositories in this view: a click scopes every view to one
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(w, 13 * k()));
  d->AddLine(ImVec2(p.x + 8 * k(), p.y + 6.5f * k()), ImVec2(p.x + w - 8 * k(), p.y + 6.5f * k()), col(t.border_muted));
  {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float sh = 22 * k(), sfs = 11 * k();
    d->AddText(f.semibold, sfs, ImVec2(q.x + 8 * k(), q.y + (sh - sfs) / 2), col(t.muted), "Repositories");
    ImGui::Dummy(ImVec2(w, sh));
  }
  std::map<std::string, int> counts;
  for (auto& pr : cur_rows) ++counts[pr.repo];
  for (auto& r : repos)
    if (!counts.count(r)) counts[r] = 0;
  size_t n = 0;
  for (auto& [repo, cnt] : counts) {
    ImVec2 q = ImGui::GetCursorScreenPos();
    bool on = scope == "repo:" + repo;
    if (ImGui::InvisibleButton(("##repo" + repo).c_str(), ImVec2(w, h))) {
      scope = on ? "" : "repo:" + repo;
      reset_selection();
      refresh();
    }
    if (ImGui::IsItemHovered()) {
      d->AddRectFilled(q, q + ImVec2(w, h), col(hover), 6 * k());
      ImGui::SetTooltip(on ? "Show every repository" : "Only %s, in every view (x clears)", repo.c_str());
    }
    if (on) d->AddRectFilled(q, q + ImVec2(w, h), col(t.sel), 6 * k());
    d->AddCircleFilled(ImVec2(q.x + 12 * k(), q.y + h / 2), 4 * k(), col(repo_dot(t, n++)));
    std::string c = std::to_string(cnt);
    ImVec2 cs = f.mono->CalcTextSizeA(code() * k(), FLT_MAX, 0, c.c_str());
    float right = q.x + w - 8 * k() - cs.x;
    ImGui::RenderTextEllipsis(d, ImVec2(q.x + 24 * k(), q.y + (h - fs) / 2), ImVec2(right - 6 * k(), q.y + h), right - 6 * k(),
                              repo.c_str(), nullptr, nullptr);
    d->AddText(f.mono, code() * k(), ImVec2(right, q.y + (h - cs.y) / 2), col(t.muted), c.c_str());
  }
  ImGui::PopStyleVar();
  ImGui::End();
}

// A view's name asked for, or its delete confirmed: modals over everything.
void Gui::modals() {
  if (confirm_delete >= 0 && !settings_open && !ImGui::IsPopupOpen("##delete")) ImGui::OpenPopup("##delete");
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 14 * k()));
  if (ImGui::BeginPopupModal("##delete", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
    size_t i = static_cast<size_t>(std::max(0, confirm_delete));
    if (i < tabs.size()) text("Delete " + tabs[i].v.name + "?", t.fg, f.semibold, 14);
    text("It goes from config.json too.", t.muted);
    if (button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      confirm_delete = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, col(t.danger));
    if (button("Delete")) {
      if (i < tabs.size()) delete_view(i);
      confirm_delete = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
  if (ask != Ask::None && !ImGui::IsPopupOpen("##name")) ImGui::OpenPopup("##name");
  name_modal();
}

// -- the filter bar ------------------------------------------------------------------

std::string sort_label(Sort s) {
  switch (s) {
    case Sort::List: return "List order";
    case Sort::Updated: return "Recently updated";
    case Sort::Created: return "Newest";
    case Sort::Size: return "Biggest";
    case Sort::Repo: return "Repository";
  }
  return "";
}

// Tab in the query completes the word under the cursor (filters.hpp).
static int complete_cb(ImGuiInputTextCallbackData* d) {
  auto* g = static_cast<Gui*>(d->UserData);
  if (d->EventFlag != ImGuiInputTextFlags_CallbackCompletion) return 0;
  std::string s(d->Buf, static_cast<size_t>(d->BufTextLen));
  size_t cur = static_cast<size_t>(d->CursorPos), ws = word_start(s, cur);
  auto comps = complete(s.substr(ws, cur - ws), g->seen(), g->view().is_list());
  if (comps.empty()) return 0;
  d->DeleteChars(static_cast<int>(ws), static_cast<int>(cur - ws));
  d->InsertChars(static_cast<int>(ws), comps[0].token.c_str());
  return 0;
}

void Gui::filter_bar() {
  std::string q = with_scope(view().query, scope);
  bool list = view().is_list();
  ImDrawList* d = ImGui::GetWindowDrawList();
  float W = ImGui::GetContentRegionAvail().x;
  uint32_t hover = t.dark ? t.border_muted : t.inset;
  // Sort and Columns' widths: they go on the query's row, after it
  std::string sort_text = sort_label(view().sort_or_default());
  float fs = ImGui::GetFontSize(), bh = 24 * k();
  float sort_w = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, "Sort ").x + f.medium->CalcTextSizeA(fs, FLT_MAX, 0, sort_text.c_str()).x + 30 * k();
  float cols_w = f.medium->CalcTextSizeA(fs, FLT_MAX, 0, "Columns").x + 34 * k();
  // the query, in an input like GitHub's filter box
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = bh, qw = W - sort_w - cols_w - 12 * k();
    d->AddRectFilled(p, p + ImVec2(qw, h), col(t.input_bg), 6 * k());
    bool active_q = ImGui::GetActiveID() == ImGui::GetID("##query");
    d->AddRect(p, p + ImVec2(qw, h), col(active_q ? t.accent : t.border), 6 * k(), active_q ? 2.f : 1.f);
    icon_filter(d, ImVec2(p.x + 8 * k(), p.y + (h - 12 * k()) / 2), 12 * k(), col(t.muted));
    if (!active_q && !q_dirty) qbuf = q;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 28 * k(), p.y + (h - (code() * k() + 4 * k())) / 2));
    ImGui::PushFont(f.mono, code() * k());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 2 * k()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    ImGui::SetNextItemWidth(qw - 36 * k());
    if (focus_query) {
      ImGui::SetKeyboardFocusHere();
      focus_query = false;
    }
    bool enter = input("##query", qbuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion,
                       nullptr, complete_cb, this);
    bool editing = ImGui::IsItemActive();
    if (ImGui::IsItemEdited()) q_dirty = true;
    if (!editing && !enter) q_dirty = false;
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    if (enter) {
      q_dirty = false;
      if (!same_query(qbuf, q)) apply_query(qbuf);
    }
    if (editing) {  // what Tab would complete
      size_t ws = word_start(qbuf, qbuf.size());
      auto comps = complete(qbuf.substr(ws), seen(), list);
      if (!comps.empty()) {
        ImGui::SetNextWindowPos(ImVec2(p.x + 28 * k(), p.y + h + 2 * k()));
        ImGui::BeginTooltip();
        for (size_t i = 0; i < comps.size() && i < 8; ++i) {
          segments({{comps[i].label, i == 0 ? t.accent : t.fg}, {comps[i].hint.empty() ? "" : "   " + comps[i].hint, t.muted}},
                   f.mono, code());
        }
        text("Tab completes", t.muted, nullptr, 11);
        ImGui::EndTooltip();
      }
    }
    // Sort and Columns
    ImVec2 sp(p.x + qw + 6 * k(), p.y);
    ImGui::SetCursorScreenPos(sp);
    if (ImGui::InvisibleButton("##sort", ImVec2(sort_w, bh))) ImGui::OpenPopup("##sortmenu");
    bool hov = ImGui::IsItemHovered();
    d->AddRectFilled(sp, sp + ImVec2(sort_w, bh), col(hov ? hover : t.btn), 6 * k());
    d->AddRect(sp, sp + ImVec2(sort_w, bh), col(t.border), 6 * k());
    float ty = sp.y + (bh - fs) / 2, sx = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, "Sort ").x;
    d->AddText(f.regular, fs, ImVec2(sp.x + 8 * k(), ty), col(t.muted), "Sort");
    d->AddText(f.medium, fs, ImVec2(sp.x + 8 * k() + sx, ty), col(t.fg), sort_text.c_str());
    icon_chevron(d, ImVec2(sp.x + sort_w - 16 * k(), sp.y + (bh - 8 * k()) / 2), 8 * k(), col(t.muted));
    ImVec2 cp(sp.x + sort_w + 6 * k(), p.y);
    ImGui::SetCursorScreenPos(cp);
    if (ImGui::InvisibleButton("##columns", ImVec2(cols_w, bh))) ImGui::OpenPopup("##colmenu");
    hov = ImGui::IsItemHovered();
    d->AddRectFilled(cp, cp + ImVec2(cols_w, bh), col(hov ? hover : t.btn), 6 * k());
    d->AddRect(cp, cp + ImVec2(cols_w, bh), col(t.border), 6 * k());
    icon_columns(d, ImVec2(cp.x + 8 * k(), cp.y + (bh - 12 * k()) / 2), 12 * k(), col(t.muted));
    d->AddText(f.medium, fs, ImVec2(cp.x + 26 * k(), ty), col(t.fg), "Columns");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
    if (ImGui::BeginPopup("##sortmenu")) {
      static const char* what[] = {"the plugin's order", "last updated first", "newest first", "biggest first", "by repo"};
      for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
        if (s == Sort::List && !list) continue;
        if (ImGui::MenuItem(sort_label(s).c_str(), what[static_cast<int>(s)], view().sort_or_default() == s)) set_sort(s);
      }
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##colmenu")) {  // View › Columns, the same entries
      for (auto& mn : menus())
        for (auto& c : mn.items)
          if (mn.title == "View" && c.label == "Columns") draw_menu(c.sub);
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6 * k()));
  }
  // the chips and "+ Filter", 22px
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4 * k(), 4 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  float x0 = ImGui::GetCursorPosX(), xmax = x0 + ImGui::GetContentRegionAvail().x;
  float cfs = 11 * k();
  auto chip = [&](const std::string& id, const std::string& key, const std::string& value, bool on) {
    ImVec2 ks = f.regular->CalcTextSizeA(cfs, FLT_MAX, 0, key.c_str()), vs = f.medium->CalcTextSizeA(cfs, FLT_MAX, 0, value.c_str());
    float h = 22 * k(), w = 8 * k() + ks.x + vs.x + 4 * k() + 8 * k() + 8 * k();
    if (ImGui::GetCursorPosX() > x0 + 1 && w > xmax - ImGui::GetCursorPosX()) ImGui::NewLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    bool on_x = on && hov && ImGui::GetIO().MousePos.x > p.x + w - 18 * k();
    d->AddRectFilled(p, p + ImVec2(w, h), col(on ? t.accent_muted : hov ? hover : t.dark ? t.panel : t.bg), h / 2);
    d->AddRect(p, p + ImVec2(w, h), col(on ? t.accent : t.border), h / 2);
    float x = p.x + 8 * k(), y = p.y + (h - ks.y) / 2;
    d->AddText(f.regular, cfs, ImVec2(x, y), col(on ? t.accent : t.muted), key.c_str());
    d->AddText(f.medium, cfs, ImVec2(x + ks.x, y), col(on ? t.accent : t.fg), value.c_str());
    ImVec2 ip(p.x + w - 16 * k(), p.y + (h - 8 * k()) / 2);
    if (on) icon_cross(d, ip, 8 * k(), col(on_x ? t.fg : t.accent));
    else icon_chevron(d, ip, 8 * k(), col(t.muted));
    ImGui::SameLine();
    return clicked ? (on_x ? 2 : 1) : 0;
  };
  if (list) {
    if (chip("##listchip", "list ", view().plugin + " · " + view().list, false)) ImGui::OpenPopup("##listmenu");
    if (ImGui::BeginPopup("##listmenu")) {
      auto ls = plugin_lists(view().plugin);
      if (ls.empty()) ImGui::MenuItem((view().list + "  (nothing listed yet)").c_str(), nullptr, false, false);
      for (auto& l : ls)
        if (ImGui::MenuItem(l.c_str(), nullptr, view().list == l)) {
          view().list = l;
          reset_selection();
          refresh();
        }
      ImGui::EndPopup();
    }
  }
  Seen sn = seen();
  auto cs = chips(q);
  for (size_t ci = 0; ci < cs.size(); ++ci) {
    const Chip& c = cs[ci];
    std::string id = "##chip" + std::to_string(ci);
    int hit = chip(id, c.label + " ", c.value, c.set);
    if (hit == 2) apply_query(without(q, c.tokens));
    else if (hit == 1) ImGui::OpenPopup((id + "menu").c_str());
    if (ImGui::BeginPopup((id + "menu").c_str())) {
      bool fixed = c.fam == "state" || c.fam == "review" || c.fam == "author" || c.fam == "repo";
      for (auto& o : chip_options(c.fam, sn)) {
        bool ok = list ? o.local : o.search;
        bool on = o.token.empty() ? !c.set : has_token(q, o.token);
        std::string hint = !ok ? (list ? "search views only" : "list views only") : !o.hint.empty() ? o.hint : o.token;
        if (ImGui::MenuItem(o.label.c_str(), hint.c_str(), on, ok))
          apply_query(fixed || c.fam == "draft" ? with_family(q, c.fam, o.token) : with_token(without(q, c.tokens), o.token));
      }
      if (!fixed && c.set) {
        ImGui::Separator();
        if (ImGui::MenuItem("Remove")) apply_query(without(q, c.tokens));
      }
      if (list) text("This view filters the " + view().plugin + " list locally.", t.muted, nullptr, 11);
      ImGui::EndPopup();
    }
  }
  // the label chip, at "any" while no label filters (the canvas's fixed five)
  if (std::none_of(cs.begin(), cs.end(), [](const Chip& c) { return c.fam == "label"; })) {
    if (chip("##labelchip", "label ", "any", false)) ImGui::OpenPopup("##labelmenu");
    if (ImGui::BeginPopup("##labelmenu")) {
      auto opts = chip_options("label", sn);
      if (opts.empty()) ImGui::MenuItem("no labels seen yet", nullptr, false, false);
      for (auto& o : opts)
        if (ImGui::MenuItem(o.label.c_str(), o.hint.c_str())) apply_query(with_token(q, o.token));
      ImGui::EndPopup();
    }
  }
  // + Filter
  {
    float w = f.regular->CalcTextSizeA(cfs, FLT_MAX, 0, "+ Filter").x + 12 * k(), h = 22 * k();
    if (ImGui::GetCursorPosX() > x0 + 1 && w > xmax - ImGui::GetCursorPosX()) ImGui::NewLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##addfilter", ImVec2(w, h)) || open_add_filter) {
      ImGui::OpenPopup("##addmenu");
      open_add_filter = false;
    }
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(t.accent_muted), h / 2);
    d->AddText(f.regular, cfs, ImVec2(p.x + 6 * k(), p.y + (h - cfs) / 2), col(t.accent), "+ Filter");
    ImGui::SameLine();
    if (ImGui::BeginPopup("##addmenu")) {
      static std::string typed;
      for (auto& kind : add_kinds()) {
        bool ok = list ? kind.local : kind.search;
        if (!ok) {
          ImGui::MenuItem(kind.label.c_str(), "search views only", false, false);
          continue;
        }
        if (ImGui::BeginMenu(kind.label.c_str())) {
          bool draft = kind.prefix == "is:draft";
          if (!draft) {
            ImGui::SetNextItemWidth(220 * k());
            if (ImGui::IsWindowAppearing()) typed.clear();
            if (input("##typed", typed, ImGuiInputTextFlags_EnterReturnsTrue, "type a value, Enter")) {
              if (auto tok = typed_token(kind, typed); !tok.empty()) apply_query(with_token(q, tok));
              typed.clear();
              ImGui::CloseCurrentPopup();
            }
          }
          int shown = 0;
          for (auto& o : add_values(kind, sn, now())) {
            if (!typed.empty() && lower(o.label).find(lower(typed)) == std::string::npos) continue;
            bool vok = list ? o.local : o.search;
            if (ImGui::MenuItem(o.label.c_str(), vok ? o.hint.c_str() : "search views only", false, vok))
              apply_query(draft ? with_family(q, "draft", o.token) : with_token(q, o.token));
            if (++shown >= 12) break;
          }
          ImGui::EndMenu();
        }
      }
      ImGui::Separator();
      text("or type GitHub search syntax in the filter (/)", t.muted, nullptr, 11);
      ImGui::EndPopup();
    }
  }
  ImGui::NewLine();
  ImGui::PopStyleVar(2);
  // a view whose filters changed: Save, Save as new view, Revert
  if (modified(active)) {
    ImGui::Dummy(ImVec2(1, 2 * k()));
    ImVec2 p = ImGui::GetCursorScreenPos();
    d->AddCircleFilled(ImVec2(p.x + 3 * k(), p.y + 10 * k()), 3 * k(), col(t.attention));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * k(), p.y + (20 * k() - 11 * k()) / 2));
    text(view().name + (tabs[active].saved ? " changed" : " isn't saved yet"), t.muted, nullptr, 11);
    ImGui::SameLine(0, 10 * k());
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
    if (button("Save", Btn::Default, 20, "S")) act("S");
    ImGui::SameLine(0, 4 * k());
    if (button("Save as new view", Btn::Ghost, 20)) ask_name(Ask::SaveAs, active);
    if (tabs[active].saved) {
      ImGui::SameLine(0, 4 * k());
      if (button("Revert", Btn::Ghost, 20)) revert_view(active);
    }
  }
  ImGui::Dummy(ImVec2(1, 4 * k()));
}

// -- the table ---------------------------------------------------------------------------

void Gui::pr_table() {
  const View& v = view();
  int pi = v.is_list() ? plugin_index(v.plugin) : -1;
  auto centered = [&](const std::string& s, uint32_t c) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2((avail.x - ts.x) / 2, avail.y / 3));
    text(s, c);
  };
  if (v.is_list() && (pi < 0 || !plugin_up(static_cast<size_t>(pi))) && cur_rows.empty())
    return centered(v.plugin + ": not running", t.danger);
  if (cur_rows.empty() && (!rows(active) || loading())) return centered("Loading…", t.muted);
  if (cur_rows.empty()) {
    std::string q = with_scope(v.query, scope);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y / 4);
    std::string head = has_token(q, "is:open") ? "No open PRs match this view." : "No PRs match this view.";
    ImGui::PushFont(f.semibold, 16 * k());
    ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(head.c_str()).x) / 2);
    ImGui::TextUnformatted(head.c_str());
    ImGui::PopFont();
    ImGui::PushFont(f.mono, 12 * k());
    ImGui::SetCursorPosX((avail.x - ImGui::CalcTextSize(q.c_str()).x) / 2);
    text(q, t.muted);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1, 8 * k()));
    auto toks = tokenize(q);
    float bw = 0;
    std::vector<std::pair<std::string, std::function<void()>>> btns;
    if (!toks.empty() && toks.back() != "is:open")
      btns.push_back({"Drop " + toks.back(), [this, q, toks] { apply_query(without(q, {toks.back()})); }});
    if (has_token(q, "is:open")) btns.push_back({"Include closed", [this, q] { apply_query(without(q, {"is:open"})); }});
    btns.push_back({"Edit the query", [this] { focus_query = true; }});
    if (modified(active) && tabs[active].saved) btns.push_back({"Revert", [this] { revert_view(active); }});
    ImGui::PushFont(f.medium, 0.f);
    for (auto& b : btns) bw += ImGui::CalcTextSize(b.first.c_str()).x + 24 * k() + 8 * k();
    ImGui::PopFont();
    ImGui::SetCursorPosX(std::max(0.f, (avail.x - bw) / 2));
    for (size_t i = 0; i < btns.size(); ++i) {
      if (i) ImGui::SameLine();
      if (button(btns[i].first)) btns[i].second();
    }
    return;
  }

  // gui.columns, in their order, then the title; labels and tags go after it
  std::vector<std::string> cols;
  for (auto& c : cfg.gui.columns)
    if (c.shown && c.name != "labels" && c.name != "tags") cols.push_back(c.name);
  cols.push_back("title");
  std::set<std::string> owners_here;
  for (auto& p : cur_rows) owners_here.insert(p.owner);
  bool show_owner = owners_here.size() > 1;
  float rh = row_height(cfg.gui.density) * k();
  ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoPadOuterX |
                       ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, col(t.border_muted));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4 * k(), 0));  // 8px between columns
  if (!ImGui::BeginTable("##prs", static_cast<int>(cols.size()), fl)) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return;
  }
  for (size_t i = 0; i < cols.size(); ++i) {
    const std::string& c = cols[i];
    // the canvas's widths; the first column carries the row's 10px padding
    float w = c == "number" ? 40 : c == "repo" ? (show_owner ? 96 : 40) : c == "size" ? 78 : c == "blocks" ? 42
              : c == "state" ? 128 : c == "age" ? 34 : c == "author" ? 92 : 0;
    if (c == "title") ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthStretch);
    else ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthFixed, (w + (i == 0 ? 6 : 0)) * k());
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  // the header row: Repo, Size and Age sort on a click; the sort shows ↓
  Sort sort = v.sort_or_default();
  float hh = 24 * k(), hfs = 11 * k();
  ImGui::TableNextRow(ImGuiTableRowFlags_Headers, hh);
  ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, col(t.bg));
  ImDrawList* d = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < cols.size(); ++i) {
    ImGui::TableSetColumnIndex(static_cast<int>(i));
    const std::string& c = cols[i];
    std::optional<Sort> key = c == "repo" ? std::optional(Sort::Repo) : c == "size" ? std::optional(Sort::Size)
                              : c == "age" ? std::optional(Sort::Created) : std::nullopt;
    std::string label = c == "number" ? "#" : c == "blocks" ? "" : std::string(1, static_cast<char>(std::toupper(c[0]))) + c.substr(1);
    bool on = key && *key == sort;
    if (on) label = "↓ " + label;
    if (i == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * k());
    float y = ImGui::GetCursorPosY();
    ImVec2 ts = f.semibold->CalcTextSizeA(hfs, FLT_MAX, 0, label.c_str());
    if (c == "age") ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ts.x);
    ImGui::SetCursorPosY(y + (hh - ts.y) / 2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hov = false;
    if (key) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::InvisibleButton("##h", ImVec2(std::max(ts.x, 12 * k()), ts.y))) set_sort(*key);
      hov = ImGui::IsItemHovered();
      if (hov) ImGui::SetTooltip("Sort by %s", sort_label(*key).c_str());
      ImGui::PopID();
    } else {
      ImGui::Dummy(ts);
    }
    d->AddText(f.semibold, hfs, p, col(on || hov ? t.fg : t.muted), label.c_str());
  }
  for (size_t ri = 0; ri < cur_rows.size(); ++ri) {
    const Pr& p = cur_rows[ri];
    ImGui::TableNextRow(0, rh);
    bool is_sel = p.url == sel_url;
    ImGui::TableSetColumnIndex(0);
    float y0 = ImGui::GetCursorPosY();
    ImGui::PushID(static_cast<int>(ri));
    ImGui::PushStyleColor(ImGuiCol_Header, col(t.sel));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col(is_sel ? t.sel : (t.dark ? 0x151b23ffu : 0xf6f8faffu)));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, col(t.sel));
    if (ImGui::Selectable("##row", is_sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0, rh))) {
      sel_url = p.url;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) open_reader(p);
    if (ImGui::BeginPopupContextItem("##rowmenu")) {
      sel_url = p.url;
      if (ImGui::MenuItem("Open in reader", "Enter")) open_reader(p);
      if (ImGui::MenuItem("Open on GitHub", "w")) open(p.url);
      if (ImGui::MenuItem("Copy URL", "y")) {
        ImGui::SetClipboardText(p.url.c_str());
        flash("copied " + p.url);
      }
      for (auto& [i, a] : plugin_keys())
        if (ImGui::MenuItem(strip_emoji(a.button.empty() ? a.label : a.button).c_str(), a.key.c_str()))
          plugin_action(a.key, p.url);
      ImGui::EndPopup();
    }
    if (is_sel) {
      ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
      d->AddRectFilled(ImVec2(mn.x, mn.y), ImVec2(mn.x + 2 * k(), mx.y), col(t.sel_mark));
      if (scroll_to_sel) {
        if (!ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
        scroll_to_sel = false;
      }
    }
    ImGui::PopID();
    pr_cells(p, cols, y0, rh, show_owner);
  }
  ImGui::EndTable();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

// One row's cells, in the table row ImGui is on (the list, and Settings'
// preview row): cols in order, the title last with its labels and tags.
void Gui::pr_cells(const Pr& p, const std::vector<std::string>& cols, float y0, float rh, bool show_owner) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  float fs = ImGui::GetFontSize(), mfs = code() * k();
  auto cell_y = [&](float h) { ImGui::SetCursorPosY(y0 + (rh - h) / 2); };
  for (size_t ci = 0; ci < cols.size(); ++ci) {
    ImGui::TableSetColumnIndex(static_cast<int>(ci));
    const std::string& c = cols[ci];
    if (ci == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6 * k());
    if (c == "repo") {
      cell_y(fs);
      text(show_owner ? p.repo : short_repo(p.repo), t.fg2);
    } else if (c == "number") {
      cell_y(mfs);
      text(std::to_string(p.number), t.muted, f.mono, code());
    } else if (c == "size") {
      if (p.partial) continue;
      cell_y(mfs);
      segments({{"+" + compact(p.additions), t.success}, {" ", t.fg}, {"−" + compact(p.deletions), t.danger}}, f.mono, code());
    } else if (c == "blocks") {
      if (p.partial) continue;
      cell_y(rh);
      diffstat_blocks(p.additions, p.deletions, rh / k());
    } else if (c == "state") {
      cell_y(18 * k());
      state_text(state_pill(derive_state(p, status_of(p.url))), t.fg2, 18);
    } else if (c == "age") {
      if (!p.created) continue;
      std::string a = age(now() - p.created);
      ImVec2 as = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, a.c_str());
      cell_y(as.y);
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - as.x);
      text(a, t.muted, nullptr, 11);
    } else if (c == "author") {
      if (p.author.empty()) continue;
      cell_y(14 * k());
      avatar(p.author);
      ImGui::SameLine(0, 6 * k());
      cell_y(fs);
      ImVec2 at = ImGui::GetCursorScreenPos();
      float right = at.x + ImGui::GetContentRegionAvail().x;
      std::string name = who(p.author);
      ImGui::PushStyleColor(ImGuiCol_Text, col(t.fg2));
      ImGui::RenderTextEllipsis(d, at, ImVec2(right, at.y + fs), right, name.c_str(), nullptr, nullptr);
      ImGui::PopStyleColor();
      ImGui::Dummy(ImVec2(right - at.x, fs));
    } else if (c == "title") {
      cell_y(fs);
      float avail = ImGui::GetContentRegionAvail().x - 10 * k();
      // labels as chips, then the plugins' tags, after the title, which gets what's left
      struct Bit {
        std::string s;
        const Person* who;
        bool tag;
      };
      std::vector<Bit> bits;
      if (cfg.gui.shows("labels"))
        for (auto& l : p.labels) bits.push_back({l, nullptr, false});
      if (cfg.gui.shows("tags"))
        for (auto& tg : tags_of(p.url))
          if (std::string s = strip_emoji(tg.glyph + " " + tg.text); !s.empty()) bits.push_back({s, tag_person(tg), true});
      float lfs = 10 * k(), bits_w = 0;
      for (auto& b : bits)
        bits_w += (b.tag ? f.mono : f.regular)->CalcTextSizeA(lfs, FLT_MAX, 0, b.s.c_str()).x + (b.tag ? 0 : 12 * k()) +
                  (b.who ? 9 * k() : 0) + 6 * k();
      std::string title = p.partial ? p.url : shown_title(p.title);
      float tw = std::min(f.regular->CalcTextSizeA(fs, FLT_MAX, 0, title.c_str()).x, std::max(avail * 0.6f, avail - bits_w));
      ImVec2 tp = ImGui::GetCursorScreenPos();
      ImGui::RenderTextEllipsis(d, tp, ImVec2(tp.x + tw, tp.y + fs), tp.x + tw, title.c_str(), nullptr, nullptr);
      float x = tp.x + tw + 6 * k(), clip = tp.x + avail;
      for (auto& b : bits) {
        ImFont* bf = b.tag ? f.mono : f.regular;
        ImVec2 ss = bf->CalcTextSizeA(lfs, FLT_MAX, 0, b.s.c_str());
        float w = ss.x + (b.tag ? 0 : 12 * k()) + (b.who ? 9 * k() : 0), h = 16 * k();
        if (x + w > clip) break;
        float y = tp.y + (fs - h) / 2, tx = x + (b.tag ? 0 : 6 * k());
        if (!b.tag) d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(t.border), h / 2);
        if (b.who && b.who->rgb >= 0) {
          d->AddCircleFilled(ImVec2(tx + 3 * k(), y + h / 2), 3 * k(), IM_COL32(b.who->rgb >> 16, (b.who->rgb >> 8) & 0xff, b.who->rgb & 0xff, 255));
          tx += 9 * k();
        }
        d->AddText(bf, lfs, ImVec2(tx, y + (h - ss.y) / 2), col(t.muted), b.s.c_str());
        x += w + 6 * k();
      }
      ImGui::Dummy(ImVec2(avail, fs));
    }
  }
}

void Gui::prs_pane() {
  std::string title = view().name + "  " + std::to_string(cur_rows.size()) + "###prs";
  if (focus_list) {
    ImGui::SetNextWindowFocus();
    focus_list = false;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool shown = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                       ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  // the filter bar, padded 8px 10px; a line under it
  ImGui::SetCursorPos(ImGui::GetCursorStartPos() + ImVec2(10 * k(), 8 * k()));
  ImGui::BeginChild("##filterarea", ImVec2(ImGui::GetContentRegionAvail().x - 10 * k(), 0), ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar);
  filter_bar();
  ImGui::EndChild();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float wx = ImGui::GetWindowPos().x;
  ImGui::GetWindowDrawList()->AddLine(ImVec2(wx, p.y + 2 * k()), ImVec2(wx + ImGui::GetWindowWidth(), p.y + 2 * k()), col(t.border));
  ImGui::SetCursorPos(ImVec2(0, ImGui::GetCursorPosY() + 3 * k()));
  pr_table();
  ImGui::End();
}

// -- the preview ---------------------------------------------------------------------------

// What each plugin says about a PR, a row each: a dot, its name, one line
// of what it says (its run, its status, its tags and the start of its
// note; all of it on hover) and its actions as 20px buttons.
void Gui::plugins_box(const Pr& p) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  bool any = false;
  for (size_t i = 0; i < cfg.plugins.size(); ++i) {
    auto it = live[i].prs.find(p.url);
    std::vector<Action> acts;
    if (!one_shot(i))
      for (auto& a : live[i].actions) acts.push_back(a);
    std::string problem = plugin_problem(i);
    if (it == live[i].prs.end() && acts.empty() && problem.empty()) continue;
    ImGui::PushID(static_cast<int>(i));
    const PrInfo* info = it == live[i].prs.end() ? nullptr : &it->second;
    std::string line, full;
    uint32_t lc = t.muted, dot = t.success;
    if (!problem.empty()) {
      line = (live[i].health == "exited" ? "not running · " : "error · ") + first_line(problem);
      full = problem;
      lc = dot = t.danger;
    } else if (info && info->marks_summary) {
      const Mark& m = *info->marks_summary;
      std::string why = first_line(m.text);
      if (auto c = why.find(": "); m.banner && c != std::string::npos && why.substr(0, c).find(' ') != std::string::npos)
        why = why.substr(c + 2);
      line = m.banner ? "run failed · " + why : unclocked(why) == why ? why : first_line(m.text);
      full = m.text;
      Result r = m.banner ? Result::Failed : result_of(m);
      lc = r == Result::Passed ? t.success : r == Result::Failed || r == Result::Error ? t.danger
           : r == Result::Running ? t.accent : r == Result::Snoozed ? t.attention : t.muted;
      dot = lc == t.muted ? t.neutral : lc;
    } else if (info) {
      std::vector<std::string> bits;
      if (info->status) bits.push_back(strip_emoji(info->status->glyph + " " + info->status->text));
      for (auto& tg : info->tags)
        if (std::string s = strip_emoji(tg.glyph + " " + tg.text); !s.empty()) bits.push_back(s);
      if (!info->note.empty())
        for (auto& b : md::blocks(info->note))
          if (b.kind == md::Block::Para) {
            std::string note;
            for (auto& sp : b.spans) note += sp.text == "\n" ? " " : sp.text;
            bits.push_back(note);
            break;
          }
      for (auto& bit : bits) line += (line.empty() ? "" : " · ") + bit;
      full = line;
      if (info->status) lc = t.accent;
      if (bits.empty()) line = "nothing on this PR";
    } else {
      line = "nothing on this PR";
      dot = live[i].health == "ok" ? t.neutral : t.attention;
    }
    ImVec2 q = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x, rh = 28 * k(), fs = ImGui::GetFontSize(), mfs = 11 * k();
    if (any) d->AddLine(q, ImVec2(q.x + W, q.y), col(t.border_muted));
    any = true;
    // the buttons, right-aligned
    float bw = 0;
    for (auto& a : acts) bw += button_width(strip_emoji(a.button.empty() ? a.label : a.button), Btn::Default, a.key, nullptr, 20) + 4 * k();
    float x = q.x + W - bw + 4 * k();
    for (auto& a : acts) {
      ImGui::SetCursorScreenPos(ImVec2(x, q.y + (rh - 20 * k()) / 2));
      if (button(strip_emoji(a.button.empty() ? a.label : a.button) + "##" + a.id, Btn::Default, 20, a.key)) plugin_action(a.key, p.url);
      x = ImGui::GetItemRectMax().x + 4 * k();
    }
    d->AddCircleFilled(ImVec2(q.x + 3 * k(), q.y + rh / 2), 3 * k(), col(dot));
    const std::string& name = cfg.plugins[i].name;
    d->AddText(f.semibold, fs, ImVec2(q.x + 14 * k(), q.y + (rh - fs) / 2), col(t.fg), name.c_str());
    float lx = q.x + 14 * k() + std::max(44 * k(), f.semibold->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str()).x + 8 * k());
    float lr = q.x + W - bw - 8 * k();
    ImGui::PushFont(f.regular, mfs);
    ImGui::PushStyleColor(ImGuiCol_Text, col(lc));
    ImGui::RenderTextEllipsis(d, ImVec2(lx, q.y + (rh - mfs) / 2), ImVec2(lr, q.y + rh), lr, strip_emoji(line).c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(lx, q.y));
    ImGui::InvisibleButton("##line", ImVec2(std::max(1.f, lr - lx), rh));
    if (ImGui::IsItemHovered() && !full.empty()) {
      ImGui::SetNextWindowSize(ImVec2(360 * k(), 0));
      ImGui::BeginTooltip();
      text_wrapped(strip_emoji(full), t.fg, nullptr, 11);
      ImGui::EndTooltip();
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rh));
    ImGui::Dummy(ImVec2(W, 0));
    ImGui::PopID();
  }
  if (!any) {
    ImGui::Dummy(ImVec2(1, 4 * k()));
    text(cfg.plugins.empty() ? "No plugins configured." : "No plugin says anything about this PR.", t.muted, nullptr, 11);
  }
}

// A PR's meta line, as Preview and the reader draw it (11px): its state,
// the author and age, then what's given in mono.
void Gui::meta_line(const Pr& p, const std::vector<std::pair<std::string, uint32_t>>& mono) {
  float h = 16 * k(), fs = 11 * k();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float y = o.y + (h - fs) / 2, x = o.x;
  auto put = [&](ImFont* fnt, const std::string& s, uint32_t c) {
    d->AddText(fnt, fs, ImVec2(x, y), col(c), s.c_str());
    x += fnt->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str()).x;
  };
  Pill pl = state_pill(derive_state(p, status_of(p.url)));
  d->AddCircleFilled(ImVec2(x + 3 * k(), o.y + h / 2), 3 * k(), col(tone_fg(pl.tone)));
  x += 11 * k();
  put(f.regular, strip_emoji(pl.text), tone_fg(pl.tone));
  x += 8 * k();
  if (!p.author.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(x, o.y + 1 * k()));
    avatar(p.author);
    x += 19 * k();
    put(f.medium, who(p.author), t.fg2);
    put(f.regular, " · " + age(now() - p.created) + " ago", t.muted);
    x += 8 * k();
  }
  for (auto& [s, c] : mono) put(f.mono, s, c);
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(x - o.x, h));
}

void Gui::preview_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 12 * k()));
  bool shown = ImGui::Begin("Preview###preview", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  const Pr* cur = selected_pr();
  if (!cur) {
    text(loading() ? "Loading…" : "No pull request selected.", t.muted);
    return ImGui::End();
  }
  const Pr& p = *cur;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * k(), 4 * k()));
  text(p.repo + "#" + std::to_string(p.number), t.muted, f.mono, code());
  text_wrapped(p.partial ? p.url : shown_title(p.title), t.fg, f.semibold, 14);
  if (!p.partial)
    meta_line(p, {{"+" + std::to_string(p.additions), t.success}, {" ", t.muted}, {"−" + std::to_string(p.deletions), t.danger},
                  {" · " + std::to_string(p.changed_files) + (p.changed_files == 1 ? " file" : " files"), t.muted}});
  ImGui::Dummy(ImVec2(1, 8 * k()));
  if (button("Open in reader", Btn::Primary, 24, "↵")) open_reader(p);
  ImGui::SameLine();
  if (button("GitHub", Btn::Default, 24, "", 0, icon_external)) open(p.url);
  ImGui::SameLine();
  if (icon_button("##copy", icon_copy, 24, true, "Copy URL")) {
    ImGui::SetClipboardText(p.url.c_str());
    flash("copied " + p.url);
  }
  ImGui::Dummy(ImVec2(1, 8 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * k(), 0));
  section("Reviewers");
  {
    std::set<std::string> named;
    bool any = false;
    float rh = 24 * k(), fs = ImGui::GetFontSize();
    auto row = [&](const std::string& login, const std::string& status, uint32_t c) {
      ImVec2 q = ImGui::GetCursorScreenPos();
      float W = ImGui::GetContentRegionAvail().x;
      ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + (rh - 14 * k()) / 2));
      avatar(login);
      ImDrawList* d = ImGui::GetWindowDrawList();
      std::string name = login == viewer ? who(login) + " (you)" : who(login);
      d->AddText(f.regular, fs, ImVec2(q.x + 20 * k(), q.y + (rh - fs) / 2), col(t.fg), name.c_str());
      ImVec2 ss = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, status.c_str());
      d->AddText(f.regular, 11 * k(), ImVec2(q.x + W - ss.x, q.y + (rh - ss.y) / 2), col(c), status.c_str());
      ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + rh));
      ImGui::Dummy(ImVec2(W, 0));
      any = true;
    };
    for (auto& [login, state] : p.reviews) {
      Pill rp = review_pill(state);
      row(login, lower(rp.text), tone_fg(rp.tone) == t.neutral || rp.tone == Tone::Attention ? t.muted : tone_fg(rp.tone));
      named.insert(lower(who(login)));
    }
    // people a plugin tags on the PR (anton-slkx's 👀 reviewers)
    for (size_t i = 0; i < cfg.plugins.size(); ++i)
      if (auto it = live[i].prs.find(p.url); it != live[i].prs.end())
        for (auto& tg : it->second.tags) {
          const Person* who_ = tag_person(tg);
          if (!who_ || named.count(lower(tg.text))) continue;
          std::string login = tg.text == "you" ? viewer : tg.text;
          for (auto& [l, pp] : cfg.people)
            if (&pp == who_) login = l;
          named.insert(lower(tg.text));
          row(login, "tagged by " + cfg.plugins[i].name, t.attention);
        }
    if (!any) {
      ImGui::Dummy(ImVec2(1, 4 * k()));
      text(p.review_decision == "REVIEW_REQUIRED" ? "No reviews yet · review required" : "No reviews yet", t.muted, nullptr, 11);
    }
  }
  ImGui::Dummy(ImVec2(1, 12 * k()));
  section("Plugins");
  plugins_box(p);
  ImGui::Dummy(ImVec2(1, 12 * k()));
  section("Description");
  ImGui::Dummy(ImVec2(1, 6 * k()));
  ImGui::PopStyleVar();
  want_body(p.url, p.updated);
  const Body& b = bodies[p.url];
  if (b.loading && b.text.empty()) text("Loading…", t.muted);
  else if (!b.error.empty() && b.text.empty()) text_wrapped(b.error, t.danger);
  else if (b.text.find_first_not_of(" \r\n") == std::string::npos) text("No description.", t.muted);
  else markdown(b.text, "", false, false, true);
  ImGui::PopStyleVar();
  ImGui::End();
}

}  // namespace anton::gui
