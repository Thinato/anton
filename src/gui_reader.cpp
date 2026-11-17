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
  if (parsed.size() > 256) parsed.clear();  // notes change as runs go: don't keep every version
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
    float sz = (wd.style & md::Code ? size * code() / reading() : size) * k();
    w += fnt->CalcTextSizeA(sz, FLT_MAX, 0, wd.s.c_str()).x + (wd.style & md::Code && !wd.space ? 8 * k() : 0);
  }
  return std::max(best, w);
}

float Gui::rich(const std::vector<md::Span>& spans, float width, float size, uint32_t color, bool measure, float line) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float lh = std::round((line > 0 ? line : size * 20 / 13) * k()), x = 0, y = 0;
  auto ws = words(spans);
  for (size_t i = 0; i < ws.size(); ++i) {
    const Word& wd = ws[i];
    if (wd.brk) {
      x = 0;
      y += lh;
      continue;
    }
    bool mono = wd.style & md::Code;
    ImFont* fnt = wd.style & md::Bold ? f.semibold : mono ? f.mono : f.regular;
    float sz = (mono ? size * code() / reading() : size) * k();
    ImVec2 ts = fnt->CalcTextSizeA(sz, FLT_MAX, 0, wd.s.c_str());
    float pad = mono && !wd.space ? 8 * k() : 0;
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
      if (mono && !wd.space)
        d->AddRectFilled(ImVec2(o.x + x, ty - 1 * k()), ImVec2(o.x + x + ts.x + pad, ty + ts.y + 1 * k()), col(t.panel), 4 * k());
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
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0));  // the gaps below are all of it
  float gutter = marks ? 16 * k() : 0;
  float x0 = ImGui::GetCursorPosX();
  float W = std::min(ImGui::GetContentRegionAvail().x, (compact ? 10000 : 880) * k());
  float body = reading(), rk = reading() / 13;  // headings scale with the reading size
  auto line = [&]() { return ImGui::GetCursorScreenPos(); };
  auto gap = [&](float px) { ImGui::Dummy(ImVec2(1, px * k())); };
  auto gutter_mark = [&](const std::vector<Mark>* mks, float y, float h) {
    if (!mks || mks->empty()) return;
    const Mark* w = worst(*mks);
    std::string g = w->glyph.empty() ? "•" : w->glyph;
    float gfs = code() * k();
    ImVec2 gs = f.mono->CalcTextSizeA(gfs, FLT_MAX, 0, g.c_str());
    ImVec2 o = ImGui::GetWindowPos();
    float gx = o.x + x0 - ImGui::GetScrollX() + (gutter - gs.x) / 2 - 2 * k();
    d->AddText(f.mono, gfs, ImVec2(gx, y + (std::min(h, 20 * k()) - gs.y) / 2), col(level_fg(t, *w)), g.c_str());
  };
  auto under_notes = [&](const std::vector<Mark>* mks) {  // ✗ line 13 · text, under a line
    if (!mks) return;
    for (auto& m : *mks) {
      if (m.text.empty() || (m.level != "error" && m.level != "warn")) continue;
      ImGui::SetCursorPosX(x0 + gutter);
      text_wrapped((m.glyph.empty() ? "•" : m.glyph) + " line " + std::to_string(m.line) + " · " + m.text,
                   m.level == "error" ? t.danger : t.attention, f.mono, code(), W - gutter);
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
        // the type scale: Reader H1 18, H2 15, the rest as Title 14; in Preview all 13
        static const float big[] = {18, 15, 14, 14, 14, 14};
        // not (compact ? a : b)[i]: GCC 11 read past that, crashing on Linux
        int level = b.heading < 1 ? 1 : b.heading > 6 ? 6 : b.heading;
        float sz = compact ? body : big[level - 1] * rk;
        if (i > 0) gap(compact ? 6 : 12);
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        std::vector<md::Span> sp = b.spans;
        for (auto& s : sp) s.style |= md::Bold;
        float h = rich(sp, cw, sz, t.fg, false, sz * (level == 1 ? 24.f / 18 : 20.f / sz));
        gutter_mark(mks, p.y, h);
        ImGui::Dummy(ImVec2(cw, h));
        if (b.heading == 1 && !compact) {  // a rule under H1
          gap(6);
          ImVec2 q = line();
          d->AddLine(ImVec2(q.x + cx - x0, q.y), ImVec2(q.x + cx - x0 + cw, q.y), col(t.border_muted));
          gap(1);
        }
        gap(compact ? 4 : 8);
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
          float mx = p.x - 18 * k(), my = p.y + std::round(body * 20 / 13 * k()) / 2;
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
            d->AddText(ImVec2(p.x - 6 * k() - mw, p.y + (std::round(body * 20 / 13 * k()) - ImGui::GetTextLineHeight()) / 2), col(t.fg2), m.c_str());
            ImGui::PopFont();
          }
        }
        float h = rich(b.spans, cw, body, b.quote ? t.muted : t.fg2);
        if (b.quote) d->AddRectFilled(ImVec2(p.x - 16 * k(), p.y), ImVec2(p.x - 12 * k(), p.y + h), col(t.border));
        gutter_mark(mks, p.y, h);
        ImGui::Dummy(ImVec2(cw, h));
        bool next_item = i + 1 < bs.size() && bs[i + 1].kind == md::Block::Para && bs[i + 1].indent > 0 && b.indent > 0;
        gap(next_item ? 4 : 10);
        under_notes(mks);
        break;
      }
      case md::Block::Code: {
        size_t j = i;
        while (j + 1 < bs.size() && bs[j + 1].kind == md::Block::Code && bs[j + 1].indent == b.indent) ++j;
        ImGui::SetCursorPosX(cx);
        ImVec2 p = line();
        ImGui::PushFont(f.mono, code() * k());
        float lh = 16 * k(), h = (static_cast<float>(j - i + 1)) * lh + 16 * k();
        d->AddRectFilled(p, ImVec2(p.x + cw, p.y + h), col(t.panel), 6 * k());
        d->PushClipRect(p, ImVec2(p.x + cw - 8 * k(), p.y + h), true);
        for (size_t c = i; c <= j; ++c) {
          float y = p.y + 8 * k() + (c - i) * lh;
          d->AddText(ImVec2(p.x + 10 * k(), y + (lh - ImGui::GetTextLineHeight()) / 2), col(t.fg2), plain(bs[c].spans).c_str());
          auto ci = at.find(bs[c].src);
          if (ci != at.end()) gutter_mark(&ci->second, y, lh);
        }
        d->PopClipRect();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(cw, h));
        gap(8);
        i = j;
        break;
      }
      case md::Block::Rule: {
        gap(6);
        ImVec2 p = line();
        d->AddRectFilled(ImVec2(p.x + cx - x0, p.y), ImVec2(p.x + cx - x0 + cw, p.y + 2 * k()), col(t.border_muted));
        gap(10);
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
          text(plain(sp), t.muted, f.mono, code());
          h = code() * k();
        } else {
          h = rich(sp, cw, 12, t.fg);
          ImGui::Dummy(ImVec2(cw, h));
        }
        gutter_mark(mks, p.y, h);
        if (mks)  // a directive with no table under it: its result after the badge
          for (auto& m : *mks) {
            ImGui::SetCursorPosX(cx);
            text_wrapped((m.glyph.empty() ? "" : m.glyph + " ") + m.text, level_fg(t, m), f.mono, code(), cw);
          }
        gap(import ? 4 : 6);
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
          float bh = 18 * k();
          ImGui::PushID(b.test);
          if (marks && lk) {
            if (ImGui::InvisibleButton("##runtable", ImVec2(bh, bh))) {
              focus[path] = b.test;
              run_line(path, b.test);
            }
            bool hov = ImGui::IsItemHovered();
            if (hov) tip("Run this table (r)");
            d->AddRectFilled(p, p + ImVec2(bh, bh), col(hov ? (t.dark ? t.border_muted : t.inset) : t.btn), 4 * k());
            d->AddRect(p, p + ImVec2(bh, bh), col(focused ? t.accent : t.border), 4 * k(), focused ? 2.f : 1.f);
            icon_play(d, ImVec2(p.x + 5 * k(), p.y + 5 * k()), 8 * k(), col(t.success));
            ImGui::SameLine(0, 6 * k());
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
          segments(fx, f.mono, code());
          gutter_mark(nullptr, p.y, bh);
          gap(8);
        }
        // column widths: natural, then shrunk to fit (comments first)
        size_t nc = 0;
        for (auto& r : b.rows) nc = std::max(nc, r.cells.size());
        if (!nc) break;
        float tsz = 12, pad = 8 * k(), tline = 16;  // Interface text on 24px rows
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
        float y = o.y, lh = std::round(tline * k());
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
            rh = std::max(rh, rich(cell, w[c] - pad * 2, r.head ? 11 : tsz, t.fg, true, tline));
          }
          rh = std::max(rh, lh) + 8 * k();
          if (jump_src >= 0 && jump_path == path && r.src >= jump_src && !compact) {
            ImGui::SetCursorScreenPos(ImVec2(o.x, y));
            ImGui::SetScrollHereY(0.3f);
            jump_src = -1;
          }
          // the row: tint, cells, the rule above it
          if (r.head) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + rh), col(t.panel), 6 * k(), ImDrawFlags_RoundCornersTop);
          else if (uint32_t tn = wm ? tint_of(t, res) : 0) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + rh), col(tn));
          if (ri > 0) d->AddLine(ImVec2(o.x, y), ImVec2(o.x + total, y), col(t.border_muted));
          float x = o.x, csz = r.head ? 11 : tsz;
          for (size_t c = 0; c < nc; ++c) {
            std::vector<md::Span> cell = c < r.cells.size() ? r.cells[c] : std::vector<md::Span>{};
            if (r.head) for (auto& s : cell) s.style |= md::Bold;
            float cwid = rich_width(cell, csz);
            int al = c < b.align.size() ? b.align[c] : 0;
            float off = al == 2 ? std::max(0.f, w[c] - pad * 2 - cwid) : al == 1 ? std::max(0.f, (w[c] - pad * 2 - cwid) / 2) : 0;
            ImGui::SetCursorScreenPos(ImVec2(x + pad + off, y + 4 * k()));
            rich(cell, w[c] - pad * 2, csz, r.head || muted_row ? t.muted : t.fg, false, tline);
            x += w[c];
          }
          // the gutter: the result's glyph; ▶ under the mouse on a test row
          bool row_hov = win_hov && mouse.y >= y && mouse.y < y + rh && mouse.x >= o.x - gutter && mouse.x < o.x + total;
          if (marks && test && lk && !r.head && row_hov) {
            ImGui::SetCursorScreenPos(ImVec2(o.x - gutter, y + (rh - 16 * k()) / 2));
            ImGui::PushID(r.src);
            if (ImGui::InvisibleButton("##runrow", ImVec2(16 * k(), 16 * k()))) run_line(path, r.src);
            if (ImGui::IsItemHovered()) tip("Run this row");
            ImGui::PopID();
            icon_play(d, ImVec2(o.x - gutter + 3 * k(), y + (rh - 9 * k()) / 2), 9 * k(), col(t.success));
          } else if (rmarks) {
            gutter_mark(rmarks, y + (rh - 20 * k()) / 2, 20 * k());
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
                ImGui::PushFont(f.mono, code() * k());
                ImVec2 ts = ImGui::CalcTextSize(ln.c_str(), nullptr, false, total - pad * 2);
                float nh = ts.y + 5 * k();
                if (uint32_t tn = tint_of(t, res)) d->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + total, y + nh), col(tn));
                d->AddText(f.mono, code() * k(), ImVec2(o.x + pad, y), col(level_fg(t, m)), ln.c_str(), nullptr, total - pad * 2);
                ImGui::PopFont();
                y += nh;
                if (nl == std::string::npos) break;
                at_ = nl + 1;
              }
            }
        }
        d->AddRect(o, ImVec2(o.x + total, y), col(t.border), 6 * k());
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
            pill(first_line(m.text).empty() ? short_result(m.text) : first_line(m.text), tn, 18, g.c_str(), false);
            std::string rest = after_first_line(m.text);
            if (!rest.empty()) {
              ImGui::SetCursorPosX(cx);
              text_wrapped(rest, t.danger, f.mono, code(), cw);
            }
          }
        }
        gap(compact ? 8 : 12);
        break;
      }
    }
  }
  if (jump_path == path && jump_src >= 0 && !compact) jump_src = -1;  // past the end
  ImGui::PopStyleVar();
}

// -- the reader pane ------------------------------------------------------------------

// A 28px banner: the glyph, the title in its tone (500), the action on the
// right; a body (why it failed) under it in the same box.
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
  float W = std::min(ImGui::GetContentRegionAvail().x, 880 * k()), H = 28 * k(), fs = ImGui::GetFontSize();
  ImVec2 p = ImGui::GetCursorScreenPos();
  d->ChannelsSplit(2);
  d->ChannelsSetCurrent(1);
  float aw = action.empty() ? 0 : button_width(action, Btn::Default, lk->action.key, nullptr, 20) + 4 * k();
  ImVec2 gs = f.mono->CalcTextSizeA(fs, FLT_MAX, 0, glyph.c_str());
  d->AddText(f.mono, fs, ImVec2(p.x + 10 * k(), p.y + (H - gs.y) / 2), col(fg), glyph.c_str());
  ImGui::PushFont(f.medium, 0.f);
  ImGui::PushStyleColor(ImGuiCol_Text, col(fg));
  float tx = p.x + 10 * k() + gs.x + 8 * k(), tr = p.x + W - aw - 8 * k();
  ImGui::RenderTextEllipsis(d, ImVec2(tx, p.y + (H - fs) / 2), ImVec2(tr, p.y + H), tr, title.c_str(), nullptr, nullptr);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  if (!action.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(p.x + W - aw, p.y + (H - 20 * k()) / 2));
    if (button(action + "##banner" + std::to_string(reinterpret_cast<uintptr_t>(&m)), Btn::Default, 20, lk->action.key))
      plugin_action(lk->action.key, reader_url, spec ? path : "");
  }
  float h = H;
  if (!body.empty()) {
    ImGui::SetCursorScreenPos(ImVec2(tx, p.y + H - 4 * k()));
    text_wrapped(body, t.fg, mono ? f.mono : f.regular, mono ? code() : 12, W - (tx - p.x) - 10 * k());
    h = ImGui::GetItemRectMax().y - p.y + 6 * k();
  }
  d->ChannelsSetCurrent(0);
  d->AddRectFilled(p, p + ImVec2(W, h), col(bg), 6 * k());
  d->AddRect(p, p + ImVec2(W, h), col((fg & 0xffffff00u) | 0x66u), 6 * k());  // the tone at 40%
  d->ChannelsMerge();
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
  ImGui::Dummy(ImVec2(W, 10 * k()));
}

// The PR's title (14/600), then its meta line and GitHub on the right.
void Gui::reader_header(const Pr& p) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  float W = ImGui::GetContentRegionAvail().x, fs = 14 * k();
  ImVec2 tp = ImGui::GetCursorScreenPos();
  std::string title = shown_title(p.title);
  ImGui::PushFont(f.semibold, fs);
  ImGui::RenderTextEllipsis(d, tp, ImVec2(tp.x + W, tp.y + 20 * k()), tp.x + W, title.c_str(), nullptr, nullptr);
  ImGui::PopFont();
  ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + 24 * k()));
  std::string head = bodies[reader_url].head;
  std::string where = short_repo(p.repo) + "#" + std::to_string(p.number) + (head.empty() ? "" : " · " + head.substr(0, 7));
  ImVec2 mp = ImGui::GetCursorScreenPos();
  meta_line(p, {{where, t.muted}});
  // GitHub, a link at the end of the line
  ImVec2 gs = f.regular->CalcTextSizeA(11 * k(), FLT_MAX, 0, "GitHub");
  ImGui::SetCursorScreenPos(ImVec2(mp.x + W - gs.x, mp.y));
  if (ImGui::InvisibleButton("##github", ImVec2(gs.x, 16 * k()))) open(p.url);
  bool hov = ImGui::IsItemHovered();
  d->AddText(f.regular, 11 * k(), ImVec2(mp.x + W - gs.x, mp.y + (16 * k() - gs.y) / 2), col(t.accent), "GitHub");
  if (hov) d->AddLine(ImVec2(mp.x + W - gs.x, mp.y + 14 * k()), ImVec2(mp.x + W, mp.y + 14 * k()), col(t.accent));
}

// The documents, as 28px tabs of 11px text: the file's state as a glyph,
// the name; the open one in fg and 600 over the tab line.
void Gui::reader_tabs(const std::vector<Doc>& ds) {
  ImDrawList* d = ImGui::GetWindowDrawList();
  ImVec2 o = ImGui::GetCursorScreenPos();
  float H = 28 * k(), W = ImGui::GetContentRegionAvail().x, fs = 11 * k();
  ImVec2 wp = ImGui::GetWindowPos();
  d->AddLine(ImVec2(wp.x, o.y + H - 0.5f), ImVec2(wp.x + ImGui::GetWindowWidth(), o.y + H - 0.5f), col(t.border_muted));
  ImGui::BeginChild("##tabs", ImVec2(W, H), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_HorizontalScrollbar);
  ImDrawList* cd = ImGui::GetWindowDrawList();
  std::string head = bodies[reader_url].head;
  for (size_t i = 0; i < ds.size(); ++i) {
    const Doc& doc = ds[i];
    // its mark: the file's state (line 0), the run's on the description
    std::string mark;
    uint32_t mc = t.muted;
    if (doc.path.empty()) {
      for (auto& st : live)
        if (auto it = st.prs.find(reader_url); it != st.prs.end() && it->second.marks_summary)
          mark = it->second.marks_summary->glyph, mc = level_fg(t, *it->second.marks_summary);
    } else {
      const Mark* w0 = nullptr;
      bool broken = false;
      for (auto& m : marks_on(reader_url, doc.path, head))
        if (m.line == 0) {
          if (m.banner) broken = true;
          else if (!w0 || severity(m) > severity(*w0)) w0 = &m;
        }
      if (broken) mark = "!", mc = t.danger;
      else if (w0) mark = w0->glyph, mc = level_fg(t, *w0);
    }
    bool on = i == reader_tab;
    ImFont* nf = on ? f.semibold : f.regular;
    ImVec2 ns = nf->CalcTextSizeA(fs, FLT_MAX, 0, doc.name.c_str());
    ImVec2 ms = mark.empty() ? ImVec2(0, 0) : f.mono->CalcTextSizeA(fs, FLT_MAX, 0, mark.c_str());
    float mw = mark.empty() ? 0 : ms.x + 5 * k(), w = mw + ns.x;
    if (i) ImGui::SameLine(0, 14 * k());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##tab", ImVec2(w, H))) reader_tab = i;
    bool hov = ImGui::IsItemHovered();
    if (hov && !doc.path.empty()) tip("%s", doc.path.c_str());
    ImGui::PopID();
    if (!mark.empty()) cd->AddText(f.semibold, fs, ImVec2(p.x, p.y + (H - ms.y) / 2), col(mc), mark.c_str());
    cd->AddText(nf, fs, ImVec2(p.x + mw, p.y + (H - ns.y) / 2), col(on || hov ? t.fg : t.muted), doc.name.c_str());
    if (on) cd->AddRectFilled(ImVec2(p.x, p.y + H - 2 * k()), ImVec2(p.x + w, p.y + H), col(t.tab_line));
  }
  if (const Files* fs_ = reader_files(); !fs_ || fs_->loading) {
    ImGui::SameLine(0, 14 * k());
    ImGui::SetCursorPosY((H - fs) / 2);
    text("loading files…", t.muted, nullptr, 11);
  } else if (!fs_->error.empty()) {
    ImGui::SameLine(0, 14 * k());
    ImGui::SetCursorPosY((H - fs) / 2);
    text(fs_->error, t.danger, nullptr, 11);
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
  // One reader, its tab named after the PR; the ###reader ID is what ImGui
  // remembers its place by. Floating, it's a window of its own (a
  // viewport that never merges into the main one).
  ImGuiWindowClass wc;
  wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton;  // its tab has the ×
  if (cfg.gui.reader_placement == "window") wc.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
  ImGui::SetNextWindowClass(&wc);
  if (reader_dock) {
    ImGui::SetNextWindowDockID(reader_dock, ImGuiCond_Always);
    reader_dock = 0;
  }
  if (reader_float) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(std::min(760 * k(), vp->WorkSize.x * 0.8f), vp->WorkSize.y * 0.85f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(vp->WorkPos + vp->WorkSize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    reader_float = false;
  }
  bool focusing = focus_reader;
  if (focus_reader) {
    ImGui::SetNextWindowFocus();
    focus_reader = false;
  }
  std::string title = short_repo(p.repo) + "#" + std::to_string(p.number) + "###reader";
  bool open = true;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 10 * k()));
  bool shown = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                      ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  if (!open) close_reader();
  if (ImGuiDockNode* n = ImGui::GetWindowDockNode(); focusing && n && n->TabBar)  // its tab to the front
    n->TabBar->NextSelectedTabId = ImGui::GetCurrentWindow()->TabId;
  if (!shown) return ImGui::End();
  reader_header(p);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * k() - ImGui::GetStyle().ItemSpacing.y);
  reader_tabs(ds);
  const Doc& doc = ds[reader_tab];
  ImGui::SetCursorPosX(0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 10 * k()));
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

// -- the panes around it ----------------------------------------------------------------------

// Results, the canvas's way: a 22px line with the document and its counts
// (Run on the right), then a 24px row per table (on the description, per
// spec file): its glyph, name, line and result word; a click goes there.
// Under them what did not pass, the last command and the other actions.
void Gui::results_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 6 * k()));
  bool shown = ImGui::Begin("Results###results", nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto ds = docs();
  if (reader_tab >= ds.size()) return ImGui::End();
  const Doc& doc = ds[reader_tab];
  std::string head = bodies[reader_url].head;
  auto lk = line_key();
  ImDrawList* d = ImGui::GetWindowDrawList();
  uint32_t hover = t.dark ? t.border_muted : t.inset;
  float fs = ImGui::GetFontSize(), mfs = 11 * k(), rh = 24 * k();
  auto style_fg = [&](unsigned st) {
    return st & md::Error ? t.danger : st & md::Warn ? t.attention : st & md::Ok ? t.success : t.muted;
  };
  // a row: glyph, name (and a muted note after it), "line N", the word on the right
  auto row = [&](const std::string& id, const std::string& g, uint32_t gc, const std::string& name, const std::string& note,
                 int ln, const std::string& word) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    bool clicked = ImGui::InvisibleButton(id.c_str(), ImVec2(w, rh));
    if (ImGui::IsItemHovered()) d->AddRectFilled(p, p + ImVec2(w, rh), col(hover));
    d->AddLine(p, ImVec2(p.x + w, p.y), col(t.border_muted));
    d->AddText(f.mono, mfs, ImVec2(p.x, p.y + (rh - mfs) / 2), col(gc), g.c_str());
    float right = p.x + w;
    if (!word.empty()) {
      ImVec2 ws = f.regular->CalcTextSizeA(mfs, FLT_MAX, 0, word.c_str());
      right -= std::max(56 * k(), ws.x);
      d->AddText(f.regular, mfs, ImVec2(p.x + w - ws.x, p.y + (rh - ws.y) / 2), col(gc), word.c_str());
    }
    if (ln > 0) {
      std::string l = "line " + std::to_string(ln);
      ImVec2 ls = f.mono->CalcTextSizeA(mfs, FLT_MAX, 0, l.c_str());
      right -= ls.x + 8 * k();
      d->AddText(f.mono, mfs, ImVec2(right, p.y + (rh - ls.y) / 2), col(t.muted), l.c_str());
    }
    float x = p.x + 18 * k(), r = right - 8 * k();
    ImGui::PushStyleColor(ImGuiCol_Text, col(t.fg));
    ImGui::RenderTextEllipsis(d, ImVec2(x, p.y + (rh - fs) / 2), ImVec2(r, p.y + rh), r, name.c_str(), nullptr, nullptr);
    ImGui::PopStyleColor();
    float nw = f.regular->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str()).x;
    if (!note.empty() && x + nw + 8 * k() < r) {
      ImGui::PushFont(f.regular, mfs);
      ImGui::PushStyleColor(ImGuiCol_Text, col(gc));
      ImGui::RenderTextEllipsis(d, ImVec2(x + nw + 8 * k(), p.y + (rh - mfs) / 2), ImVec2(r, p.y + rh), r, note.c_str(), nullptr, nullptr);
      ImGui::PopStyleColor();
      ImGui::PopFont();
      if (ImGui::IsItemHovered()) tip("%s", note.c_str());
    }
    return clicked;
  };
  if (doc.spec || doc.path.empty()) {
    std::vector<Mark> ms = doc.path.empty() ? std::vector<Mark>{} : marks_on(reader_url, doc.path, head);
    // the line: the document, its counts, Run file / Run all
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x, h = 22 * k();
      float bx = p.x + w;
      if (lk) {
        std::string all = doc.spec ? "Run all" : "Run all specs";
        bx -= button_width(all, Btn::Default, doc.spec ? "" : lk->action.key, nullptr, 20);
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + 1 * k()));
        if (button(all + "##all", Btn::Default, 20, doc.spec ? "" : lk->action.key)) plugin_action(lk->action.key, reader_url);
        if (doc.spec) {
          bx -= 4 * k() + button_width("Run file", Btn::Default, lk->action.key, nullptr, 20);
          ImGui::SetCursorScreenPos(ImVec2(bx, p.y + 1 * k()));
          if (button("Run file", Btn::Default, 20, lk->action.key)) plugin_action(lk->action.key, reader_url, doc.path);
        }
      }
      float x = p.x;
      auto put = [&](const std::string& s, uint32_t c) {
        if (x + 40 * k() > bx) return;
        d->AddText(f.regular, mfs, ImVec2(x, p.y + (h - mfs) / 2), col(c), s.c_str());
        x += f.regular->CalcTextSizeA(mfs, FLT_MAX, 0, s.c_str()).x + 12 * k();
      };
      put(doc.path.empty() ? "This pull request" : doc.name, t.muted);
      const Mark* state = nullptr;
      std::vector<Mark> top = doc.path.empty() ? reader_top("", false, {}) : std::vector<Mark>{};
      for (auto& m : doc.path.empty() ? top : ms)
        if (m.line == 0 && (!state || severity(m) > severity(*state) || m.banner)) state = &m;
      if (const Files* fs_ = reader_files(); doc.spec && fs_)
        for (auto& ft : fs_->tabs)
          if (ft.path == doc.path)
            for (auto& c : md::tally(ms, ft.dirs)) put(c.glyph + " " + std::to_string(c.n), style_fg(c.style));
      if (state && (doc.path.empty() || state->banner))
        put((state->glyph.empty() ? "!" : state->glyph) + " " + (state->banner ? "temoc failed" : short_result(state->text)),
            state->banner ? t.danger : level_fg(t, *state));
      if (!state && !lk) put("No plugin runs specs (a \"line\" action).", t.muted);
      else if (!state) put("not run", t.muted);
      ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
      ImGui::Dummy(ImVec2(w, 0));
    }
    if (doc.spec) {
      const auto& bs = blocks_of(*doc.text);
      auto at = attach(bs, ms);
      for (size_t i = 0; i < bs.size(); ++i) {
        if (bs[i].kind != md::Block::Table || !bs[i].test) continue;
        auto it = at.find(bs[i].test);
        const Mark* m = it != at.end() ? worst(it->second) : nullptr;
        uint32_t c = m ? level_fg(t, *m) : t.muted;
        std::string g = m ? (m->glyph.empty() ? result_glyph(result_of(*m)) : m->glyph) : "·";
        if (row("##table" + std::to_string(i), g, c, table_name(bs, i), "", bs[i].test, m ? short_result(m->text) : "not run")) {
          focus[doc.path] = bs[i].test;
          jump(doc.path, bs[i].test);
        }
      }
      // what did not pass: each failing row, table and file, in line order
      struct Bad {
        std::string where, text;
        uint32_t c;
        int line;
        std::string glyph;
      };
      std::vector<Bad> bad;
      for (auto& m : ms) {
        if (m.level != "error" && m.level != "warn") continue;
        std::string g = m.glyph.empty() ? "!" : m.glyph;
        if (m.line == 0) {
          if (m.banner) bad.push_back({"The whole file", m.text.substr(m.text.find(": ") == std::string::npos ? 0 : m.text.find(": ") + 2), t.danger, 0, g});
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
        bad.push_back({where, first_line(txt), m.level == "error" ? t.danger : t.attention, m.line, g});
      }
      std::stable_sort(bad.begin(), bad.end(), [](const Bad& a, const Bad& b) { return a.line < b.line; });
      if (!bad.empty()) {
        ImGui::Dummy(ImVec2(1, 8 * k()));
        section("What did not pass");
        for (size_t i = 0; i < bad.size(); ++i)
          if (row("##bad" + std::to_string(i), bad[i].glyph, bad[i].c, bad[i].where, bad[i].text, bad[i].line, ""))
            jump(doc.path, bad[i].line);
      }
    } else {
      // the description: each spec file's result, a click opens it
      bool any = false;
      for (size_t i = 1; i < ds.size(); ++i) {
        if (!ds[i].spec) continue;
        any = true;
        const Mark* w0 = nullptr;
        auto fm = marks_on(reader_url, ds[i].path, head);
        for (auto& m : fm)
          if (m.line == 0 && (!w0 || m.banner || severity(m) > severity(*w0))) w0 = &m;
        uint32_t c = w0 ? (w0->banner ? t.danger : level_fg(t, *w0)) : t.muted;
        std::string g = w0 ? (w0->banner ? "!" : w0->glyph) : "·";
        std::string word = w0 ? (w0->banner ? "temoc failed" : short_result(w0->text)) : "not run";
        if (row("##spec" + std::to_string(i), g, c, ds[i].name, "", 0, word)) reader_tab = i;
      }
      if (!any) text(reader_files() && !reader_files()->loading ? "This PR changes no spec file." : "Loading files…", t.muted, nullptr, 11);
    }
    // the command the results came from, and the commit
    for (size_t i = 0; i < live.size(); ++i) {
      auto it = live[i].prs.find(reader_url);
      if (it == live[i].prs.end() || it->second.marks_command.empty()) continue;
      ImGui::Dummy(ImVec2(1, 8 * k()));
      section("Last command");
      ImGui::Dummy(ImVec2(1, 4 * k()));
      text_wrapped(it->second.marks_command, t.fg2, f.mono, code());
      const std::string& ref = it->second.marks_ref;
      bool moved = !ref.empty() && !head.empty() && !head.starts_with(ref);
      std::string on = ref.empty() ? "" : "on " + ref.substr(0, 7) + (moved ? "; the PR has moved on to " + head.substr(0, 7) : ", the PR's head");
      if (!on.empty()) text_wrapped(on, moved ? t.attention : t.muted, nullptr, 11);
    }
  } else {
    // any other file: a line per plugin with marks on it
    bool any = false;
    for (size_t i = 0; i < live.size(); ++i) {
      auto ms = marks_on(reader_url, doc.path, head, nullptr, static_cast<int>(i));
      if (ms.empty()) continue;
      any = true;
      std::vector<std::pair<std::string, uint32_t>> parts = {{cfg.plugins[i].name + "  ", t.muted}};
      if (const Files* fs_ = reader_files())
        for (auto& ft : fs_->tabs)
          if (ft.path == doc.path)
            for (auto& c : md::tally(ms, ft.dirs)) parts.push_back({c.glyph + " " + std::to_string(c.n) + "  ", style_fg(c.style)});
      if (parts.size() == 1) parts.push_back({std::to_string(ms.size()) + " marks", t.fg});
      segments(parts, nullptr, 11);
    }
    if (!any) text("No plugin says anything about this file.", t.muted, nullptr, 11);
  }
  // the PR's other plugin actions
  std::vector<PluginKey> others;
  for (auto& pk : plugin_keys())
    if (!pk.action.line) others.push_back(pk);
  if (!others.empty()) {
    ImGui::Dummy(ImVec2(1, 8 * k()));
    section("Actions");
    ImGui::Dummy(ImVec2(1, 4 * k()));
    for (auto& [i, a] : others) {
      if (button(button_label(a.button, a.label, a.id) + "##act" + a.id, Btn::Default, 20, a.key))
        plugin_action(a.key, reader_url);
      if (ImGui::IsItemHovered()) tip("%s  %s", strip_emoji(a.label.empty() ? a.id : a.label).c_str(), a.key.c_str());
      ImGui::SameLine(0, 4 * k());
    }
    ImGui::NewLine();
  }
  ImGui::End();
}

void Gui::outline_pane() {
  ImGui::SetNextWindowDockID(ImGui::FindWindowByName("###results") ? ImGui::FindWindowByName("###results")->DockId : 0,
                             ImGuiCond_FirstUseEver);  // first shown: a tab beside Results
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k(), 12 * k()));
  bool shown = ImGui::Begin("Outline###outline", &show_outline, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto ds = docs();
  if (reader_tab < ds.size() && ds[reader_tab].text) {
    const Doc& doc = ds[reader_tab];
    const auto& bs = blocks_of(*doc.text);
    std::vector<Mark> ms = doc.path.empty() ? std::vector<Mark>{} : marks_on(reader_url, doc.path, bodies[reader_url].head);
    auto at = attach(bs, ms);
    section("On this page");
    ImGui::Dummy(ImVec2(1, 2 * k()));
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
      float w = ImGui::GetContentRegionAvail().x, h = 24 * k();
      if (ImGui::InvisibleButton("##ol", ImVec2(w, h))) jump(doc.path, b.kind == md::Block::Table ? b.test : b.src);
      if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 6 * k());
      float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
      if (!g.empty()) ImGui::GetWindowDrawList()->AddText(f.mono, code() * k(), ImVec2(p.x + indent - 16 * k(), ty), col(c), g.c_str());
      ImGui::GetWindowDrawList()->AddText(b.kind == md::Block::Heading && b.heading == 1 ? f.semibold : f.regular, ImGui::GetFontSize(),
                                          ImVec2(p.x + 8 * k() + indent, ty), col(t.fg), label.c_str());
      ImGui::PopID();
    }
    if (!any) text("No headings.", t.muted);
  }
  ImGui::End();
}

void Gui::llm_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 10 * k()));
  if (focus_llm) {
    ImGui::SetNextWindowFocus();
    focus_llm = false;
  }
  ImGui::SetNextWindowDockID(ImGui::FindWindowByName("###results") ? ImGui::FindWindowByName("###results")->DockId : 0,
                             ImGuiCond_FirstUseEver);
  bool shown = ImGui::Begin("LLM review###llm", &show_llm, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  auto notes = notes_of(reader_url);
  if (notes.empty()) text_wrapped("No plugin left a note on this PR. anton-llm's reviews land here.", t.muted);
  for (auto& [name, body] : notes) {
    if (notes.size() > 1) section(name);
    markdown(body, "", false, false, true);
    ImGui::Dummy(ImVec2(1, 12 * k()));
  }
  ImGui::End();
}

void Gui::runlog_pane() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 6 * k()));
  bool shown = ImGui::Begin("Run log###runlog", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImGui::PushFont(f.mono, code() * k());
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12 * k(), 4 * k()));
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
    ImGui::SetCursorPosX(x + 130 * k());
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
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * k(), 6 * k()));
  bool shown = ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!shown) return ImGui::End();
  ImGui::PushFont(f.mono, code() * k());
  if (ps.empty()) text("No problems on this PR.", t.muted);
  for (size_t i = 0; i < ps.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    std::string where = ps[i].path.empty() ? "plugin" : path_tail(ps[i].path, 1) + (ps[i].line > 0 ? ":" + std::to_string(ps[i].line) : "");
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 20 * k();
    if (ImGui::InvisibleButton("##pb", ImVec2(w, h)) && !ps[i].path.empty()) jump(ps[i].path, std::max(0, ps[i].line));
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(w, h), col(t.dark ? t.border_muted : t.inset), 4 * k());
    float ty = p.y + (h - ImGui::GetFontSize()) / 2;
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 4 * k(), ty), col(ps[i].c), where.c_str());
    std::string txt = first_line(ps[i].text);
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 150 * k(), ty), col(t.fg2), txt.c_str());
    ImGui::PopID();
  }
  ImGui::PopFont();
  ImGui::End();
}

}  // namespace anton::gui
