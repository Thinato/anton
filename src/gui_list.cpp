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

void Gui::open_reader(const Pr& p) {
  prepare_reader(p);
  screen = Screen::Reader;
}

void Gui::act(const std::string& key) {
  const Pr* cur = selected_pr();
  if (key == "⏎") {
    if (cur) open_reader(*cur);
  } else if (key == "w") {
    if (cur) open(cur->url);
  } else if (key == "y") {
    std::string url = screen == Screen::Reader ? reader_url : cur ? cur->url : "";
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
      focus_llm = true;
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
  ImGui::OpenPopup("##name");
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * k(), 8 * k()));
  bool shown = ImGui::Begin("Views###views", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 2 * k()));
  for (size_t i = 0; i < tabs.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 32 * k();
    if (ImGui::InvisibleButton("##view", ImVec2(w, h))) switch_view(i);
    bool hov = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##viewmenu")) {
      view_menu(i);
      ImGui::EndPopup();
    }
    if (hov && tabs[i].saved) ImGui::SetTooltip("%s", tabs[i].v.query.c_str());
    if (i == active) d->AddRectFilled(p, p + ImVec2(w, h), col(t.sel), 6 * k());
    else if (hov) d->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
    float x = p.x + 8 * k();
    // the count pill on the right, then the plugin, then the name
    auto rs = rows(i);
    std::string count = rs ? std::to_string(rs->size()) : "…";
    ImGui::PushFont(f.medium, 12 * k());
    ImVec2 cs = ImGui::CalcTextSize(count.c_str());
    float cw = std::max(20 * k(), cs.x + 12 * k()), cx = p.x + w - 8 * k() - cw;
    d->AddRectFilled(ImVec2(cx, p.y + (h - 18 * k()) / 2), ImVec2(cx + cw, p.y + (h + 18 * k()) / 2), col(t.neutral_muted), 9 * k());
    d->AddText(ImVec2(cx + (cw - cs.x) / 2, p.y + (h - cs.y) / 2), col(t.fg), count.c_str());
    ImGui::PopFont();
    float right = cx - 8 * k();
    if (tabs[i].v.is_list()) {
      ImGui::PushFont(f.regular, 11 * k());
      ImVec2 ps = ImGui::CalcTextSize(tabs[i].v.plugin.c_str());
      right -= ps.x;
      d->AddText(ImVec2(right, p.y + (h - ps.y) / 2), col(t.muted), tabs[i].v.plugin.c_str());
      right -= 8 * k();
      ImGui::PopFont();
    }
    ImGui::PushFont(i == active ? f.semibold : f.regular, 0.f);
    std::string name = tabs[i].v.name;
    ImVec2 ns = ImGui::CalcTextSize(name.c_str());
    d->PushClipRect(ImVec2(x, p.y), ImVec2(right - (modified(i) ? 12 * k() : 0), p.y + h), true);
    d->AddText(ImVec2(x, p.y + (h - ns.y) / 2), col(t.fg), name.c_str());
    d->PopClipRect();
    if (modified(i))  // filters changed since saved: the yellow dot
      d->AddCircleFilled(ImVec2(std::min(x + ns.x + 8 * k(), right - 6 * k()), p.y + h / 2), 3 * k(), col(t.attention));
    ImGui::PopFont();
    ImGui::PopID();
  }
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 32 * k();
    if (ImGui::InvisibleButton("##newview", ImVec2(w, h))) act("+");
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
    icon_plus(d, ImVec2(p.x + 8 * k(), p.y + (h - 14 * k()) / 2), 14 * k(), col(t.muted));
    d->AddText(ImVec2(p.x + 30 * k(), p.y + (h - ImGui::GetTextLineHeight()) / 2), col(t.muted), "New view");
  }
  ImGui::PopStyleVar();
  // the repositories in this view: a click scopes every view to one
  ImVec2 p = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  ImGui::Dummy(ImVec2(w, 8 * k()));
  d->AddLine(ImVec2(p.x + 8 * k(), p.y + 4 * k()), ImVec2(p.x + w - 8 * k(), p.y + 4 * k()), col(t.border_muted));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8 * k());
  ImGui::Dummy(ImVec2(1, 2 * k()));
  ImGui::SetCursorPosX(16 * k());
  section("Repositories");
  std::map<std::string, int> counts;
  for (auto& pr : cur_rows) ++counts[pr.repo];
  for (auto& r : repos)
    if (!counts.count(r)) counts[r] = 0;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 2 * k()));
  size_t n = 0;
  for (auto& [repo, cnt] : counts) {
    ImVec2 q = ImGui::GetCursorScreenPos();
    float h = 28 * k();
    bool on = scope == "repo:" + repo;
    if (ImGui::InvisibleButton(("##repo" + repo).c_str(), ImVec2(w, h))) {
      if (on) {
        scope.clear();
        reset_selection();
        refresh();
      } else {
        scope = "repo:" + repo;
        reset_selection();
        refresh();
      }
    }
    if (ImGui::IsItemHovered()) {
      d->AddRectFilled(q, q + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
      ImGui::SetTooltip(on ? "Show every repository" : "Only %s, in every view (x clears)", repo.c_str());
    }
    if (on) d->AddRectFilled(q, q + ImVec2(w, h), col(t.sel), 6 * k());
    d->AddCircleFilled(ImVec2(q.x + 12 * k(), q.y + h / 2), 4 * k(), col(repo_dot(t, n++)));
    float ty = q.y + (h - ImGui::GetTextLineHeight()) / 2;
    d->AddText(ImVec2(q.x + 24 * k(), ty), col(t.fg), repo.c_str());
    ImGui::PushFont(nullptr, 12 * k());
    std::string c = std::to_string(cnt);
    ImVec2 cs = ImGui::CalcTextSize(c.c_str());
    d->AddText(ImVec2(q.x + w - 8 * k() - cs.x, q.y + (h - cs.y) / 2), col(t.muted), c.c_str());
    ImGui::PopFont();
  }
  ImGui::PopStyleVar();
  if (confirm_delete >= 0) ImGui::OpenPopup("##delete");
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
  ImGui::End();
}

// -- the filter bar ------------------------------------------------------------------

static std::string sort_label(Sort s) {
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
  // the query, in an input like GitHub's filter box
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = 32 * k();
    d->AddRectFilled(p, p + ImVec2(W, h), col(t.input_bg), 6 * k());
    bool active_q = ImGui::GetActiveID() == ImGui::GetID("##query");
    d->AddRect(p, p + ImVec2(W, h), col(active_q ? t.accent : t.border), 6 * k(), active_q ? 2.f : 1.f);
    icon_filter(d, ImVec2(p.x + 10 * k(), p.y + (h - 14 * k()) / 2), 14 * k(), col(t.muted));
    if (!active_q && !q_dirty) qbuf = q;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 32 * k(), p.y + (h - (12 * k() + 6 * k())) / 2));
    ImGui::PushFont(f.mono, 12 * k());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 3 * k()));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, 0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, 0);
    ImGui::SetNextItemWidth(W - 32 * k() - (active_q ? 10 : 140) * k());
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
    if (!editing) {
      ImGui::PushFont(nullptr, 12 * k());
      const char* hint = "click a chip or type";
      ImVec2 hs = ImGui::CalcTextSize(hint);
      d->AddText(ImVec2(p.x + W - 10 * k() - hs.x, p.y + (h - hs.y) / 2), col(t.muted), hint);
      ImGui::PopFont();
    } else {  // what Tab would complete
      size_t ws = word_start(qbuf, qbuf.size());
      auto comps = complete(qbuf.substr(ws), seen(), list);
      if (!comps.empty()) {
        ImGui::SetNextWindowPos(ImVec2(p.x + 32 * k(), p.y + h + 2 * k()));
        ImGui::BeginTooltip();
        for (size_t i = 0; i < comps.size() && i < 8; ++i) {
          segments({{comps[i].label, i == 0 ? t.accent : t.fg}, {comps[i].hint.empty() ? "" : "   " + comps[i].hint, t.muted}},
                   f.mono, 12);
        }
        text("Tab completes", t.muted, nullptr, 11);
        ImGui::EndTooltip();
      }
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 10 * k()));
  }
  // the chips, "+ Filter", then Sort and Columns on the right
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * k(), 6 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * k(), 6 * k()));
  float x0 = ImGui::GetCursorPosX(), xmax = x0 + ImGui::GetContentRegionAvail().x, right_w = 0;
  // Sort and Columns' width, measured first so chips wrap before them
  std::string sort_text = sort_label(view().sort_or_default());
  ImGui::PushFont(f.medium, 12 * k());
  right_w = ImGui::CalcTextSize(("Sort " + sort_text).c_str()).x + 46 * k() + ImGui::CalcTextSize("Columns").x + 44 * k() + 6 * k();
  ImGui::PopFont();
  auto chip = [&](const std::string& id, const std::string& key, const std::string& value, bool on) {
    ImGui::PushFont(f.medium, 12 * k());
    ImVec2 ks = ImGui::CalcTextSize(key.c_str()), vs = ImGui::CalcTextSize(value.c_str());
    float h = 26 * k(), w = 10 * k() + ks.x + 4 * k() + vs.x + 6 * k() + 10 * k() + 10 * k();
    float avail = xmax - ImGui::GetCursorPosX() - right_w;
    if (ImGui::GetCursorPosX() > x0 + 1 && w > avail) ImGui::NewLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    bool on_x = on && hov && ImGui::GetIO().MousePos.x > p.x + w - 22 * k();
    d->AddRectFilled(p, p + ImVec2(w, h), col(on ? t.accent_muted : hov ? (t.dark ? t.border_muted : t.inset) : t.btn), h / 2);
    d->AddRect(p, p + ImVec2(w, h), col(on ? t.accent : t.border), h / 2);
    float x = p.x + 10 * k(), y = p.y + (h - ks.y) / 2;
    d->AddText(ImVec2(x, y), col(on ? t.accent : t.muted), key.c_str());
    d->AddText(ImVec2(x + ks.x + 4 * k(), y), col(on ? t.accent : t.fg), value.c_str());
    ImVec2 ip(p.x + w - 20 * k(), p.y + (h - 10 * k()) / 2);
    if (on) icon_cross(d, ip, 10 * k(), col(on_x ? t.fg : t.accent));
    else icon_chevron(d, ip, 10 * k(), col(t.fg));
    ImGui::PopFont();
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
    ImGui::PushFont(f.medium, 12 * k());
    float w = ImGui::CalcTextSize("Filter").x + 30 * k(), h = 26 * k();
    if (ImGui::GetCursorPosX() > x0 + 1 && w > xmax - ImGui::GetCursorPosX() - right_w)
      ImGui::NewLine();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##addfilter", ImVec2(w, h)) || open_add_filter) {
      ImGui::OpenPopup("##addmenu");
      open_add_filter = false;
    }
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(t.accent_muted), h / 2);
    icon_plus(d, ImVec2(p.x + 8 * k(), p.y + (h - 12 * k()) / 2), 12 * k(), col(t.accent));
    d->AddText(ImVec2(p.x + 24 * k(), p.y + (h - ImGui::GetTextLineHeight()) / 2), col(t.accent), "Filter");
    ImGui::PopFont();
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
  // Sort and Columns, right-aligned on this line
  {
    float x = xmax - right_w;
    if (ImGui::GetCursorPosX() > x) ImGui::NewLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), x));
    ImGui::PushFont(f.medium, 12 * k());
    ImVec2 p = ImGui::GetCursorScreenPos();
    float sw = ImGui::CalcTextSize(("Sort " + sort_text).c_str()).x + 40 * k(), h = 26 * k();
    if (ImGui::InvisibleButton("##sort", ImVec2(sw, h))) ImGui::OpenPopup("##sortmenu");
    bool hov = ImGui::IsItemHovered();
    d->AddRectFilled(p, p + ImVec2(sw, h), col(hov ? (t.dark ? t.border_muted : t.inset) : t.btn), 6 * k());
    d->AddRect(p, p + ImVec2(sw, h), col(t.border), 6 * k());
    float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
    d->AddText(ImVec2(p.x + 10 * k(), ty), col(t.muted), "Sort");
    d->AddText(ImVec2(p.x + 10 * k() + ImGui::CalcTextSize("Sort ").x, ty), col(t.fg), sort_text.c_str());
    icon_chevron(d, ImVec2(p.x + sw - 20 * k(), p.y + (h - 10 * k()) / 2), 10 * k(), col(t.fg));
    ImGui::SameLine();
    ImVec2 c = ImGui::GetCursorScreenPos();
    float cw = ImGui::CalcTextSize("Columns").x + 38 * k();
    if (ImGui::InvisibleButton("##columns", ImVec2(cw, h))) ImGui::OpenPopup("##colmenu");
    hov = ImGui::IsItemHovered();
    d->AddRectFilled(c, c + ImVec2(cw, h), col(hov ? (t.dark ? t.border_muted : t.inset) : t.btn), 6 * k());
    d->AddRect(c, c + ImVec2(cw, h), col(t.border), 6 * k());
    icon_columns(d, ImVec2(c.x + 10 * k(), c.y + (h - 12 * k()) / 2), 12 * k(), col(t.fg));
    d->AddText(ImVec2(c.x + 28 * k(), ty), col(t.fg), "Columns");
    ImGui::PopFont();
    if (ImGui::BeginPopup("##sortmenu")) {
      static const char* what[] = {"the plugin's order", "last updated first", "newest first", "biggest first", "by repo"};
      for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
        if (s == Sort::List && !list) continue;
        if (ImGui::MenuItem(sort_label(s).c_str(), what[static_cast<int>(s)], view().sort_or_default() == s)) set_sort(s);
      }
      ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##colmenu")) {
      section("Columns (shared with the terminal UI)");
      int shown = 0;
      for (auto& cl : cfg.columns) shown += !cl.hidden;
      for (size_t i = 0; i < cfg.columns.size(); ++i) {
        Column& cl = cfg.columns[i];
        if (!cl.command.empty()) {
          ImGui::MenuItem(cl.name.c_str(), "command column: terminal UI only", false, false);
          continue;
        }
        bool on = !cl.hidden;
        std::string label = cl.name == "number" ? "#" : cl.name == "tags" ? "tags (as pills by the title)" : cl.name;
        if (ImGui::MenuItem(label.c_str(), nullptr, on, !on || shown > 1)) {
          cl.hidden = on;
          save("saved");
        }
      }
      ImGui::EndPopup();
    }
  }
  ImGui::NewLine();
  ImGui::PopStyleVar(2);
  // a view whose filters changed: Save, Save as new view, Revert
  if (modified(active)) {
    ImGui::Dummy(ImVec2(1, 2 * k()));
    ImVec2 p = ImGui::GetCursorScreenPos();
    d->AddCircleFilled(ImVec2(p.x + 4 * k(), p.y + 13 * k()), 3.5f * k(), col(t.attention));
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14 * k(), p.y + (26 * k() - ImGui::GetTextLineHeight()) / 2));
    text(view().name + (tabs[active].saved ? " changed" : " isn't saved yet"), t.muted, nullptr, 12);
    ImGui::SameLine(0, 12 * k());
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
    if (button("Save", Btn::Default, 26, "S")) act("S");
    ImGui::SameLine();
    if (button("Save as new view", Btn::Ghost, 26)) ask_name(Ask::SaveAs, active);
    if (tabs[active].saved) {
      ImGui::SameLine();
      if (button("Revert", Btn::Ghost, 26)) revert_view(active);
    }
  }
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

  // the columns config.json shows, in its order; tags go by the title
  std::vector<std::string> cols;
  bool tags = false;
  for (auto& c : cfg.columns) {
    if (c.hidden || !c.command.empty()) continue;
    if (c.name == "tags") tags = true;
    else cols.push_back(c.name);
  }
  if (cols.empty()) cols.push_back("title");
  std::set<std::string> owners_here;
  for (auto& p : cur_rows) owners_here.insert(p.owner);
  bool show_owner = owners_here.size() > 1;
  float rh = row_height(cfg.gui.density) * k();
  ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_SizingFixedFit |
                       ImGuiTableFlags_NoSavedSettings;
  ImGui::PushStyleColor(ImGuiCol_TableBorderLight, col(t.border_muted));
  if (!ImGui::BeginTable("##prs", static_cast<int>(cols.size()), fl)) {
    ImGui::PopStyleColor();
    return;
  }
  for (auto& c : cols) {
    // the canvas's widths, less the cells' padding
    float w = (c == "repo" ? (show_owner ? 120 : 72) : c == "number" ? 56 : c == "size" ? 132 : c == "state" ? 140
               : c == "age" ? 56 : c == "author" ? 104 : 0) - 8;
    if (c == "title") {
      ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthStretch);
    } else {
      ImGui::TableSetupColumn(c.c_str(), ImGuiTableColumnFlags_WidthFixed, w * k());
    }
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  // the header row: Repo, Size and Age sort on a click; the sort shows ↓
  Sort sort = v.sort_or_default();
  ImGui::TableNextRow(ImGuiTableRowFlags_Headers, 32 * k());
  ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, col(t.panel));
  for (size_t i = 0; i < cols.size(); ++i) {
    ImGui::TableSetColumnIndex(static_cast<int>(i));
    const std::string& c = cols[i];
    std::optional<Sort> key = c == "repo" ? std::optional(Sort::Repo) : c == "size" ? std::optional(Sort::Size)
                              : c == "age" ? std::optional(Sort::Created) : std::nullopt;
    std::string label = c == "number" ? "#" : std::string(1, static_cast<char>(std::toupper(c[0]))) + c.substr(1);
    bool on = key && *key == sort;
    if (on) label += " ↓";
    float y = ImGui::GetCursorPosY();
    if (i == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16 * k());
    ImGui::PushFont(f.semibold, 12 * k());
    ImGui::SetCursorPosY(y + (32 * k() - ImGui::GetTextLineHeight()) / 2);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    if (key) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::InvisibleButton("##h", ImVec2(std::max(ts.x, 20.f), ts.y))) set_sort(*key);
      bool hov = ImGui::IsItemHovered();
      if (hov) ImGui::SetTooltip("Sort by %s", sort_label(*key).c_str());
      ImGui::GetWindowDrawList()->AddText(p, col(on || hov ? t.fg : t.muted), label.c_str());
      ImGui::PopID();
    } else {
      text(label, t.muted);
    }
    ImGui::PopFont();
  }
  ImDrawList* d = ImGui::GetWindowDrawList();
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
    float lh = ImGui::GetTextLineHeight();
    auto cell_y = [&](float h) { ImGui::SetCursorPosY(y0 + (rh - h) / 2); };
    for (size_t ci = 0; ci < cols.size(); ++ci) {
      ImGui::TableSetColumnIndex(static_cast<int>(ci));
      const std::string& c = cols[ci];
      if (ci == 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16 * k());
      if (c == "repo") {
        cell_y(lh);
        text(show_owner ? p.repo : short_repo(p.repo), t.muted);
      } else if (c == "number") {
        ImGui::PushFont(f.mono, 12 * k());
        cell_y(ImGui::GetTextLineHeight());
        text(std::to_string(p.number), t.muted);
        ImGui::PopFont();
      } else if (c == "size") {
        if (p.partial) continue;
        ImGui::PushFont(f.mono, 12 * k());
        cell_y(ImGui::GetTextLineHeight());
        float x = ImGui::GetCursorPosX();
        text("+" + compact(p.additions), t.success);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(x + 44 * k());
        text("−" + compact(p.deletions), t.danger);
        ImGui::SameLine(0, 0);
        ImGui::SetCursorPosX(x + 88 * k());
        diffstat_blocks(p.additions, p.deletions);
        ImGui::PopFont();
      } else if (c == "state") {
        cell_y(22 * k());
        Pill pl = state_pill(derive_state(p, status_of(p.url)));
        pill(strip_emoji(pl.text), pl.tone);
      } else if (c == "age") {
        cell_y(lh);
        if (p.created) text(age(now() - p.created), t.muted);
      } else if (c == "author") {
        if (p.author.empty()) continue;
        cell_y(20 * k());
        avatar(p.author);
        ImGui::SameLine(0, 6 * k());
        cell_y(lh);
        text(who(p.author), t.fg);
      } else if (c == "title") {
        cell_y(lh);
        float avail = ImGui::GetContentRegionAvail().x - 16 * k();
        // the labels' and tags' pills after the title, which gets what's left
        std::vector<std::pair<std::string, const Person*>> pills;
        for (auto& l : p.labels) pills.push_back({l, nullptr});
        if (tags)
          for (auto& tg : tags_of(p.url))
            if (std::string s = strip_emoji(tg.glyph + " " + tg.text); !s.empty()) pills.push_back({s, tag_person(tg)});
        ImGui::PushFont(nullptr, 11 * k());
        float pills_w = 0;
        for (auto& [s, who_] : pills) pills_w += ImGui::CalcTextSize(s.c_str()).x + 16 * k() + 6 * k() + (who_ ? 10 * k() : 0);
        ImGui::PopFont();
        std::string title = p.partial ? p.url : shown_title(p.title);
        ImGui::PushFont(f.medium, 0.f);
        float tw = std::min(ImGui::CalcTextSize(title.c_str()).x, std::max(avail * 0.6f, avail - pills_w));
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImGui::RenderTextEllipsis(d, tp, ImVec2(tp.x + tw, tp.y + lh), tp.x + tw, title.c_str(), nullptr, nullptr);
        ImGui::PopFont();
        float x = tp.x + tw + 6 * k();
        ImGui::PushFont(nullptr, 11 * k());
        float clip = tp.x + avail;
        for (auto& [s, pp] : pills) {
          ImVec2 ss = ImGui::CalcTextSize(s.c_str());
          float w = ss.x + 14 * k() + (pp ? 10 * k() : 0), h = 18 * k();
          if (x + w > clip) break;
          float y = tp.y + (lh - h) / 2;
          d->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), col(t.border), h / 2);
          float tx = x + 7 * k();
          if (pp && pp->rgb >= 0) {
            d->AddCircleFilled(ImVec2(tx + 3 * k(), y + h / 2), 3 * k(), IM_COL32(pp->rgb >> 16, (pp->rgb >> 8) & 0xff, pp->rgb & 0xff, 255));
            tx += 10 * k();
          }
          d->AddText(ImVec2(tx, y + (h - ss.y) / 2), col(t.muted), s.c_str());
          x += w + 6 * k();
        }
        ImGui::PopFont();
      }
    }
  }
  ImGui::EndTable();
  ImGui::PopStyleColor();
}

void Gui::prs_pane() {
  std::string title = view().name + "  " + std::to_string(cur_rows.size()) + "###prs";
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  bool shown = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                       ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImGui::SetCursorPos(ImGui::GetCursorStartPos() + ImVec2(16 * k(), 12 * k()));
  ImGui::BeginGroup();
  ImGui::PushItemWidth(-1);
  ImGui::BeginChild("##filterarea", ImVec2(ImGui::GetContentRegionAvail().x - 16 * k(), 0),
                    ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
  filter_bar();
  ImGui::EndChild();
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, p.y + 4 * k()),
                                      ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), p.y + 4 * k()),
                                      col(t.border_muted));
  ImGui::SetCursorPos(ImVec2(0, ImGui::GetCursorPosY() + 5 * k()));
  pr_table();
  ImGui::End();
}

// -- the preview ---------------------------------------------------------------------------

// What each plugin says about a PR: its state, tags, the start of its note,
// and its actions as buttons (temoc's "Run specs").
void Gui::plugins_box(const Pr& p, bool compact) {
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
    if (any) {
      ImVec2 q = ImGui::GetCursorScreenPos();
      d->AddLine(ImVec2(q.x - 12 * k(), q.y), ImVec2(q.x + ImGui::GetContentRegionAvail().x + 12 * k(), q.y), col(t.border_muted));
    }
    any = true;
    ImGui::Dummy(ImVec2(1, 10 * k()));
    const PrInfo* info = it == live[i].prs.end() ? nullptr : &it->second;
    std::string headline;
    uint32_t hcol = t.muted, dot = t.success;
    if (!problem.empty()) {
      headline = live[i].health == "exited" ? "not running" : "error";
      hcol = t.danger;
      dot = t.danger;
    } else if (info && info->marks_summary) {
      headline = info->marks_summary->banner ? "run failed" : first_line(info->marks_summary->text);
      Result r = result_of(*info->marks_summary);
      hcol = r == Result::Passed ? t.success : r == Result::Failed || r == Result::Error ? t.danger
             : r == Result::Running ? t.accent : r == Result::Snoozed ? t.attention : t.muted;
      dot = hcol == t.muted ? t.neutral : hcol;
    } else if (info && info->status) {
      headline = strip_emoji(info->status->glyph + " " + info->status->text);
      hcol = t.accent;
    } else if (!info) {
      headline = "nothing on this PR";
      dot = live[i].health == "ok" ? t.neutral : t.attention;
    }
    ImVec2 q = ImGui::GetCursorScreenPos();
    d->AddCircleFilled(ImVec2(q.x + 4 * k(), q.y + ImGui::GetTextLineHeight() / 2), 4 * k(), col(dot));
    float bx = ImGui::GetContentRegionAvail().x;
    // the actions, right-aligned on the first line
    std::vector<std::pair<std::string, Action>> btns;
    for (auto& a : acts) btns.push_back({strip_emoji(a.button.empty() ? a.label : a.button), a});
    float bw = 0;
    ImGui::PushFont(nullptr, 12 * k());
    for (auto& [label, a] : btns) bw += button_width(label, Btn::Default, a.key) + 6 * k();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(q.x + 18 * k(), q.y));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bx - 18 * k() - (compact ? 0 : bw) - 4 * k());
    segments({{cfg.plugins[i].name, t.fg}, {headline.empty() ? "" : "  ", t.fg}}, f.semibold);
    if (!headline.empty()) {
      ImGui::SameLine(0, 0);
      text(headline, hcol, nullptr, 12);
    }
    if (!problem.empty()) text_wrapped(problem, t.danger, f.mono, 12);
    if (info && info->marks_summary && info->marks_summary->banner) {
      std::string why = first_line(info->marks_summary->text);
      if (auto c = why.find(": "); c != std::string::npos && why.substr(0, c).find(' ') != std::string::npos) why = why.substr(c + 2);
      text_wrapped(why, t.danger, f.mono, 12);
    }
    if (info) {
      // a summary says what the tags would (anton-temoc's 🧪 2 ✗)
      for (auto& tg : info->marks_summary ? std::vector<Tag>{} : info->tags) {
        std::string s = strip_emoji(tg.glyph + " " + tg.text);
        if (s.empty()) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, col(t.muted));
        ImGui::TextWrapped("%s%s", s.c_str(), tg.hint.empty() ? "" : (" · " + strip_emoji(tg.hint)).c_str());
        ImGui::PopStyleColor();
      }
      if (!info->note.empty()) {
        std::string note;
        for (auto& b : md::blocks(info->note))
          if (b.kind == md::Block::Para) {
            for (auto& sp : b.spans) note += sp.text == "\n" ? " " : sp.text;
            break;
          }
        if (note.size() > 220) note = note.substr(0, 217) + "…";
        if (!note.empty()) text_wrapped(note, t.muted);
      }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    float end_y = ImGui::GetCursorScreenPos().y;
    if (!btns.empty()) {
      float x = compact ? q.x + 18 * k() : q.x + bx - bw + 6 * k();
      float y = compact ? end_y + 2 * k() : q.y - 3 * k();
      for (auto& [label, a] : btns) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushFont(nullptr, 12 * k());
        if (button(label + "##" + a.id, Btn::Default, 26, a.key)) plugin_action(a.key, p.url);
        ImGui::PopFont();
        x = ImGui::GetItemRectMax().x + 6 * k();
      }
      if (compact) end_y = ImGui::GetItemRectMax().y + 2 * k();
      end_y = std::max(end_y, ImGui::GetItemRectMax().y);
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x, end_y));
    ImGui::Dummy(ImVec2(1, 8 * k()));
    ImGui::PopID();
  }
  if (!any) {
    ImGui::Dummy(ImVec2(1, 8 * k()));
    text(cfg.plugins.empty() ? "No plugins configured." : "No plugin says anything about this PR.", t.muted);
    ImGui::Dummy(ImVec2(1, 8 * k()));
  }
}

// A box with a header strip, as the canvas's Reviewers and From plugins.
static void box(Gui& g, const char* title, const std::function<void()>& body) {
  ImGui::PushID(title);
  ImDrawList* d = ImGui::GetWindowDrawList();
  float w = ImGui::GetContentRegionAvail().x;
  ImVec2 p = ImGui::GetCursorScreenPos();
  float hh = 36 * g.k();
  d->ChannelsSplit(2);
  d->ChannelsSetCurrent(1);
  ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * g.k(), p.y + (hh - ImGui::GetTextLineHeight()) / 2));
  g.text(title, g.t.fg, g.f.semibold);
  ImGui::SetCursorScreenPos(ImVec2(p.x + 12 * g.k(), p.y + hh));
  ImGui::BeginGroup();
  ImGui::PushItemWidth(w - 24 * g.k());
  ImGui::BeginChild("##boxbody", ImVec2(w - 24 * g.k(), 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
  body();
  ImGui::EndChild();
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  float bottom = ImGui::GetItemRectMax().y;
  d->ChannelsSetCurrent(0);
  d->AddRectFilled(p, ImVec2(p.x + w, p.y + hh), g.col(g.t.panel), 6 * g.k(), ImDrawFlags_RoundCornersTop);
  d->AddLine(ImVec2(p.x, p.y + hh), ImVec2(p.x + w, p.y + hh), g.col(g.t.border));
  d->AddRect(p, ImVec2(p.x + w, bottom), g.col(g.t.border), 6 * g.k());
  d->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(p.x, bottom));
  ImGui::Dummy(ImVec2(w, 0));
  ImGui::PopID();
}

void Gui::preview_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 16 * k()));
  bool shown = ImGui::Begin("Preview###preview", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  const Pr* cur = selected_pr();
  if (!cur) {
    text(loading() ? "Loading…" : "No pull request selected.", t.muted);
    return ImGui::End();
  }
  const Pr& p = *cur;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 6 * k()));
  text(p.repo + "#" + std::to_string(p.number), t.muted, f.mono, 12);
  text_wrapped(p.partial ? p.url : shown_title(p.title), t.fg, f.semibold, 18);
  if (!p.partial) {
    Pill pl = state_pill(derive_state(p, status_of(p.url)));
    pill(strip_emoji(pl.text), pl.tone, 24);
    ImGui::SameLine(0, 8 * k());
    float y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(y + 2 * k());
    avatar(p.author);
    ImGui::SameLine(0, 6 * k());
    ImGui::SetCursorPosY(y + (24 * k() - ImGui::GetTextLineHeight()) / 2);
    text(who(p.author), t.fg, f.semibold);
    ImGui::SameLine(0, 8 * k());
    ImGui::SetCursorPosY(y + (24 * k() - ImGui::GetTextLineHeight()) / 2);
    text("opened " + age(now() - p.created) + " ago", t.muted);
    ImGui::PushFont(f.mono, 12 * k());
    text("+" + std::to_string(p.additions), t.success);
    ImGui::SameLine();
    text("−" + std::to_string(p.deletions), t.danger);
    ImGui::SameLine();
    diffstat_blocks(p.additions, p.deletions);
    ImGui::PopFont();
    ImGui::SameLine();
    text("in " + std::to_string(p.changed_files) + (p.changed_files == 1 ? " file" : " files"), t.muted);
  }
  ImGui::Dummy(ImVec2(1, 10 * k()));
  if (button("Open in reader", Btn::Primary, 32, "Enter")) open_reader(p);
  ImGui::SameLine();
  {
    if (button("GitHub", Btn::Default, 32, "", 0, icon_external)) open(p.url);
  }
  ImGui::SameLine();
  if (icon_button("##copy", icon_copy, 32, true, "Copy URL")) {
    ImGui::SetClipboardText(p.url.c_str());
    flash("copied " + p.url);
  }
  ImGui::Dummy(ImVec2(1, 10 * k()));
  box(*this, "Reviewers", [&] {
    std::set<std::string> named;
    bool any = false;
    auto row = [&](const std::string& login, const std::string& status, uint32_t c) {
      ImGui::Dummy(ImVec2(1, 6 * k()));
      float y = ImGui::GetCursorPosY();
      avatar(login);
      ImGui::SameLine(0, 8 * k());
      ImGui::SetCursorPosY(y + (20 * k() - ImGui::GetTextLineHeight()) / 2);
      text(login == viewer ? who(login) + " (you)" : who(login), t.fg, f.medium);
      ImGui::PushFont(nullptr, 12 * k());
      float sw = ImGui::CalcTextSize(status.c_str()).x;
      ImGui::SameLine(ImGui::GetWindowWidth() - sw);
      ImGui::SetCursorPosY(y + (20 * k() - ImGui::GetTextLineHeight()) / 2);
      text(status, c);
      ImGui::PopFont();
      ImGui::Dummy(ImVec2(1, 2 * k()));
      any = true;
    };
    for (auto& [login, state] : p.reviews) {
      Pill rp = review_pill(state);
      row(login, rp.text, tone_fg(rp.tone) == t.neutral ? t.muted : tone_fg(rp.tone));
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
      ImGui::Dummy(ImVec2(1, 6 * k()));
      text(p.review_decision == "REVIEW_REQUIRED" ? "No reviews yet · review required" : "No reviews yet", t.muted);
      ImGui::Dummy(ImVec2(1, 6 * k()));
    }
  });
  ImGui::Dummy(ImVec2(1, 8 * k()));
  box(*this, "From plugins", [&] { plugins_box(p, false); });
  ImGui::Dummy(ImVec2(1, 10 * k()));
  section("Description");
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
