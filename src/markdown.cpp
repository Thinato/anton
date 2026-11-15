#include "markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <climits>
#include <map>
#include <set>
#include <sstream>

#include <ftxui/screen/string.hpp>
#include <md4c.h>

#include "model.hpp"

namespace anton::md {
namespace {

int cells(const std::string& s) { return ftxui::string_width(s); }

std::string repeat(const std::string& s, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += s;
  return out;
}

// s cut to w cells, ending in "…" when it didn't fit.
std::string cut(const std::string& s, int w) {
  if (cells(s) <= w) return s;
  std::string out;
  int used = 0;
  for (auto& g : ftxui::Utf8ToGlyphs(s)) {
    int gw = cells(g);
    if (used + gw > w - 1) break;
    out += g;
    used += gw;
  }
  return out + "…";
}

void add(std::vector<Span>& to, const std::string& text, unsigned style) {
  if (text.empty()) return;
  if (!to.empty() && to.back().style == style && to.back().text != "\n")
    to.back().text += text;
  else
    to.push_back({text, style});
}

std::string utf8(unsigned cp) {
  // Control characters and non-characters become U+FFFD: an entity like
  // &#27; must not smuggle an escape into the terminal.
  if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || (cp >= 0xd800 && cp < 0xe000) || cp > 0x10ffff)
    cp = 0xfffd;
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xc0 | (cp >> 6));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xe0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    s += static_cast<char>(0xf0 | (cp >> 18));
    s += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    s += static_cast<char>(0x80 | (cp & 0x3f));
  }
  return s;
}

// "&amp;" -> "&". md4c hands entities over verbatim; numeric ones decode,
// the few named ones PR bodies use decode, anything else stays as written.
std::string entity(const std::string& e) {
  static const std::map<std::string, std::string> named = {
      {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
      {"mdash", "—"}, {"ndash", "–"}, {"hellip", "…"}, {"copy", "©"}, {"rarr", "→"}, {"larr", "←"}};
  if (e.size() < 3 || e.front() != '&' || e.back() != ';') return e;
  std::string body = e.substr(1, e.size() - 2);
  if (body[0] != '#') {
    auto it = named.find(body);
    return it != named.end() ? it->second : e;
  }
  bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
  std::string digits = body.substr(hex ? 2 : 1);
  if (digits.empty() || digits.size() > 7) return e;
  for (char c : digits)
    if (!(hex ? std::isxdigit(static_cast<unsigned char>(c)) : std::isdigit(static_cast<unsigned char>(c))))
      return e;
  return utf8(static_cast<unsigned>(std::stoul(digits, nullptr, hex ? 16 : 10)));
}

// Every "&...;" in raw HTML text, decoded.
std::string entities(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    size_t semi = s[i] == '&' ? s.find(';', i) : std::string::npos;
    if (semi != std::string::npos && semi - i <= 10) {
      out += entity(s.substr(i, semi - i + 1));
      i = semi;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Word wrap to w cells. Words break at spaces (a run of them is one break);
// a word wider than w is cut across lines. A "\n" span is a hard break.
std::vector<std::vector<Span>> wrap(const std::vector<Span>& in, int w) {
  std::vector<std::vector<Span>> out;
  std::vector<Span> line, word;
  int lw = 0, ww = 0;
  unsigned space_style = 0;
  auto newline = [&] {
    out.push_back(std::move(line));
    line.clear();
    lw = 0;
  };
  auto put_word = [&] {
    if (word.empty()) return;
    if (lw > 0 && lw + 1 + ww > w) newline();
    if (lw > 0) {
      add(line, " ", space_style);
      ++lw;
    }
    for (auto& s : word) {
      if (lw + cells(s.text) <= w) {
        add(line, s.text, s.style);
        lw += cells(s.text);
        continue;
      }
      for (auto& g : ftxui::Utf8ToGlyphs(s.text)) {
        int gw = cells(g);
        if (lw > 0 && lw + gw > w) newline();
        add(line, g, s.style);
        lw += gw;
      }
    }
    word.clear();
    ww = 0;
  };
  for (auto& s : in) {
    if (s.text == "\n") {
      put_word();
      newline();
      continue;
    }
    size_t at = 0;
    while (at < s.text.size()) {
      size_t sp = s.text.find(' ', at);
      std::string piece = s.text.substr(at, sp == std::string::npos ? std::string::npos : sp - at);
      add(word, piece, s.style);
      ww += cells(piece);
      if (sp == std::string::npos) break;
      put_word();
      space_style = s.style;
      at = sp + 1;
    }
  }
  put_word();
  if (!line.empty() || out.empty()) out.push_back(std::move(line));
  return out;
}

// Whether the current line (after the last hard break) shows anything: a
// break on an empty line would be a blank row.
bool visible(const std::vector<Span>& spans) {
  for (auto it = spans.rbegin(); it != spans.rend() && it->text != "\n"; ++it)
    if (it->text.find_first_not_of(' ') != std::string::npos) return true;
  return false;
}

std::string text_of(const std::vector<Span>& spans, size_t from = 0) {
  std::string s;
  for (size_t i = from; i < spans.size(); ++i) s += spans[i].text;
  return s;
}

std::string attr(const MD_ATTRIBUTE& a) { return a.text ? std::string(a.text, a.size) : ""; }

// The value of name="..." (or '...') in an HTML tag, "" if it has none.
std::string html_attr(const std::string& tag, const std::string& name) {
  std::string low = lower(tag);
  for (size_t at = low.find(name + "="); at != std::string::npos; at = low.find(name + "=", at + 1)) {
    if (at > 0 && !std::isspace(static_cast<unsigned char>(low[at - 1]))) continue;
    size_t v = at + name.size() + 1;
    if (v >= tag.size()) return "";
    char q = tag[v];
    if (q != '"' && q != '\'') return tag.substr(v, tag.find_first_of(" >", v) - v);
    size_t end = tag.find(q, v + 1);
    return end == std::string::npos ? "" : tag.substr(v + 1, end - v - 1);
  }
  return "";
}

std::string strip(const std::string& s) {
  size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

unsigned level_style(const std::string& level) {
  return level == "ok" ? Ok : level == "error" ? Error : level == "warn" ? Warn : Dim;
}

int rank(const std::string& level) { return level == "error" ? 3 : level == "warn" ? 2 : level == "ok" ? 1 : 0; }

// What a mark shows in a gutter: its glyph, at most two cells.
std::string glyph(const Mark& m) { return m.glyph.empty() ? "•" : cut(m.glyph, 2); }

struct Renderer {
  const std::string& doc;
  int width;
  std::string today;
  std::vector<Directive> dirs;
  size_t next_dir = 0;  // the first directive not shown yet
  bool test_next = false;  // a decisionTable badge was the last line out: the next table is a test
  int test_dir = 0;        // ...and that badge's directive line
  std::vector<size_t> line_starts;
  std::vector<Line> out;

  // A container's prefix: "│ " for a quote, the marker then its width in
  // spaces for a list item.
  struct Box {
    std::string first, rest;
    unsigned style = 0;
    bool used = false;
  };
  std::vector<Box> boxes;
  struct List {
    bool ordered;
    unsigned next;
  };
  std::vector<List> lists;
  // A blank line goes before the next emitted line, prefixed by the first
  // gap_depth boxes (the ones still open where the gap started).
  bool gap = false;
  size_t gap_depth = SIZE_MAX;

  // Inline text of the block being read, and where it started.
  std::vector<Span> inl;
  int inl_src = 0;
  std::vector<unsigned> styles;  // nested spans, innermost last
  unsigned base = 0;             // the block's own style (headings)
  int heading = 0;
  struct LinkAt {
    size_t from;
    std::string href;
  };
  std::vector<LinkAt> links;

  bool in_code = false;
  std::string code_line;
  int code_src = 0, last_code_src = 0;

  // Raw HTML is read a piece at a time; tags and comments can span pieces.
  bool in_tag = false, in_comment = false, in_summary = false;
  std::string tag, html_text;

  struct Row {
    std::vector<std::vector<Span>> cells;
    int src = 0;
    bool head = false;
  };
  bool in_table = false, in_head = false;
  std::vector<Row> rows;
  std::vector<MD_ALIGN> aligns;
  MD_ALIGN cell_align = MD_ALIGN_DEFAULT;

  Renderer(const std::string& d, int w, const std::string& t) : doc(d), width(w), today(t), dirs(directives(d)) {
    line_starts.push_back(0);
    for (size_t i = 0; i < doc.size(); ++i)
      if (doc[i] == '\n') line_starts.push_back(i + 1);
  }

  // md4c's text pointers point into doc, so an offset is a source line.
  int line_of(const char* p) const {
    if (p < doc.data() || p >= doc.data() + doc.size()) return 0;
    size_t off = static_cast<size_t>(p - doc.data());
    return static_cast<int>(std::upper_bound(line_starts.begin(), line_starts.end(), off) - line_starts.begin());
  }

  unsigned style() const { return base | (styles.empty() ? 0 : styles.back()); }

  int avail() const {
    int used = 0;
    for (auto& b : boxes) used += cells(b.rest);
    return std::max(8, width - used);
  }

  void set_gap() {
    gap = true;
    gap_depth = std::min(gap_depth, boxes.size());
  }

  // The directives above source line src, as badge lines: md4c renders them
  // as nothing, and they say what a table tests. Pushed bare, without the
  // boxes' prefixes: a directive belongs to no list or quote.
  void badges_before(int src) {
    for (; next_dir < dirs.size() && dirs[next_dir].line < src; ++next_dir) {
      const Directive& d = dirs[next_dir];
      std::vector<Span> spans;
      if (d.import) {
        spans.push_back({"import " + d.target, Dim});
      } else {
        spans.push_back({"🧪 " + d.target, Bold});
        if (expired(d, today)) spans.push_back({" · snooze expired " + d.snooze, Dim});
        else if (!d.snooze.empty()) spans.push_back({" · snoozed until " + d.snooze, Warn});
      }
      if (gap && !out.empty()) out.emplace_back();
      gap = false;
      gap_depth = SIZE_MAX;
      for (auto& w : wrap(spans, width)) {
        Line l;
        l.spans = std::move(w);
        l.src = d.line;
        l.part = Part::Badge;
        out.push_back(std::move(l));
      }
      test_next = !d.import;
      if (!d.import) test_dir = d.line;
    }
  }

  void emit(std::vector<Span> spans, int src, int head = 0, Part part = Part::Text) {
    if (src > 0) badges_before(src);
    if (!in_table) test_next = false;
    if (gap && !out.empty()) {
      Line blank;
      for (size_t i = 0; i < boxes.size() && i < gap_depth; ++i) add(blank.spans, boxes[i].rest, boxes[i].style);
      out.push_back(std::move(blank));
    }
    gap = false;
    gap_depth = SIZE_MAX;
    Line l;
    for (auto& b : boxes) {
      add(l.spans, b.used ? b.rest : b.first, b.style);
      b.used = true;
    }
    for (auto& s : spans) add(l.spans, s.text, s.style);
    l.src = src;
    l.heading = head;
    l.part = part;
    out.push_back(std::move(l));
  }

  // The block's inline text, wrapped and emitted.
  void flush() {
    flush_html_text();
    if (text_of(inl).find_first_not_of(" \n") != std::string::npos)
      for (auto& l : wrap(inl, avail())) emit(std::move(l), inl_src, heading);
    inl.clear();
    inl_src = 0;
  }

  void put(const std::string& s) {
    std::string t = s;
    std::replace(t.begin(), t.end(), '\n', ' ');
    add(inl, t, style());
  }

  void hard_break() {
    if (visible(inl)) inl.push_back({"\n", 0});
  }

  // -- raw HTML -------------------------------------------------------------

  void flush_html_text() {
    if (html_text.empty()) return;
    std::string t = entities(html_text);
    html_text.clear();
    if (in_summary) {
      std::replace(t.begin(), t.end(), '\n', ' ');
      add(inl, t, style() | Bold);
    } else {
      put(t);
    }
  }

  // Comments go; <br> and block-level tags break the line; <summary> starts
  // a "▸ " line; <img alt> becomes [image: alt]; other tags vanish and their
  // text stays.
  void on_tag(const std::string& t) {
    static const std::set<std::string> blocks = {"p",  "div", "details", "h1", "h2", "h3", "h4",
                                                 "h5", "h6",  "li",      "ul", "ol", "table", "tr",
                                                 "hr", "pre", "blockquote"};
    bool closing = t.size() > 1 && t[1] == '/';
    size_t a = closing ? 2 : 1, b = a;
    while (b < t.size() && std::isalnum(static_cast<unsigned char>(t[b]))) ++b;
    std::string name = lower(t.substr(a, b - a));
    if (name == "summary") {
      hard_break();
      in_summary = !closing;
      if (!closing) add(inl, "▸ ", style() | Bold);
      else hard_break();
    } else if (name == "br" || blocks.count(name)) {
      hard_break();
    } else if (name == "img" && !closing) {
      std::string alt = entities(html_attr(t, "alt"));
      put(alt.empty() ? "[image]" : "[image: " + alt + "]");
    }
  }

  void html(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      if (in_comment) {
        tag += c;
        if (tag.size() >= 3 && tag.compare(tag.size() - 3, 3, "-->") == 0) {
          in_comment = false;
          tag.clear();
        }
      } else if (in_tag) {
        if (tag.size() == 1 && !std::isalpha(static_cast<unsigned char>(c)) && c != '/' && c != '!') {
          in_tag = false;  // "a < b" is text, not a tag
          html_text += tag;
          tag.clear();
          --i;
          continue;
        }
        tag += c;
        if (tag == "<!--") {
          in_tag = false;
          in_comment = true;
          tag = "  ";  // so "<!-->" doesn't close at once
        } else if (c == '>') {
          in_tag = false;
          on_tag(tag);
          tag.clear();
        }
      } else if (c == '<') {
        flush_html_text();
        in_tag = true;
        tag = "<";
      } else {
        html_text += c;
      }
    }
  }

  // -- code blocks ----------------------------------------------------------

  void code(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (code_line.empty() && !code_src) {
        int l = line_of(p + i);
        code_src = l ? l : last_code_src + 1;
      }
      if (p[i] == '\n') {
        end_code_line();
      } else {
        code_line += p[i];
      }
    }
  }

  // Never wrapped: a cut line ends in "…" (y copies the whole text).
  void end_code_line() {
    emit({{"  " + cut(code_line, std::max(2, avail() - 2)), Dim}}, code_src);
    last_code_src = code_src;
    code_line.clear();
    code_src = 0;
  }

  // -- tables ---------------------------------------------------------------

  // Natural column widths, then the widest shrunk one cell at a time until
  // the table fits; cells wrap inside their column.
  void table() {
    size_t ncols = aligns.size();
    for (auto& r : rows) ncols = std::max(ncols, r.cells.size());
    if (ncols == 0) return;
    for (auto& r : rows)
      if (r.src) {
        badges_before(r.src);
        break;
      }
    // A temoc test: a header with a "?" is an output, the expected values,
    // and one starting with "#" a comment temoc never reads.
    std::vector<unsigned> col_style(ncols, 0);
    if (test_next && rows[0].head)
      for (size_t c = 0; c < rows[0].cells.size(); ++c) {
        std::string h = strip(text_of(rows[0].cells[c]));
        col_style[c] = h.starts_with("#") ? Dim : h.find('?') != std::string::npos ? Output : 0;
      }
    const int test = test_next ? test_dir : 0;
    test_next = false;
    std::vector<int> w(ncols, 1);
    for (auto& r : rows)
      for (size_t c = 0; c < r.cells.size(); ++c) w[c] = std::max(w[c], cells(text_of(r.cells[c])));
    int total = 1;
    for (int x : w) total += x + 3;
    // A test's comments give way first: its inputs and outputs are the test.
    for (size_t c = 0; c < ncols && total > avail(); ++c)
      if (int give = std::min(total - avail(), w[c] - 6); col_style[c] == Dim && give > 0) {
        w[c] -= give;
        total -= give;
      }
    while (total > avail()) {
      auto widest = std::max_element(w.begin(), w.end());
      if (*widest <= 2) break;
      --*widest;
      --total;
    }
    auto border = [&](const char* l, const char* m, const char* r, Part part = Part::Table) {
      std::string s = l;
      for (size_t c = 0; c < ncols; ++c) s += repeat("─", w[c] + 2) + (c + 1 < ncols ? m : r);
      emit({{s, Dim}}, 0, 0, part);
      out.back().test = test;
    };
    bool tall = false;  // a wrapped row: separate every row, or they blur
    std::vector<std::vector<std::vector<std::vector<Span>>>> wrapped;
    for (auto& r : rows) {
      wrapped.emplace_back();
      for (size_t c = 0; c < ncols; ++c) {
        std::vector<Span> cell = c < r.cells.size() ? r.cells[c] : std::vector<Span>{};
        for (auto& s : cell) s.style |= col_style[c] | (r.head ? Bold : 0);
        wrapped.back().push_back(wrap(cell, w[c]));
        tall = tall || wrapped.back().back().size() > 1;
      }
    }
    border("┌", "┬", "┐");
    for (size_t ri = 0; ri < rows.size(); ++ri) {
      size_t height = 1;
      for (auto& c : wrapped[ri]) height = std::max(height, c.size());
      for (size_t li = 0; li < height; ++li) {
        std::vector<Span> line = {{"│", Dim}};
        for (size_t c = 0; c < ncols; ++c) {
          auto& cl = wrapped[ri][c];
          std::vector<Span> text = li < cl.size() ? cl[li] : std::vector<Span>{};
          int pad = w[c] - cells(text_of(text));
          MD_ALIGN al = c < aligns.size() ? aligns[c] : MD_ALIGN_DEFAULT;
          int left = al == MD_ALIGN_RIGHT ? pad : al == MD_ALIGN_CENTER ? pad / 2 : 0;
          add(line, std::string(static_cast<size_t>(1 + std::max(0, left)), ' '), 0);
          for (auto& s : text) add(line, s.text, s.style);
          add(line, std::string(static_cast<size_t>(1 + std::max(0, pad - left)), ' '), 0);
          add(line, "│", Dim);
        }
        emit(std::move(line), rows[ri].src, 0, Part::Table);
        out.back().test = test;
        out.back().head = rows[ri].head;
      }
      bool last = ri + 1 == rows.size();
      if (!last && (rows[ri].head != rows[ri + 1].head || tall)) border("├", "┼", "┤");
    }
    border("└", "┴", "┘", Part::TableEnd);
  }

  // -- md4c callbacks -------------------------------------------------------

  void enter_block(MD_BLOCKTYPE type, void* detail) {
    if (type != MD_BLOCK_TH && type != MD_BLOCK_TD) flush();
    switch (type) {
      case MD_BLOCK_QUOTE: boxes.push_back({"│ ", "│ ", Dim}); break;
      case MD_BLOCK_ADMONITION: {  // > [!NOTE]: a quote headed by its kind
        boxes.push_back({"│ ", "│ ", Dim});
        std::string kind = attr(static_cast<MD_BLOCK_ADMONITION_DETAIL*>(detail)->type);
        if (!kind.empty()) kind[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(kind[0])));
        emit({{kind, Bold}}, 0);
        break;
      }
      case MD_BLOCK_UL: lists.push_back({false, 0}); break;
      case MD_BLOCK_OL: lists.push_back({true, static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start}); break;
      case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        static const char* bullets[] = {"• ", "◦ ", "▪ "};
        List& l = lists.back();
        std::string m = l.ordered ? std::to_string(l.next++) + ". " : "";
        if (li->is_task) m += li->task_mark == ' ' ? "☐ " : "☑ ";
        else if (!l.ordered) m = bullets[(lists.size() - 1) % 3];
        boxes.push_back({m, std::string(static_cast<size_t>(cells(m)), ' ')});
        break;
      }
      case MD_BLOCK_FOOTNOTE_DEF: {
        std::string m = "[" + std::to_string(static_cast<MD_BLOCK_FOOTNOTE_DEF_DETAIL*>(detail)->id) + "] ";
        boxes.push_back({m, std::string(static_cast<size_t>(cells(m)), ' '), Dim});
        break;
      }
      case MD_BLOCK_HR:
        emit({{repeat("─", avail()), Dim}}, 0);
        set_gap();
        break;
      case MD_BLOCK_H:
        heading = static_cast<int>(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
        base = Bold;
        add(inl, std::string(static_cast<size_t>(heading), '#') + " ", Bold);
        break;
      case MD_BLOCK_CODE: in_code = true; break;
      case MD_BLOCK_TABLE:
        in_table = true;
        rows.clear();
        aligns.clear();
        break;
      case MD_BLOCK_THEAD: in_head = true; break;
      case MD_BLOCK_TBODY: in_head = false; break;
      case MD_BLOCK_TR: rows.push_back({{}, 0, in_head}); break;
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: cell_align = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align; break;
      default: break;
    }
  }

  void leave_block(MD_BLOCKTYPE type) {
    if (type == MD_BLOCK_TH || type == MD_BLOCK_TD) {
      Row& r = rows.back();
      r.cells.push_back(std::move(inl));
      if (aligns.size() < r.cells.size()) aligns.push_back(cell_align);
      if (!r.src) r.src = inl_src;
      inl.clear();
      inl_src = 0;
      return;
    }
    flush();
    switch (type) {
      case MD_BLOCK_QUOTE:
      case MD_BLOCK_ADMONITION:
        boxes.pop_back();
        set_gap();
        break;
      case MD_BLOCK_UL:
      case MD_BLOCK_OL:
        lists.pop_back();
        if (lists.empty()) set_gap();  // a nested list runs on into its item
        break;
      case MD_BLOCK_LI:
      case MD_BLOCK_FOOTNOTE_DEF: boxes.pop_back(); break;
      case MD_BLOCK_H:
        heading = 0;
        base = 0;
        set_gap();
        break;
      case MD_BLOCK_CODE:
        if (!code_line.empty()) end_code_line();
        in_code = false;
        set_gap();
        break;
      case MD_BLOCK_HTML:
        in_tag = in_comment = in_summary = false;  // an unclosed tag ends with its block
        tag.clear();
        set_gap();
        break;
      case MD_BLOCK_P:
      case MD_BLOCK_FOOTNOTE_DEF_SECTION: set_gap(); break;
      case MD_BLOCK_TR:  // a row of empty cells: count from the one above (the
                         // header's next row is 2 down, past the |---| line)
        if (rows.back().src == 0 && rows.size() > 1)
          rows.back().src = rows[rows.size() - 2].src + (rows[rows.size() - 2].head ? 2 : 1);
        break;
      case MD_BLOCK_TABLE:
        table();
        in_table = false;
        set_gap();
        break;
      default: break;
    }
  }

  void enter_span(MD_SPANTYPE type, void* detail) {
    unsigned bit = 0;
    switch (type) {
      case MD_SPAN_EM: bit = Italic; break;
      case MD_SPAN_STRONG: bit = Bold; break;
      case MD_SPAN_CODE: bit = Code; break;
      case MD_SPAN_DEL: bit = Strike; break;
      case MD_SPAN_A:
        bit = Link;
        links.push_back({inl.size(), attr(static_cast<MD_SPAN_A_DETAIL*>(detail)->href)});
        break;
      case MD_SPAN_IMG: put("[image: "); break;
      case MD_SPAN_FOOTNOTE_REF:
        add(inl, "[" + std::to_string(static_cast<MD_SPAN_FOOTNOTE_REF_DETAIL*>(detail)->id) + "]", style() | Dim);
        break;
      default: break;
    }
    styles.push_back(style() | bit);
  }

  void leave_span(MD_SPANTYPE type) {
    styles.pop_back();
    if (type == MD_SPAN_IMG) put("]");
    if (type == MD_SPAN_A && !links.empty()) {
      // The URL after the text, dim, unless the text already is the URL.
      LinkAt l = links.back();
      links.pop_back();
      std::string text = text_of(inl, std::min(l.from, inl.size()));
      std::string href = printable(l.href);
      if (!href.empty() && text != href && "http://" + text != href && "https://" + text != href &&
          "mailto:" + text != href)
        add(inl, " " + href, style() | Dim);
    }
  }

  void text(MD_TEXTTYPE type, const char* p, size_t n) {
    std::string s(p, n);
    if (in_code && type == MD_TEXT_CODE) return code(p, n);
    if (!inl_src) inl_src = line_of(p);
    switch (type) {
      case MD_TEXT_NULLCHAR: put("�"); break;
      case MD_TEXT_BR: hard_break(); break;
      case MD_TEXT_SOFTBR: put(" "); break;
      case MD_TEXT_ENTITY: put(entity(s)); break;
      case MD_TEXT_HTML: html(s); break;
      default: put(s); break;
    }
  }
};

template <class F>
int call(void* u, F f) {
  f(*static_cast<Renderer*>(u));
  return 0;
}

}  // namespace

std::vector<Directive> directives(const std::string& markdown) {
  std::vector<Directive> out;
  std::istringstream in(markdown);
  std::string fence;  // the open fence's marker
  int n = 0;
  for (std::string l; std::getline(in, l);) {
    ++n;
    size_t i = l.find_first_not_of(' ');
    if (i == std::string::npos || i > 3) continue;
    if (l.compare(i, 3, "```") == 0 || l.compare(i, 3, "~~~") == 0) {
      if (fence.empty()) fence = l.substr(i, 3);
      else if (fence == l.substr(i, 3)) fence.clear();
      continue;
    }
    if (!fence.empty() || l.compare(i, 5, "[//]:") != 0) continue;
    size_t hash = l.find_first_not_of(" \t", i + 5);
    if (hash == std::string::npos || l[hash] != '#') continue;
    size_t q = l.find_first_not_of(" \t", hash + 1);
    if (q == std::string::npos || q == hash + 1 || (l[q] != '"' && l[q] != '\'' && l[q] != '(')) continue;
    size_t e = l.find_last_of(l[q] == '(' ? ')' : l[q]);
    if (e == std::string::npos || e <= q || l.find_first_not_of(" \t", e + 1) != std::string::npos) continue;
    // As temoc reads the title: the command, a space, its argument.
    std::string title = l.substr(q + 1, e - q - 1);
    size_t sp = title.find(' ');
    if (sp == std::string::npos) continue;
    std::string cmd = title.substr(0, sp), arg = strip(title.substr(sp + 1));
    Directive d;
    d.line = n;
    if (cmd == "import") {
      d.import = true;
      d.target = arg;
    } else if (cmd == "decisionTable") {
      size_t dash = arg.find(" -- ");
      d.target = strip(arg.substr(0, dash));
      std::string rest = dash == std::string::npos ? "" : strip(arg.substr(dash + 4));
      if (rest.starts_with("snooze until")) d.snooze = strip(rest.substr(12));
    } else {
      continue;
    }
    if (!d.target.empty()) out.push_back(d);
  }
  return out;
}

std::vector<Line> annotate(const std::vector<Line>& lines, const std::vector<Mark>& marks, int width) {
  // Each mark goes on the first display line of the nearest source line at
  // or above it; none (line 0, or above everything): the top.
  std::map<int, size_t> first;
  for (size_t i = 0; i < lines.size(); ++i)
    if (lines[i].src > 0) first.emplace(lines[i].src, i);
  std::vector<std::vector<const Mark*>> on(lines.size());
  std::vector<const Mark*> top;
  for (auto& m : marks) {
    auto it = first.upper_bound(m.line);
    if (it == first.begin()) top.push_back(&m);
    else on[std::prev(it)->second].push_back(&m);
  }
  auto worst = [](const std::vector<const Mark*>& ms) {
    return *std::max_element(ms.begin(), ms.end(), [](const Mark* a, const Mark* b) { return rank(a->level) < rank(b->level); });
  };
  auto noted = [](const Mark* m) { return !m->text.empty() && (m->level == "error" || m->level == "warn"); };

  // Where texts go: after a block's last line (a row's, a paragraph's, a
  // badge's), or as a footer in place of a test table's bottom border.
  std::map<size_t, std::vector<const Mark*>> after, footer;
  std::vector<unsigned> tint(lines.size(), 0);
  for (size_t i = 0; i < lines.size(); ++i) {
    if (on[i].empty()) continue;
    const Line& l = lines[i];
    size_t end = i;
    while (end + 1 < lines.size() && lines[end + 1].src == l.src && lines[end + 1].part == l.part) ++end;
    if (l.part == Part::Badge) {
      size_t bottom = 0;
      for (size_t k = i + 1; k < lines.size() && !bottom; ++k)
        if (lines[k].part == Part::TableEnd && lines[k].test == l.src) bottom = k;
      for (auto* m : on[i])
        if (bottom) footer[bottom].push_back(m);
        else if (!m->text.empty()) after[end].push_back(m);
    } else if (l.part == Part::Table) {
      unsigned t = level_style(worst(on[i])->level);
      for (size_t k = i; k <= end; ++k) tint[k] = t == Dim ? 0 : t;
      for (auto* m : on[i])
        if (noted(m)) after[end].push_back(m);
    } else {
      for (auto* m : on[i])
        if (noted(m)) after[end].push_back(m);
    }
  }

  // A table's indent (whatever is left of it: a quote's bar, a list's
  // indent) and the width inside its border, from its bottom border.
  auto prefix_of = [](const Line& bottom) {
    std::string t = text_of(bottom.spans);
    return t.substr(0, t.find("└"));
  };
  auto indent_of = [&](const Line& bottom) { return cells(prefix_of(bottom)); };
  auto inner_of = [&](const Line& bottom) { return std::max(1, cells(text_of(bottom.spans)) - indent_of(bottom) - 2); };
  // A table's last column grows to hold its notes and footer, up to the width.
  std::vector<Line> ls = lines;
  std::vector<size_t> bottom_of(ls.size(), 0);  // a table line's bottom border
  for (size_t a = 0; a < ls.size(); ++a) {
    if (ls[a].part != Part::Table || (a > 0 && ls[a - 1].part == Part::Table)) continue;  // not a table's top
    size_t b = a;
    while (b < ls.size() && ls[b].part == Part::Table) ++b;
    if (b == ls.size() || ls[b].part != Part::TableEnd) continue;
    std::fill(bottom_of.begin() + static_cast<long>(a), bottom_of.begin() + static_cast<long>(b) + 1, b);
    int need = 0;
    auto lines_need = [&](const std::string& text, int pad) {
      std::istringstream in(text);
      for (std::string l; std::getline(in, l);) need = std::max(need, cells(l) + pad);
    };
    for (size_t k = a; k < b; ++k)
      if (auto it = after.find(k); it != after.end())
        for (auto* m : it->second) lines_need(m->text, 4);
    if (auto f = footer.find(b); f != footer.end())
      for (auto* m : f->second) lines_need(m->text, 4 + cells(glyph(*m)));
    int extra = std::min(need, width - kGutter - indent_of(ls[b]) - 2) - inner_of(ls[b]);
    for (size_t k = a; k <= b && extra > 0; ++k) {
      Span& last = ls[k].spans.back();  // a row's closing "│", or a border ending in its corner
      if (last.text == "│") ls[k].spans.insert(ls[k].spans.end() - 1, {std::string(static_cast<size_t>(extra), ' '), 0});
      else if (last.text.size() >= 3) last.text.insert(last.text.size() - 3, repeat("─", extra));
    }
  }

  std::vector<Line> out;
  auto gutter = [&](const std::vector<const Mark*>& ms) -> Span {
    if (ms.empty()) return {std::string(kGutter, ' '), 0};
    const Mark* w = worst(ms);
    std::string g = glyph(*w);
    return {g + std::string(static_cast<size_t>(std::max(0, kGutter - cells(g))), ' '), level_style(w->level)};
  };
  // A text's lines as spans, hard breaks between.
  auto spans_of = [](const std::string& text, unsigned style) {
    std::vector<Span> sp;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) {
      if (!sp.empty()) sp.push_back({"\n", 0});
      sp.push_back({l, style});
    }
    return sp;
  };
  // spans wrapped past the gutter, which the first line may fill.
  auto lines_of = [&](const std::vector<Span>& spans, Span head, int indent) {
    auto wrapped = wrap(spans, std::max(8, width - kGutter - indent));
    for (size_t k = 0; k < wrapped.size(); ++k) {
      Line l;
      l.spans.push_back(k ? Span{std::string(static_cast<size_t>(kGutter + indent), ' '), 0} : head);
      for (auto& s : wrapped[k]) add(l.spans, s.text, s.style);
      out.push_back(std::move(l));
    }
  };
  // A line inside the table whose bottom border is ls[b]: its indent and
  // outer border, spans in between, padded to its width.
  auto boxed = [&](size_t b, std::vector<Span> spans, Part part = Part::Table) {
    int inner = inner_of(ls[b]);
    Line l;
    l.part = part;
    l.test = ls[b].test;
    l.spans = {{std::string(kGutter, ' '), 0}};
    add(l.spans, prefix_of(ls[b]), Dim);  // a quote's bar, a list's indent
    add(l.spans, "│", Dim);
    int used = 0;
    for (auto& s : spans) {
      std::string c = cut(s.text, inner - used);
      add(l.spans, c, s.style);
      used += cells(c);
    }
    add(l.spans, std::string(static_cast<size_t>(std::max(0, inner - used)), ' '), 0);
    add(l.spans, "│", Dim);
    return l;
  };
  // Notes inside a table, under a row: a line each, wrapped, indented.
  auto notes = [&](size_t b, const std::string& text, unsigned style, unsigned tinted) {
    for (auto& w : wrap(spans_of(text, style), std::max(4, inner_of(ls[b]) - 4))) {
      std::vector<Span> sp = {{"   ", 0}};
      sp.insert(sp.end(), w.begin(), w.end());
      Line l = boxed(b, sp);
      l.tint = tinted;
      out.push_back(std::move(l));
    }
  };

  for (auto* m : top) {
    if (!m->banner) {
      lines_of(spans_of(m->text, 0), gutter({m}), 0);
      continue;
    }
    auto wrapped = wrap(spans_of(m->text, Error), std::max(8, width - kGutter - 2));
    for (size_t k = 0; k < wrapped.size(); ++k) {
      Line l;
      l.tint = Error;
      l.spans = {k ? Span{std::string(kGutter, ' '), 0} : gutter({m}), {" ", 0}};
      for (auto& s : wrapped[k]) add(l.spans, s.text, s.style | Bold);
      add(l.spans, std::string(static_cast<size_t>(std::max(0, width - cells(text_of(l.spans)))), ' '), 0);
      out.push_back(std::move(l));
    }
  }
  if (!top.empty() && !lines.empty()) out.emplace_back();
  for (size_t i = 0; i < ls.size(); ++i) {
    Line l = ls[i];
    l.spans.insert(l.spans.begin(), gutter(on[i]));
    l.tint = tint[i];
    if (auto f = footer.find(i); f != footer.end()) {  // the table's result inside its border
      Line sep = l;
      for (auto& s : sep.spans) {
        if (size_t at = s.text.find("└"); at != std::string::npos) s.text.replace(at, std::string("└").size(), "├");
        if (size_t at = s.text.rfind("┘"); at != std::string::npos) s.text.replace(at, std::string("┘").size(), "┤");
      }
      sep.part = Part::Table;
      out.push_back(std::move(sep));
      for (auto* m : f->second) {
        std::istringstream in(m->text);
        std::string head;
        std::getline(in, head);
        out.push_back(boxed(i, {{" " + glyph(*m) + (head.empty() ? "" : " " + head), level_style(m->level)}}));
        std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (rest.find_first_not_of(" \n") != std::string::npos) notes(i, rest, m->level == "error" ? Note : Dim, 0);
      }
      Line bottom = boxed(i, {}, Part::TableEnd);
      bottom.spans = {{std::string(kGutter, ' '), 0}, {prefix_of(ls[i]) + "└" + repeat("─", inner_of(ls[i])) + "┘", Dim}};
      out.push_back(std::move(bottom));
      continue;
    }
    const Line& src = ls[i];
    out.push_back(std::move(l));
    auto it = after.find(i);
    if (it == after.end()) continue;
    if (src.part == Part::Table) {
      if (ls[bottom_of[i]].part == Part::TableEnd)
        for (auto* m : it->second) notes(bottom_of[i], m->text, m->level == "error" ? Note : Dim, tint[i]);
    } else if (src.part == Part::Badge) {  // on the badge if it fits, else under it
      std::vector<Span> extra;
      for (auto* m : it->second) {
        extra.push_back({" · ", Dim});
        extra.push_back({m->text, level_style(m->level)});
      }
      Line& b = out.back();
      bool fits = cells(text_of(b.spans)) + cells(text_of(extra)) <= width;
      if (fits) b.spans.insert(b.spans.end(), extra.begin(), extra.end());
      else
        for (auto* m : it->second)  // under the fixture's name, past "🧪 "
          lines_of(spans_of(m->text, level_style(m->level)), {std::string(kGutter + 3, ' '), 0}, 3);
    } else {
      for (auto* m : it->second) {
        std::vector<Span> sp = {{glyph(*m), level_style(m->level)}, {" line " + std::to_string(m->line) + " · ", Dim}};
        for (auto& s : spans_of(m->text, 0)) sp.push_back(s);
        lines_of(sp, {std::string(kGutter, ' '), 0}, 2);
      }
    }
  }
  return out;
}

std::vector<Count> tally(const std::vector<Mark>& marks, const std::vector<Directive>& dirs) {
  std::set<int> sums = {0};
  for (auto& d : dirs) sums.insert(d.line);
  std::vector<Count> out;
  for (const char* level : {"ok", "error", "warn", "dim"})
    for (auto& m : marks) {
      if (m.level != level || sums.count(m.line)) continue;
      std::string g = glyph(m);
      auto it = std::find_if(out.begin(), out.end(), [&](const Count& c) { return c.glyph == g && c.style == level_style(level); });
      if (it == out.end()) out.push_back({g, level_style(level), 1});
      else ++it->n;
    }
  return out;
}

std::vector<Line> render(const std::string& markdown, int width, const std::string& today) {
  std::string doc = printable(markdown);
  Renderer r(doc, std::max(10, width), today);
  MD_PARSER p{};
  p.flags = MD_DIALECT_GITHUB;
  p.enter_block = [](MD_BLOCKTYPE t, void* d, void* u) { return call(u, [&](Renderer& r) { r.enter_block(t, d); }); };
  p.leave_block = [](MD_BLOCKTYPE t, void*, void* u) { return call(u, [&](Renderer& r) { r.leave_block(t); }); };
  p.enter_span = [](MD_SPANTYPE t, void* d, void* u) { return call(u, [&](Renderer& r) { r.enter_span(t, d); }); };
  p.leave_span = [](MD_SPANTYPE t, void*, void* u) { return call(u, [&](Renderer& r) { r.leave_span(t); }); };
  p.text = [](MD_TEXTTYPE t, const MD_CHAR* s, MD_SIZE n, void* u) {
    return call(u, [&](Renderer& r) { r.text(t, s, n); });
  };
  md_parse(doc.data(), static_cast<MD_SIZE>(doc.size()), &p, &r);
  r.badges_before(INT_MAX);  // directives after the last block
  return std::move(r.out);
}

std::string plain(const Line& l) {
  std::string s;
  for (auto& sp : l.spans) s += sp.text;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

}  // namespace anton::md
