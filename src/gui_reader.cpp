// The reader: a PR's description and markdown files, GitHub-style, with
// temoc's results on its spec files the way temoc-ui shows them; and the
// panes around it: Results, Outline, LLM review, Run log, Problems.
#include <algorithm>
#include <ctime>
#include <functional>

#include <imgui_internal.h>

#include "gui.hpp"

namespace anton::gui {

std::vector<Doc> Gui::docs() const {
  std::vector<Doc> out;
  auto b = bodies.find(reader_url);
  static const std::string empty;
  Doc desc{"", "Description", b == bodies.end() ? &empty : &b->second.text,
           b == bodies.end() ? "" : b->second.error, false, b == bodies.end() || b->second.loading};
  out.push_back(desc);
  if (const Files* fs = reader_files())
    for (auto& ft : fs->tabs) out.push_back({ft.path, ft.name, &ft.text, ft.error, ft.spec, false});
  return out;
}

// Parsed once per text (keyed by its hash): md4c needn't run every frame.
const std::vector<md::Block>& Gui::blocks_of(const std::string& text) {
  size_t h = std::hash<std::string>{}(text);
  auto it = parsed.find(h);
  if (it == parsed.end()) it = parsed.emplace(h, md::blocks(text, today())).first;
  return it->second;
}

void Gui::jump(const std::string& path, int src) {
  auto ds = docs();
  for (size_t i = 0; i < ds.size(); ++i)
    if (ds[i].path == path) reader_tab = i;
  jump_path = path;
  jump_src = src;
}

void Gui::focus_table(int by) {
  const FileTab* ft = reader_tab_file();
  std::vector<int> tests;
  for (auto& d : ft ? ft->dirs : std::vector<md::Directive>{})
    if (!d.import) tests.push_back(d.line);
  if (tests.empty()) return flash("no test tables in this file");
  int& fcs = focus[ft->path];
  auto it = std::find(tests.begin(), tests.end(), fcs);
  long at = it == tests.end() ? 0 : std::clamp<long>(it - tests.begin() + by, 0, static_cast<long>(tests.size()) - 1);
  fcs = tests[static_cast<size_t>(at)];
  jump(ft->path, fcs);
}

// -- rich text ----------------------------------------------------------------------

namespace {

struct Word {
  std::string s;
  unsigned style;
  bool space, brk;
};

std::vector<Word> words(const std::vector<md::Span>& spans) {
  std::vector<Word> out;
  for (auto& sp : spans) {
    if (sp.text == "\n") {
      out.push_back({"", sp.style, false, true});
      continue;
    }
    size_t at = 0;
    while (at <= sp.text.size()) {
      size_t s = sp.text.find(' ', at);
      std::string w = sp.text.substr(at, s == std::string::npos ? std::string::npos : s - at);
      if (!w.empty()) out.push_back({w, sp.style, false, false});
      if (s == std::string::npos) break;
      out.push_back({" ", sp.style, true, false});
      at = s + 1;
    }
  }
  return out;
}

}  // namespace

float Gui::rich_width(const std::vector<md::Span>& spans, float size) {
  float w = 0, best = 0;
  for (auto& wd : words(spans)) {
    if (wd.brk) {
      best = std::max(best, w);
      w = 0;
      continue;
    }
    ImFont* fnt = wd.style & md::Bold ? f.semibold : wd.style & md::Code ? f.mono : f.regular;
    float sz = (wd.style & md::Code ? size * 0.86f : size) * k();
    w += fnt->CalcTextSizeA(sz, FLT_MAX, 0, wd.s.c_str()).x + (wd.style & md::Code && !wd.space ? 4 * k() : 0);
  }
  return std::max(best, w);
}

float Gui::rich(const std::vector<md::Span>& spans, float width, float size, uint32_t color, bool measure) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float lh = std::round(size * 1.6f * k()), x = 0, y = 0;
  auto ws = words(spans);
  for (size_t i = 0; i < ws.size(); ++i) {
    const Word& wd = ws[i];
    if (wd.brk) {
      x = 0;
      y += lh;
      continue;
    }
    bool code = wd.style & md::Code;
    ImFont* fnt = wd.style & md::Bold ? f.semibold : code ? f.mono : f.regular;
    float sz = (code ? size * 0.86f : size) * k();
    ImVec2 ts = fnt->CalcTextSizeA(sz, FLT_MAX, 0, wd.s.c_str());
    float pad = code && !wd.space ? 4 * k() : 0;
    if (wd.space && x == 0) continue;
    if (!wd.space && x > 0 && x + ts.x + pad > width) {
      x = 0;
      y += lh;
    }
    if (wd.space && x + ts.x > width) continue;
    if (!measure) {
      uint32_t c = color;
      if (wd.style & md::Link) c = t.accent;
      if (wd.style & md::Dim) c = t.muted;
      if (wd.style & md::Output) c = t.accent;
      if (wd.style & md::Ok) c = t.success;
      if (wd.style & md::Error) c = t.danger;
      if (wd.style & md::Warn) c = t.attention;
      if (wd.style & md::Note) c = t.danger;
      float ty = o.y + y + (lh - ts.y) / 2;
      if (code && !wd.space)
        d->AddRectFilled(ImVec2(o.x + x, ty - 1 * k()), ImVec2(o.x + x + ts.x + pad, ty + ts.y + 1 * k()),
                         col(t.dark ? 0x656c7633u : 0x818b981fu), 4 * k());
      d->AddText(fnt, sz, ImVec2(o.x + x + pad / 2, ty), col(c), wd.s.c_str());
      if (wd.style & md::Strike)
        d->AddLine(ImVec2(o.x + x, ty + ts.y / 2), ImVec2(o.x + x + ts.x, ty + ts.y / 2), col(c));
      if (wd.style & md::Link && !wd.space)
        d->AddLine(ImVec2(o.x + x, ty + ts.y), ImVec2(o.x + x + ts.x, ty + ts.y), col(t.accent_muted));
    }
    x += ts.x + pad;
  }
  return y + lh;
}

// -- markdown --------------------------------------------------------------------------

namespace {

uint32_t level_fg(const Theme& t, const Mark& m) {
  Result r = result_of(m);
  if (r == Result::Running) return t.accent;
  return m.level == "ok" ? t.success : m.level == "error" ? t.danger : m.level == "warn" ? t.attention : t.muted;
}

// A row's tint by its result, as the canvas: passed a soft green, failed and
// error red, snoozed yellow, running blue; skipped and queued none.
uint32_t tint_of(const Theme& t, Result r) {
  switch (r) {
    case Result::Passed: return t.success_soft;
    case Result::Failed:
    case Result::Error: return t.danger_muted;
    case Result::Snoozed: return t.attention_muted;
    case Result::Running: return t.accent_muted;
    default: return 0;
  }
}

std::string plain(const std::vector<md::Span>& spans) {
  std::string s;
  for (auto& sp : spans) s += sp.text == "\n" ? " " : sp.text;
  return s;
}

std::string capital(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

}  // namespace

void Gui::markdown(const std::string& md_text, const std::string& path, bool spec, bool marks, bool compact) {
  (void)spec;
  const auto& bs = blocks_of(md_text);
  std::vector<Mark> ms;
  std::string head = bodies[reader_url].head;
  if (marks && !path.empty()) ms = marks_on(reader_url, path, head);
  auto at = attach(bs, ms);
  ImDrawList* d = ImGui::GetWindowDrawList();
  float gutter = marks ? 32 * k() : 0;
  float x0 = ImGui::GetCursorPosX();
  float W = std::min(ImGui::GetContentRegionAvail().x, (compact ? 10000 : 880) * k());
  float body = compact ? 13 : 14;
  auto line = [&]() { return ImGui::GetCursorScreenPos(); };
  auto gap = [&](float px) { ImGui::Dummy(ImVec2(1, px * k())); };
  auto gutter_mark = [&](const std::vector<Mark>* mks, float y, float h) {
    if (!mks || mks->empty()) return;
    const Mark* w = worst(*mks);
    std::string g = w->glyph.empty() ? "•" : w->glyph;
    ImGui::PushFont(f.mono, 13 * k());
    ImVec2 gs = ImGui::CalcTextSize(g.c_str());
    ImVec2 o = ImGui::GetWindowPos();
    float gx = o.x + x0 - ImGui::GetScrollX() + (gutter - gs.x) / 2;
    d->AddText(f.mono, 13 * k(), ImVec2(gx, y + (std::min(h, 22 * k()) - gs.y) / 2),
               col(level_fg(t, *w)), g.c_str());
    ImGui::PopFont();
  };
  auto under_notes = [&](const std::vector<Mark>* mks) {  // ✗ line 13 · text, under a line
    if (!mks) return;
    for (auto& m : *mks) {
      if (m.text.empty() || (m.level != "error" && m.level != "warn")) continue;
      ImGui::SetCursorPosX(x0 + gutter);
      text_wrapped((m.glyph.empty() ? "•" : m.glyph) + " line " + std::to_string(m.line) + " · " + m.text,
                   m.level == "error" ? t.danger : t.attention, f.mono, 12, W - gutter);
    }
  };
  for (size_t i = 0; i < bs.size(); ++i) {
    const md::Block& b = bs[i];
    bool jump_here = jump_src >= 0 && jump_path == path && !compact &&
                     (b.src >= jump_src || (b.kind == md::Block::Table && b.rows.back().src >= jump_src));
    if (jump_here && b.kind != md::Block::Table) {
      ImGui::SetScrollHereY(0.15f);
      jump_src = -1;
    }
    float indent = b.indent * 24 * k() + (b.quote ? 16 * k() : 0);
    float cx = x0 + gutter + indent, cw = W - gutter - indent;
    auto it = at.find(b.src);
    const std::vector<Mark>* mks = b.src > 0 && it != at.end() ? &it->second : nullptr;
    switch (b.kind) {
      case md::Block::Heading: {
        static const float big[] = {26, 20, 16, 14, 13, 13}, small[] = {17, 15, 14, 13, 13, 13};
        float sz = (compact ? small : big)[std::clamp(b.heading, 1, 6) - 1];
        if (i > 0) gap(compact ? 6 : 16);
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        std::vector<md::Span> sp = b.spans;
        for (auto& s : sp) s.style |= md::Bold;
        float h = rich(sp, cw, sz, t.fg);
        gutter_mark(mks, p.y, h);
        ImGui::Dummy(ImVec2(cw, h));
        if (b.heading <= 2 && !compact) {
          gap(b.heading == 1 ? 6 : 4);
          ImVec2 q = line();
          d->AddLine(ImVec2(q.x + cx - x0, q.y), ImVec2(q.x + cx - x0 + cw, q.y), col(t.border_muted));
          gap(1);
        }
        gap(compact ? 2 : 8);
        under_notes(mks);
        break;
      }
      case md::Block::Para: {
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        if (b.quote) d->AddRectFilled(ImVec2(p.x - 16 * k(), p.y), ImVec2(p.x - 12 * k(), p.y + 22 * k()), col(t.border));
        if (!b.bullet.empty()) {  // the list marker in the indent
          std::string m = b.bullet;
          while (!m.empty() && m.back() == ' ') m.pop_back();
          float mx = p.x - 18 * k(), my = p.y + std::round(body * 1.6f * k()) / 2;
          if (m == "•") d->AddCircleFilled(ImVec2(mx + 4 * k(), my), 2.5f * k(), col(t.fg));
          else if (m == "◦") d->AddCircle(ImVec2(mx + 4 * k(), my), 2.5f * k(), col(t.fg), 0, 1.2f * k());
          else if (m == "▪") d->AddRectFilled(ImVec2(mx + 2 * k(), my - 2 * k()), ImVec2(mx + 6 * k(), my + 2 * k()), col(t.fg));
          else if (m == "☐" || m == "☑") {
            ImVec2 a(mx - 2 * k(), my - 6.5f * k()), z(mx + 11 * k(), my + 6.5f * k());
            if (m == "☑") {
              d->AddRectFilled(a, z, col(t.accent), 3 * k());
              d->AddLine(ImVec2(a.x + 3 * k(), my), ImVec2(a.x + 5.5f * k(), my + 3 * k()), IM_COL32_WHITE, 1.6f * k());
              d->AddLine(ImVec2(a.x + 5.5f * k(), my + 3 * k()), ImVec2(a.x + 10 * k(), my - 3 * k()), IM_COL32_WHITE, 1.6f * k());
            } else {
              d->AddRect(a, z, col(t.border), 3 * k(), 1.2f * k());
            }
            ImGui::SetCursorPosX(cx + 2 * k());
            p = line();
          } else {
            ImGui::PushFont(f.regular, body * k());
            float mw = ImGui::CalcTextSize(m.c_str()).x;
            d->AddText(ImVec2(p.x - 6 * k() - mw, p.y + (std::round(body * 1.6f * k()) - ImGui::GetTextLineHeight()) / 2), col(t.fg), m.c_str());
            ImGui::PopFont();
          }
        }
        float h = rich(b.spans, cw, body, b.quote ? t.muted : t.fg);
        if (b.quote) d->AddRectFilled(ImVec2(p.x - 16 * k(), p.y), ImVec2(p.x - 12 * k(), p.y + h), col(t.border));
        gutter_mark(mks, p.y, h);
        ImGui::Dummy(ImVec2(cw, h));
        bool next_item = i + 1 < bs.size() && bs[i + 1].kind == md::Block::Para && bs[i + 1].indent > 0 && b.indent > 0;
        gap(next_item ? 2 : compact ? 6 : 12);
        under_notes(mks);
        break;
      }
      case md::Block::Code: {
        size_t j = i;
        while (j + 1 < bs.size() && bs[j + 1].kind == md::Block::Code && bs[j + 1].indent == b.indent) ++j;
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        ImGui::PushFont(f.mono, 12 * k());
        float lh = 20 * k(), h = (static_cast<float>(j - i + 1)) * lh + 24 * k();
        d->AddRectFilled(p, ImVec2(p.x + cw, p.y + h), col(t.panel), 6 * k());
        d->PushClipRect(p, ImVec2(p.x + cw - 8 * k(), p.y + h), true);
        for (size_t c = i; c <= j; ++c) {
          float y = p.y + 12 * k() + (c - i) * lh;
          d->AddText(ImVec2(p.x + 16 * k(), y + (lh - ImGui::GetTextLineHeight()) / 2), col(t.fg), plain(bs[c].spans).c_str());
          auto ci = at.find(bs[c].src);
          if (ci != at.end()) gutter_mark(&ci->second, y, lh);
        }
        d->PopClipRect();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(cw, h));
        gap(compact ? 6 : 14);
        i = j;
        break;
      }
      case md::Block::Rule: {
        gap(8);
        ImVec2 p = line();
        d->AddRectFilled(ImVec2(p.x + cx - x0, p.y), ImVec2(p.x + cx - x0 + cw, p.y + 3 * k()), col(t.border_muted));
        gap(14);
        break;
      }
      case md::Block::Badge: {
        bool titled = i + 1 < bs.size() && bs[i + 1].kind == md::Block::Table && bs[i + 1].test == b.src;
        if (titled) break;  // the table draws it as its title
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        std::vector<md::Span> sp = b.spans;
        bool import = !sp.empty() && sp[0].text.starts_with("import ");
        for (auto& s : sp) s.text = strip_emoji(s.text);
        float h = 0;
        if (import) {
          ImGui::PushFont(f.mono, 12 * k());
          text(plain(sp), t.muted);
          h = ImGui::GetTextLineHeight();
          ImGui::PopFont();
        } else {
          h = rich(sp, cw, 13, t.fg);
          ImGui::Dummy(ImVec2(cw, h));
        }
        gutter_mark(mks, p.y, h);
        if (mks)  // a directive with no table under it: its result after the badge
          for (auto& m : *mks) {
            ImGui::SetCursorPosX(cx);
            text_wrapped((m.glyph.empty() ? "" : m.glyph + " ") + m.text, level_fg(t, m), f.mono, 12, cw);
          }
        gap(import ? 2 : 6);
        break;
      }
      case md::Block::Table: {
        bool test = b.test > 0;
        const md::Block* badge = i > 0 && bs[i - 1].kind == md::Block::Badge && bs[i - 1].src == b.test && test ? &bs[i - 1] : nullptr;
        auto dir = at.find(b.test);
        const std::vector<Mark>* dmarks = test && dir != at.end() ? &dir->second : nullptr;
        auto lk = line_key();
        bool focused = test && focus.count(path) && focus[path] == b.test;
        if (jump_here && (b.test >= jump_src || b.rows[0].src >= jump_src)) {
          ImGui::SetScrollHereY(0.12f);
          jump_src = -1;
        }
        // the title: ▶ (run the table), its name, its fixture
        if (badge) {
          ImGui::SetCursorPosX(cx);
          ImVec2 p = line();
          float bh = 24 * k();
          ImGui::PushID(b.test);
          if (marks && lk) {
            if (ImGui::InvisibleButton("##runtable", ImVec2(bh, bh))) {
              focus[path] = b.test;
              run_line(path, b.test);
            }
            bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetTooltip("Run this table (r)");
            d->AddRectFilled(p, p + ImVec2(bh, bh), col(hov ? (t.dark ? t.border_muted : t.inset) : t.btn), 6 * k());
            d->AddRect(p, p + ImVec2(bh, bh), col(focused ? t.accent : t.border), 6 * k(), focused ? 2.f : 1.f);
            icon_play(d, ImVec2(p.x + 7 * k(), p.y + 7 * k()), 10 * k(), col(t.success));
            ImGui::SameLine(0, 8 * k());
          }
          ImGui::PopID();
          std::string fixture = strip_emoji(badge->spans.empty() ? "" : badge->spans[0].text);
          std::string name = table_name(bs, i);
          ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (bh - ImGui::GetTextLineHeight()) / 2);
          text(name, focused ? t.accent : t.fg, f.semibold);
          ImGui::SameLine(0, 8 * k());
          std::vector<std::pair<std::string, uint32_t>> fx = {{name == fixture ? "" : fixture, t.muted}};
          for (size_t s = 1; s < badge->spans.size(); ++s)
            fx.push_back({badge->spans[s].text, badge->spans[s].style & md::Warn ? t.attention : t.muted});
          ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1 * k());
          segments(fx, f.mono, 12);
          gutter_mark(nullptr, p.y, bh);
          gap(6);
        }
        // column widths: natural, then shrunk to fit (comments first)
        size_t nc = 0;
        for (auto& r : b.rows) nc = std::max(nc, r.cells.size());
        if (!nc) break;
        float tsz = compact ? 12 : 14, pad = 13 * k();
        std::vector<float> w(nc, 30 * k());
        std::vector<bool> comment(nc, false);
        for (auto& r : b.rows)
          for (size_t c = 0; c < r.cells.size(); ++c) {
            w[c] = std::max(w[c], rich_width(r.cells[c], tsz) + pad * 2);
            if (r.head && !r.cells[c].empty() && (r.cells[c][0].style & md::Dim)) comment[c] = true;
          }
        float avail = cw, total = 0;
        for (float x : w) total += x;
        for (size_t c = 0; c < nc && total > avail; ++c)
          if (comment[c] && w[c] > 80 * k()) {
            float give = std::min(total - avail, w[c] - 80 * k());
            w[c] -= give;
            total -= give;
          }
        while (total > avail) {
          auto widest = std::max_element(w.begin(), w.end());
          if (*widest <= 48 * k()) break;
          float give = std::min(total - avail, *widest - 48 * k());
          *widest -= std::max(1.f, std::min(give, 8 * k()));
          total = 0;
          for (float x : w) total += x;
        }
        ImGui::SetCursorPosX(cx);
        ImVec2 o = line();
        float y = o.y, lh = std::round(tsz * 1.6f * k());
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool win_hov = ImGui::IsWindowHovered();
        for (size_t ri = 0; ri < b.rows.size(); ++ri) {
          const auto& r = b.rows[ri];
          auto rm = at.find(r.src);
          const std::vector<Mark>* rmarks = !r.head && rm != at.end() && r.src != b.test ? &rm->second : nullptr;
          const Mark* wm = rmarks ? worst(*rmarks) : nullptr;
          Result res = wm ? result_of(*wm) : Result::Partial;
          bool muted_row = wm && (res == Result::Skipped || res == Result::Queued);
          float rh = 0;
          for (size_t c = 0; c < nc; ++c) {
            ImGui::SetCursorScreenPos(ImVec2(o.x, y));
            std::vector<md::Span> cell = c < r.cells.size() ? r.cells[c] : std::vector<md::Span>{};
            rh = std::max(rh, rich(cell, w[c] - pad * 2, tsz, t.fg, true));
          }
          rh = std::max(rh, lh) + 12 * k();
          if (jump_src >= 0 && jump_path == path && r.src >= jump_src && !compact) {
            ImGui::SetCursorScreenPos(ImVec2(o.x, y));
            ImGui::SetScrollHereY(0.3f);
            jump_src = -1;
          }
          // the row: tint, cells, borders
          if (r.head) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + rh), col(t.panel));
          else if (uint32_t tn = wm ? tint_of(t, res) : 0) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + rh), col(tn));
          float x = o.x;
          for (size_t c = 0; c < nc; ++c) {
            std::vector<md::Span> cell = c < r.cells.size() ? r.cells[c] : std::vector<md::Span>{};
            if (r.head) for (auto& s : cell) s.style |= md::Bold;
            float cwid = rich_width(cell, tsz);
            int al = c < b.align.size() ? b.align[c] : 0;
            float off = al == 2 ? std::max(0.f, w[c] - pad * 2 - cwid) : al == 1 ? std::max(0.f, (w[c] - pad * 2 - cwid) / 2) : 0;
            ImGui::SetCursorScreenPos(ImVec2(x + pad + off, y + 6 * k()));
            rich(cell, w[c] - pad * 2, tsz, muted_row ? t.muted : t.fg);
            d->AddRect(ImVec2(x, y), ImVec2(x + w[c] + 1, y + rh + 1), col(t.border));
            x += w[c];
          }
          // the gutter: the result's glyph; ▶ under the mouse on a test row
          bool row_hov = win_hov && mouse.y >= y && mouse.y < y + rh && mouse.x >= o.x - gutter && mouse.x < o.x + total;
          if (marks && test && lk && !r.head && row_hov) {
            ImGui::SetCursorScreenPos(ImVec2(o.x - gutter + 4 * k(), y + (rh - 22 * k()) / 2));
            ImGui::PushID(r.src);
            if (ImGui::InvisibleButton("##runrow", ImVec2(22 * k(), 22 * k()))) run_line(path, r.src);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run this row");
            ImGui::PopID();
            icon_play(d, ImVec2(o.x - gutter + 9 * k(), y + (rh - 12 * k()) / 2), 12 * k(), col(t.success));
          } else if (rmarks) {
            gutter_mark(rmarks, y + 3 * k(), lh);
          }
          y += rh;
          // an error's or warning's text under its row, across the table
          if (rmarks)
            for (auto& m : *rmarks) {
              if (m.text.empty() || (m.level != "error" && m.level != "warn")) continue;
              size_t at_ = 0;
              while (at_ <= m.text.size()) {
                size_t nl = m.text.find('\n', at_);
                std::string ln = m.text.substr(at_, nl == std::string::npos ? std::string::npos : nl - at_);
                ImGui::PushFont(f.mono, 12 * k());
                ImVec2 ts = ImGui::CalcTextSize(ln.c_str(), nullptr, false, total - pad * 2);
                float nh = ts.y + 10 * k();
                if (uint32_t tn = tint_of(t, res)) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + nh), col(tn));
                d->AddText(f.mono, 12 * k(), ImVec2(o.x + pad, y + 4 * k()), col(level_fg(t, m)), ln.c_str(), nullptr, total - pad * 2);
                d->AddLine(ImVec2(o.x, y), ImVec2(o.x, y + nh), col(t.border));
                d->AddLine(ImVec2(o.x + total, y), ImVec2(o.x + total, y + nh), col(t.border));
                d->AddLine(ImVec2(o.x, y + nh), ImVec2(o.x + total + 1, y + nh), col(t.border));
                ImGui::PopFont();
                y += nh;
                if (nl == std::string::npos) break;
                at_ = nl + 1;
              }
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x, y));
        ImGui::Dummy(ImVec2(total, 1));
        // the table's result, a pill under it; then the failures that are the table's
        if (dmarks) {
          gap(6);
          for (auto& m : *dmarks) {
            ImGui::SetCursorPosX(cx);
            Result r = result_of(m);
            Tone tn = r == Result::Passed ? Tone::Success : r == Result::Failed || r == Result::Error ? Tone::Danger
                      : r == Result::Snoozed ? Tone::Attention : r == Result::Running ? Tone::Accent : Tone::Neutral;
            std::string g = m.glyph.empty() ? result_glyph(r) : m.glyph;
            pill(first_line(m.text).empty() ? short_result(m.text) : first_line(m.text), tn, 22, g.c_str(), false);
            std::string rest = after_first_line(m.text);
            if (!rest.empty()) {
              ImGui::SetCursorPosX(cx);
              text_wrapped(rest, t.danger, f.mono, 12, cw);
            }
          }
        }
        gap(compact ? 8 : 16);
        break;
      }
    }
  }
  if (jump_path == path && jump_src >= 0 && !compact) jump_src = -1;  // past the end
}

// -- the reader pane ------------------------------------------------------------------

void Gui::banner(const Mark& m, const std::string& path, bool spec) {
  Result r = result_of(m);
  bool broken = m.banner;
  uint32_t fg = broken || m.level == "error" ? t.danger : m.level == "ok" ? t.success : m.level == "warn" ? t.attention
                : r == Result::Running ? t.accent : t.muted;
  uint32_t bg = fg == t.danger ? t.danger_muted : fg == t.success ? t.success_muted : fg == t.attention ? t.attention_muted
                : fg == t.accent ? t.accent_muted : t.neutral_muted;
  std::string glyph = m.glyph.empty() ? "!" : m.glyph, title = first_line(m.text), body = after_first_line(m.text);
  bool mono = false;
  if (broken) {  // "temoc failed: <message>": the message in monospace
    auto colon = title.find(": ");
    if (colon != std::string::npos) {
      body = title.substr(colon + 2) + (body.empty() ? "" : "\n" + body);
      title = title.substr(0, colon);
    }
    mono = true;
  }
  if (!broken) title = capital(title);  // "temoc failed" keeps the plugin's name as it is
  std::string action;
  auto lk = line_key();
  if (lk && r != Result::Running) action = path.empty() ? "Run all specs" : spec ? "Run file" : "";
  ImDrawList* d = ImGui::GetWindowDrawList();
  float W = std::min(ImGui::GetContentRegionAvail().x, 880 * k() - 64 * k());
  ImVec2 p = ImGui::GetCursorScreenPos();
  d->ChannelsSplit(2);
  d->ChannelsSetCurrent(1);
  float aw = 0;
  if (!action.empty()) {
    ImGui::PushFont(f.medium, 12 * k());
    aw = ImGui::CalcTextSize(action.c_str()).x + 40 * k();
    ImGui::PopFont();
  }
  ImGui::SetCursorScreenPos(ImVec2(p.x + 50 * k(), p.y + 12 * k()));
  ImGui::BeginGroup();
  text_wrapped(title, fg, f.semibold, 0, W - 66 * k() - aw);
  if (!body.empty()) text_wrapped(body, t.fg, mono ? f.mono : f.regular, mono ? 12 : 13, W - 66 * k() - aw);
  ImGui::EndGroup();
  float h = std::max(ImGui::GetItemRectMax().y - p.y + 12 * k(), 46 * k());
  if (!action.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - aw - 12 * k(), p.y + 9 * k()));
    ImGui::PushFont(nullptr, 12 * k());
    if (button(action + "##banner" + std::to_string(reinterpret_cast<uintptr_t>(&m)), Btn::Default, 28, lk->action.key))
      plugin_action(lk->action.key, reader_url, spec ? path : "");
    ImGui::PopFont();
  }
  d->ChannelsSetCurrent(0);
  d->AddRectFilled(p, p + ImVec2(W, h), col(bg), 6 * k());
  d->AddRect(p, p + ImVec2(W, h), col(fg), 6 * k());
  float gc = 11 * k();
  ImVec2 gcen(p.x + 16 * k() + gc, p.y + 12 * k() + gc);
  d->AddCircleFilled(gcen, gc, col(fg));
  ImGui::PushFont(f.mono, 13 * k());
  ImVec2 gs = ImGui::CalcTextSize(glyph.c_str());
  d->AddText(ImVec2(gcen.x - gs.x / 2, gcen.y - gs.y / 2), col(t.bg), glyph.c_str());
  ImGui::PopFont();
  d->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(W, 12 * k()));
}

void Gui::reader_header(const Pr& p) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  float W = ImGui::GetContentRegionAvail().x;
  if (button("Pull requests", Btn::Default, 28, "", 0, icon_back)) screen = Screen::List;
  ImGui::SameLine(0, 12 * k());
  float gw = 0;
  ImGui::PushFont(f.medium, 0.f);
  gw = ImGui::CalcTextSize("GitHub").x + 50 * k();
  ImGui::PopFont();
  float tx = ImGui::GetCursorPosX(), tw = W - tx - gw - 12 * k() + ImGui::GetStyle().WindowPadding.x;
  ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::PushFont(f.semibold, 18 * k());
  std::string title = shown_title(p.title);
  float th = ImGui::GetTextLineHeight();
  std::string num = "  " + short_repo(p.repo) + "#" + std::to_string(p.number);
  ImGui::PushFont(f.regular, 18 * k());
  float nw = ImGui::CalcTextSize(num.c_str()).x;
  ImGui::PopFont();
  float title_w = std::min(ImGui::CalcTextSize(title.c_str()).x, tw - nw);
  ImGui::RenderTextEllipsis(d, ImVec2(tp.x, tp.y + (28 * k() - th) / 2), ImVec2(tp.x + title_w, tp.y + 28 * k()),
                            tp.x + title_w, title.c_str(), nullptr, nullptr);
  ImGui::PopFont();
  d->AddText(f.regular, 18 * k(), ImVec2(tp.x + title_w, tp.y + (28 * k() - th) / 2), col(t.muted), num.c_str());
  ImGui::SetCursorScreenPos(ImVec2(tp.x + tw, tp.y));
  if (button("GitHub", Btn::Default, 28, "", 0, icon_external)) open(p.url);
  // the meta line
  ImGui::Dummy(ImVec2(1, 2 * k()));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 4 * k()));
  Pill pl = state_pill(derive_state(p, status_of(p.url)));
  pill(strip_emoji(pl.text), pl.tone);
  float y = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y + ImGui::GetScrollY();
  auto mid = [&](float h) { ImGui::SetCursorPosY(y + (22 * k() - h) / 2); };
  ImGui::SameLine();
  mid(20 * k());
  avatar(p.author);
  ImGui::SameLine(0, 6 * k());
  float lh = ImGui::GetTextLineHeight();
  mid(lh);
  text(who(p.author), t.fg, f.semibold);
  auto dotsep = [&] {
    ImGui::SameLine();
    mid(lh);
    text("·", t.muted);
    ImGui::SameLine();
  };
  ImGui::SameLine();
  mid(lh);
  text("opened " + age(now() - p.created) + " ago", t.muted);
  dotsep();
  ImGui::PushFont(f.mono, 12 * k());
  mid(ImGui::GetTextLineHeight());
  text("+" + std::to_string(p.additions), t.success);
  ImGui::SameLine(0, 4 * k());
  mid(ImGui::GetTextLineHeight());
  text("−" + std::to_string(p.deletions), t.danger);
  ImGui::PopFont();
  ImGui::SameLine();
  mid(lh);
  text("in " + std::to_string(p.changed_files) + " files", t.muted);
  if (!p.reviews.empty()) {
    std::string names;
    for (auto& [login, st] : p.reviews) names += (names.empty() ? "" : ", ") + (login == viewer ? std::string("you") : who(login));
    dotsep();
    mid(lh);
    text("reviewers " + names, t.muted);
  }
  for (size_t i = 0; i < cfg.plugins.size(); ++i) {
    std::string problem = plugin_problem(i);
    auto it = live[i].prs.find(p.url);
    std::string s;
    uint32_t c = t.muted;
    if (!problem.empty()) {
      s = cfg.plugins[i].name + " error";
      c = t.danger;
    } else if (it != live[i].prs.end() && it->second.marks_summary) {
      Result r = result_of(*it->second.marks_summary);
      s = cfg.plugins[i].name + " " + (it->second.marks_summary->banner ? "failed" : r == Result::Running ? "running"
                                       : short_result(it->second.marks_summary->text));
      if (it->second.marks_summary->banner) r = Result::Failed;
      c = r == Result::Passed ? t.success : r == Result::Failed || r == Result::Error ? t.danger : r == Result::Running ? t.accent : t.muted;
    } else if (it != live[i].prs.end()) {
      for (auto& tg : it->second.tags)
        if (std::string x = strip_emoji(tg.glyph + " " + tg.text); !x.empty() && !tag_person(tg)) s += (s.empty() ? "" : ", ") + x;
    }
    if (s.empty()) continue;
    dotsep();
    mid(lh);
    text(s, c, c == t.muted ? nullptr : f.medium);
  }
  ImGui::PopStyleVar();
}

void Gui::reader_tabs(const std::vector<Doc>& ds) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float H = 36 * k(), W = ImGui::GetContentRegionAvail().x;
  d->AddLine(ImVec2(o.x - 16 * k(), o.y + H - 1), ImVec2(o.x + W + 16 * k(), o.y + H - 1), col(t.border));
  ImGui::BeginChild("##tabs", ImVec2(W, H), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* cd = ImGui::GetWindowDrawList();
  std::string head = bodies[reader_url].head;
  for (size_t i = 0; i < ds.size(); ++i) {
    const Doc& doc = ds[i];
    // its icon and counts: the file's state (line 0) and its rows by glyph
    std::string icon, count;
    uint32_t icol = 0;
    std::vector<Mark> ms = doc.path.empty() ? std::vector<Mark>{} : marks_on(reader_url, doc.path, head);
    if (doc.path.empty()) {
      for (auto& st : live)
        if (auto it = st.prs.find(reader_url); it != st.prs.end() && it->second.marks_summary) {
          const Mark& m = *it->second.marks_summary;
          icon = m.glyph;
          icol = level_fg(t, m);
        }
    } else {
      const Mark* w0 = nullptr;
      bool banner_ = false;
      for (auto& m : ms)
        if (m.line == 0) {
          if (m.banner) banner_ = true;
          else if (!w0 || severity(m) > severity(*w0)) w0 = &m;
        }
      if (banner_) icon = "!", icol = t.danger;
      else if (w0) icon = w0->glyph, icol = level_fg(t, *w0);
      if (w0 && result_of(*w0) == Result::Running) {
        std::string tx = first_line(w0->text), un = unclocked(tx);
        count = un != tx ? tx.substr(un.size() + std::string(" · ").size()) : "";
      } else if (const Files* fs = reader_files()) {
        for (auto& ft : fs->tabs)
          if (ft.path == doc.path)
            for (auto& c : md::tally(ms, ft.dirs)) count += (count.empty() ? "" : "  ") + c.glyph + " " + std::to_string(c.n);
      }
    }
    bool on = i == reader_tab;
    ImGui::PushFont(on ? f.semibold : f.regular, 0.f);
    ImVec2 ns = ImGui::CalcTextSize(doc.name.c_str());
    ImGui::PopFont();
    float cw = 0;
    if (!count.empty()) {
      ImGui::PushFont(f.regular, 12 * k());
      cw = ImGui::CalcTextSize(count.c_str()).x + 8 * k();
      ImGui::PopFont();
    }
    float iw = icon.empty() ? 0 : 16 * k() + 8 * k();
    float w = 24 * k() + iw + ns.x + cw;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##tab", ImVec2(w, H))) reader_tab = i;
    bool hov = ImGui::IsItemHovered();
    if (hov && !doc.path.empty()) ImGui::SetTooltip("%s", doc.path.c_str());
    ImGui::PopID();
    float x = p.x + 12 * k();
    if (!icon.empty()) {
      ImVec2 c(x + 8 * k(), p.y + H / 2);
      cd->AddCircleFilled(c, 8 * k(), col(icol));
      ImGui::PushFont(f.mono, 11 * k());
      ImVec2 gs = ImGui::CalcTextSize(icon.c_str());
      cd->AddText(ImVec2(c.x - gs.x / 2, c.y - gs.y / 2), col(t.bg), icon.c_str());
      ImGui::PopFont();
      x += iw;
    }
    cd->AddText(on ? f.semibold : f.regular, ImGui::GetFontSize(), ImVec2(x, p.y + (H - ns.y) / 2), col(on || hov ? t.fg : t.muted),
                doc.name.c_str());
    if (!count.empty())
      cd->AddText(f.regular, 12 * k(), ImVec2(x + ns.x + 8 * k(), p.y + (H - 12 * k() * 1.2f) / 2), col(t.muted), count.c_str());
    if (on) cd->AddRectFilled(ImVec2(p.x, p.y + H - 2 * k()), ImVec2(p.x + w, p.y + H), col(t.tab_line));
    ImGui::SameLine(0, 2 * k());
  }
  if (const Files* fs = reader_files(); !fs || fs->loading) {
    ImGui::SetCursorPosY((H - ImGui::GetTextLineHeight()) / 2);
    text("loading files…", t.muted, nullptr, 12);
  } else if (!fs->error.empty()) {
    ImGui::SetCursorPosY((H - ImGui::GetTextLineHeight()) / 2);
    text(fs->error, t.danger, nullptr, 12);
  }
  ImGui::EndChild();
}

void Gui::reader_pane() {
  auto c = cache.find(reader_url);
  if (c == cache.end()) return;
  const Pr& p = c->second;
  want_body(reader_url, p.updated);
  want_files(reader_url);
  auto ds = docs();
  if (reader_tab >= ds.size()) reader_tab = 0;
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_AutoHideTabBar;
  ImGui::SetNextWindowClass(&wc);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * k(), 12 * k()));
  bool shown = ImGui::Begin("Reader###reader", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                          ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  reader_header(p);
  ImGui::Dummy(ImVec2(1, 4 * k()));
  ImGui::SetCursorPosX(16 * k());
  reader_tabs(ds);
  const Doc& doc = ds[reader_tab];
  ImGui::SetCursorPosX(0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32 * k(), 20 * k()));
  ImGui::BeginChild(("##doc" + doc.path).c_str(), ImVec2(ImGui::GetWindowWidth(), 0), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  if (scroll_by != 0) {  // the reader's keys: a step, a page, the ends
    float page = ImGui::GetWindowHeight() * 0.9f, by = scroll_by;
    if (std::abs(by) >= 1e6f && std::abs(by) < 1e9f) by = (by / 1e6f) * page;
    ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() + by));  // ImGui stops it at the end
    scroll_by = 0;
  }
  std::vector<std::string> stale;
  std::vector<Mark> ms;
  if (!doc.path.empty()) ms = marks_on(reader_url, doc.path, bodies[reader_url].head, &stale);
  for (auto& s : stale) banner({doc.path, 0, "warn", "~", s}, doc.path, doc.spec);
  for (auto& m : reader_top(doc.path, doc.spec, ms)) banner(m, doc.path, doc.spec);
  // the file's state; a banner (temoc couldn't run it) says it already
  bool banners = std::any_of(ms.begin(), ms.end(), [](const Mark& m) { return m.line == 0 && m.banner; });
  for (auto& m : ms)
    if (m.line == 0 && (m.banner || !banners)) banner(m, doc.path, doc.spec);
  if (doc.loading && doc.text->empty()) text("Loading…", t.muted);
  else if (!doc.error.empty() && doc.text->empty()) text_wrapped(doc.error, t.danger);
  else if (doc.text->find_first_not_of(" \r\n") == std::string::npos) text(doc.path.empty() ? "No description." : "(empty file)", t.muted);
  else markdown(*doc.text, doc.path, doc.spec, true);
  ImGui::Dummy(ImVec2(1, 40 * k()));
  ImGui::EndChild();
  ImGui::End();
}

// -- the right panes ------------------------------------------------------------------------

void Gui::results_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 16 * k()));
  bool shown = ImGui::Begin("Results###results", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto ds = docs();
  if (reader_tab >= ds.size()) return ImGui::End();
  const Doc& doc = ds[reader_tab];
  std::string head = bodies[reader_url].head;
  auto lk = line_key();
  ImDrawList* d = ImGui::GetWindowDrawList();
  auto tone_of = [&](const Mark& m) {
    Result r = result_of(m);
    return m.banner ? Tone::Danger : r == Result::Passed ? Tone::Success : r == Result::Failed || r == Result::Error ? Tone::Danger
           : r == Result::Snoozed ? Tone::Attention : r == Result::Running ? Tone::Accent : Tone::Neutral;
  };
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k(), 6 * k()));
  if (doc.spec || doc.path.empty()) {
    std::vector<Mark> ms = doc.path.empty() ? std::vector<Mark>{} : marks_on(reader_url, doc.path, head);
    section(doc.path.empty() ? "This pull request" : "This file");
    const Mark* state = nullptr;
    std::vector<Mark> top = doc.path.empty() ? reader_top("", false, {}) : std::vector<Mark>{};
    for (auto& m : doc.path.empty() ? top : ms)
      if (m.line == 0 && (!state || severity(m) > severity(*state) || m.banner)) state = &m;
    if (state) pill(state->banner ? "temoc failed" : first_line(state->text), tone_of(*state), 24,
                    (state->glyph.empty() ? "!" : state->glyph).c_str(), false);
    else pill("not run", Tone::Neutral, 24, "·", false);
    ImGui::Dummy(ImVec2(1, 4 * k()));
    if (lk) {
      if (doc.spec) {
        if (button("Run file", Btn::Primary, 28, lk->action.key, 0, icon_play)) plugin_action(lk->action.key, reader_url, doc.path);
        ImGui::SameLine();
      }
      if (button("Run all specs", doc.spec ? Btn::Default : Btn::Primary, 28, doc.spec ? "" : lk->action.key))
        plugin_action(lk->action.key, reader_url);
    } else {
      text("No plugin runs specs (a \"line\" action).", t.muted, nullptr, 12);
    }
    ImGui::Dummy(ImVec2(1, 12 * k()));
    if (doc.spec) {
      // the tables: each one's result, a click focuses it and jumps there
      const auto& bs = blocks_of(*doc.text);
      auto at = attach(bs, ms);
      section("Tables");
      for (size_t i = 0; i < bs.size(); ++i) {
        if (bs[i].kind != md::Block::Table || !bs[i].test) continue;
        auto it = at.find(bs[i].test);
        const Mark* m = it != at.end() ? worst(it->second) : nullptr;
        bool focused = focus.count(doc.path) && focus[doc.path] == bs[i].test;
        ImGui::PushID(static_cast<int>(i));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 30 * k();
        if (ImGui::InvisibleButton("##table", ImVec2(w, h))) {
          focus[doc.path] = bs[i].test;
          jump(doc.path, bs[i].test);
        }
        if (focused) d->AddRectFilled(p, p + ImVec2(w, h), col(t.sel), 6 * k());
        else if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
        uint32_t c = m ? level_fg(t, *m) : t.muted;
        std::string g = m ? (m->glyph.empty() ? result_glyph(result_of(*m)) : m->glyph) : "·";
        d->AddText(f.mono, 13 * k(), ImVec2(p.x + 8 * k(), p.y + (h - 13 * k() * 1.2f) / 2), col(c), g.c_str());
        std::string word = m ? short_result(m->text) : "not run";
        ImGui::PushFont(nullptr, 12 * k());
        float ww = ImGui::CalcTextSize(word.c_str()).x;
        d->AddText(ImVec2(p.x + w - 8 * k() - ww, p.y + (h - ImGui::GetTextLineHeight()) / 2), col(c), word.c_str());
        ImGui::PopFont();
        std::string name = table_name(bs, i);
        float lh = ImGui::GetTextLineHeight();
        ImGui::RenderTextEllipsis(d, ImVec2(p.x + 32 * k(), p.y + (h - lh) / 2), ImVec2(p.x + w - 16 * k() - ww, p.y + h),
                                  p.x + w - 16 * k() - ww, name.c_str(), nullptr, nullptr);
        ImGui::PopID();
      }
      // what did not pass: each failing row, table and file, a click jumps there
      struct Bad {
        std::string where, text;
        uint32_t c;
        int line;
      };
      std::vector<Bad> bad;
      for (auto& m : ms) {
        if (m.level != "error" && m.level != "warn") continue;
        if (m.line == 0) {
          if (m.banner) bad.push_back({"The whole file", m.text.substr(m.text.find(": ") == std::string::npos ? 0 : m.text.find(": ") + 2), t.danger, 0});
          continue;
        }
        std::string where = "line " + std::to_string(m.line);
        bool is_table = false;
        for (size_t i = 0; i < bs.size(); ++i) {
          if (bs[i].kind != md::Block::Table || !bs[i].test) continue;
          if (bs[i].test == m.line) {
            where = table_name(bs, i) + " (the table)";
            is_table = true;
          }
          for (auto& r : bs[i].rows)
            if (r.src == m.line && !r.cells.empty()) where = table_name(bs, i) + ", " + plain(r.cells[0]);
        }
        std::string txt = is_table ? after_first_line(m.text) : m.text;
        if (txt.empty()) continue;
        bad.push_back({where, first_line(txt), m.level == "error" ? t.danger : t.attention, m.line});
      }
      std::stable_sort(bad.begin(), bad.end(), [](const Bad& a, const Bad& b) { return a.line < b.line; });
      if (!bad.empty()) {
        ImGui::Dummy(ImVec2(1, 12 * k()));
        section("What did not pass");
        for (size_t i = 0; i < bad.size(); ++i) {
          ImGui::PushID(static_cast<int>(i) + 1000);
          ImVec2 p = ImGui::GetCursorScreenPos();
          float w = ImGui::GetContentRegionAvail().x;
          ImGui::SetCursorScreenPos(p + ImVec2(10 * k(), 8 * k()));
          ImGui::BeginGroup();
          text_wrapped(bad[i].where, t.fg, f.medium, 0, w - 20 * k());
          text_wrapped(bad[i].text, bad[i].c, f.mono, 12, w - 20 * k());
          ImGui::EndGroup();
          float h = ImGui::GetItemRectMax().y - p.y + 8 * k();
          ImGui::SetCursorScreenPos(p);
          if (ImGui::InvisibleButton("##bad", ImVec2(w, h))) jump(doc.path, bad[i].line);
          bool hov = ImGui::IsItemHovered();
          d->AddRect(p, p + ImVec2(w, h), col(hov ? t.border : t.border_muted), 6 * k());
          ImGui::Dummy(ImVec2(1, 2 * k()));
          ImGui::PopID();
        }
      }
    } else {
      // the description: each spec file's result, a click opens it
      section("Spec files");
      bool any = false;
      for (size_t i = 1; i < ds.size(); ++i) {
        if (!ds[i].spec) continue;
        any = true;
        const Mark* w0 = nullptr;
        auto fm = marks_on(reader_url, ds[i].path, head);
        for (auto& m : fm)
          if (m.line == 0 && (!w0 || m.banner || severity(m) > severity(*w0))) w0 = &m;
        ImGui::PushID(static_cast<int>(i));
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = 30 * k();
        if (ImGui::InvisibleButton("##spec", ImVec2(w, h))) reader_tab = i;
        if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
        uint32_t c = w0 ? (w0->banner ? t.danger : level_fg(t, *w0)) : t.muted;
        std::string g = w0 ? (w0->banner ? "!" : w0->glyph) : "·";
        d->AddText(f.mono, 13 * k(), ImVec2(p.x + 8 * k(), p.y + (h - 13 * k() * 1.2f) / 2), col(c), g.c_str());
        std::string word = w0 ? (w0->banner ? "temoc failed" : short_result(w0->text)) : "not run";
        ImGui::PushFont(nullptr, 12 * k());
        float ww = ImGui::CalcTextSize(word.c_str()).x;
        d->AddText(ImVec2(p.x + w - 8 * k() - ww, p.y + (h - ImGui::GetTextLineHeight()) / 2), col(c), word.c_str());
        ImGui::PopFont();
        d->AddText(ImVec2(p.x + 32 * k(), p.y + (h - ImGui::GetTextLineHeight()) / 2), col(t.fg), ds[i].name.c_str());
        ImGui::PopID();
      }
      if (!any) text(reader_files() && !reader_files()->loading ? "This PR changes no spec file." : "Loading files…", t.muted);
    }
    // the command the results came from, and the commit
    for (size_t i = 0; i < live.size(); ++i) {
      auto it = live[i].prs.find(reader_url);
      if (it == live[i].prs.end() || it->second.marks_command.empty()) continue;
      ImGui::Dummy(ImVec2(1, 12 * k()));
      section("Last command");
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x;
      d->ChannelsSplit(2);
      d->ChannelsSetCurrent(1);
      ImGui::SetCursorScreenPos(p + ImVec2(10 * k(), 8 * k()));
      text_wrapped(it->second.marks_command, t.fg, f.mono, 12, w - 20 * k());
      float h = ImGui::GetItemRectMax().y - p.y + 8 * k();
      d->ChannelsSetCurrent(0);
      d->AddRectFilled(p, p + ImVec2(w, h), col(t.panel), 6 * k());
      d->AddRect(p, p + ImVec2(w, h), col(t.border_muted), 6 * k());
      d->ChannelsMerge();
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 4 * k()));
      const std::string& ref = it->second.marks_ref;
      bool moved = !ref.empty() && !head.empty() && !head.starts_with(ref);
      std::string at = ref.empty() ? "" : "on " + ref.substr(0, 7) + (moved ? "; the PR has moved on to " + head.substr(0, 7) : ", the PR's head");
      if (!at.empty()) text_wrapped(at, moved ? t.attention : t.muted, nullptr, 12);
    }
  } else {
    // any other file: a block per plugin with marks on it
    bool any = false;
    for (size_t i = 0; i < live.size(); ++i) {
      auto ms = marks_on(reader_url, doc.path, head, nullptr, static_cast<int>(i));
      if (ms.empty()) continue;
      any = true;
      section(cfg.plugins[i].name);
      const Files* fs = reader_files();
      std::string counts;
      if (fs)
        for (auto& ft : fs->tabs)
          if (ft.path == doc.path)
            for (auto& c : md::tally(ms, ft.dirs)) counts += c.glyph + " " + std::to_string(c.n) + "  ";
      text(counts.empty() ? std::to_string(ms.size()) + " marks" : counts, t.fg);
      ImGui::Dummy(ImVec2(1, 8 * k()));
    }
    if (!any) text_wrapped("No plugin says anything about this file.", t.muted);
  }
  // the PR's other plugin actions
  std::vector<PluginKey> others;
  for (auto& pk : plugin_keys())
    if (!pk.action.line) others.push_back(pk);
  if (!others.empty()) {
    ImGui::Dummy(ImVec2(1, 12 * k()));
    section("Actions");
    for (auto& [i, a] : others) {
      if (button(strip_emoji(a.button.empty() ? a.label : a.button) + "##act" + a.id, Btn::Default, 28, a.key))
        plugin_action(a.key, reader_url);
      ImGui::SameLine();
    }
    ImGui::NewLine();
  }
  ImGui::PopStyleVar();
  ImGui::End();
}

void Gui::outline_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 12 * k()));
  bool shown = ImGui::Begin("Outline###outline", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto ds = docs();
  if (reader_tab < ds.size() && ds[reader_tab].text) {
    const Doc& doc = ds[reader_tab];
    const auto& bs = blocks_of(*doc.text);
    std::vector<Mark> ms = doc.path.empty() ? std::vector<Mark>{} : marks_on(reader_url, doc.path, bodies[reader_url].head);
    auto at = attach(bs, ms);
    section("On this page");
    bool any = false;
    for (size_t i = 0; i < bs.size(); ++i) {
      const md::Block& b = bs[i];
      std::string label;
      float indent = 0;
      uint32_t c = t.fg;
      std::string g;
      if (b.kind == md::Block::Heading && b.heading <= 3) {
        label = plain(b.spans);
        indent = (b.heading - 1) * 14 * k();
      } else if (b.kind == md::Block::Table && b.test) {
        label = table_name(bs, i);
        indent = 28 * k();
        auto it = at.find(b.test);
        if (it != at.end())
          if (const Mark* m = worst(it->second)) g = m->glyph, c = level_fg(t, *m);
        if (g.empty()) g = "·", c = t.muted;
      } else {
        continue;
      }
      any = true;
      ImGui::PushID(static_cast<int>(i));
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x, h = 26 * k();
      if (ImGui::InvisibleButton("##ol", ImVec2(w, h))) jump(doc.path, b.kind == md::Block::Table ? b.test : b.src);
      if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
      float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
      if (!g.empty()) ImGui::GetWindowDrawList()->AddText(f.mono, 12 * k(), ImVec2(p.x + indent - 16 * k(), ty), col(c), g.c_str());
      ImGui::GetWindowDrawList()->AddText(b.kind == md::Block::Heading && b.heading == 1 ? f.semibold : f.regular, ImGui::GetFontSize(),
                                          ImVec2(p.x + 8 * k() + indent, ty), col(t.fg), label.c_str());
      ImGui::PopID();
    }
    if (!any) text("No headings.", t.muted);
  }
  ImGui::End();
}

void Gui::llm_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * k(), 14 * k()));
  if (focus_llm) {
    ImGui::SetNextWindowFocus();
    focus_llm = false;
  }
  bool shown = ImGui::Begin("LLM review###llm", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto notes = notes_of(reader_url);
  if (notes.empty()) text_wrapped("No plugin left a note on this PR. anton-llm's reviews land here.", t.muted);
  for (auto& [name, body] : notes) {
    if (notes.size() > 1) text(name, t.muted, f.semibold, 12);
    markdown(body, "", false, false, true);
    ImGui::Dummy(ImVec2(1, 12 * k()));
  }
  ImGui::End();
}

void Gui::runlog_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 6 * k()));
  bool shown = ImGui::Begin("Run log###runlog", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImGui::PushFont(f.mono, 12 * k());
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * k(), 1 * k()));
  bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
  size_t lines = 0;
  for (auto& e : runlog) {
    if (!e.url.empty() && e.url != reader_url) continue;
    ++lines;
    char tm[16];
    std::tm lt{};
    localtime_r(&e.at, &lt);
    std::strftime(tm, sizeof tm, "%H:%M:%S", &lt);
    uint32_t c = e.level == "ok" ? t.success : e.level == "error" ? t.danger : e.level == "warn" ? t.attention
                 : e.event == "running" || e.event == "action" ? t.accent : t.muted;
    text(tm, t.muted);
    ImGui::SameLine();
    float x = ImGui::GetCursorPosX();
    text(e.plugin + " " + e.event, c);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(x + 150 * k());
    text(strip_emoji(e.text), t.fg);
  }
  if (!lines) text("Nothing yet: a plugin's progress shows here as it runs (press T to run specs).", t.muted);
  if (at_bottom) ImGui::SetScrollHereY(1.0f);
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::End();
}

void Gui::problems_pane() {
  struct Problem {
    std::string path, text;
    int line;
    uint32_t c;
  };
  std::vector<Problem> ps;
  for (size_t i = 0; i < live.size(); ++i) {
    if (std::string pr = plugin_problem(i); !pr.empty()) ps.push_back({"", pr, -1, t.danger});
    auto it = live[i].prs.find(reader_url);
    if (it == live[i].prs.end()) continue;
    for (auto& m : it->second.marks)
      if ((m.level == "error" || m.level == "warn") && !(m.line == 0 && !m.banner) && !m.text.empty())
        ps.push_back({m.file, m.text, m.line, m.level == "error" ? t.danger : t.attention});
  }
  std::string title = "Problems  " + std::to_string(ps.size()) + "###problems";
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 6 * k()));
  bool shown = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImGui::PushFont(f.mono, 12 * k());
  if (ps.empty()) text("No problems on this PR.", t.muted);
  for (size_t i = 0; i < ps.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    std::string where = ps[i].path.empty() ? "plugin" : path_tail(ps[i].path, 1) + (ps[i].line > 0 ? ":" + std::to_string(ps[i].line) : "");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetTextLineHeight() + 2 * k();
    if (ImGui::InvisibleButton("##pb", ImVec2(w, h)) && !ps[i].path.empty()) jump(ps[i].path, std::max(0, ps[i].line));
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 4 * k());
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 4 * k(), p.y + 1 * k()), col(ps[i].c), where.c_str());
    std::string txt = first_line(ps[i].text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 170 * k(), p.y + 1 * k()), col(t.fg), txt.c_str());
    ImGui::PopID();
  }
  ImGui::PopFont();
  ImGui::End();
}

}  // namespace anton::gui
