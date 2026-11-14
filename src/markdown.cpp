#include "markdown.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <set>

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

struct Renderer {
  const std::string& doc;
  int width;
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

  Renderer(const std::string& d, int w) : doc(d), width(w) {
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

  void emit(std::vector<Span> spans, int src, int head = 0) {
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
    std::vector<int> w(ncols, 1);
    for (auto& r : rows)
      for (size_t c = 0; c < r.cells.size(); ++c) w[c] = std::max(w[c], cells(text_of(r.cells[c])));
    int total = 1;
    for (int x : w) total += x + 3;
    while (total > avail()) {
      auto widest = std::max_element(w.begin(), w.end());
      if (*widest <= 2) break;
      --*widest;
      --total;
    }
    auto border = [&](const char* l, const char* m, const char* r) {
      std::string s = l;
      for (size_t c = 0; c < ncols; ++c) s += repeat("─", w[c] + 2) + (c + 1 < ncols ? m : r);
      emit({{s, Dim}}, 0);
    };
    bool tall = false;  // a wrapped row: separate every row, or they blur
    std::vector<std::vector<std::vector<std::vector<Span>>>> wrapped;
    for (auto& r : rows) {
      wrapped.emplace_back();
      for (size_t c = 0; c < ncols; ++c) {
        std::vector<Span> cell = c < r.cells.size() ? r.cells[c] : std::vector<Span>{};
        if (r.head)
          for (auto& s : cell) s.style |= Bold;
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
        emit(std::move(line), rows[ri].src);
      }
      bool last = ri + 1 == rows.size();
      if (!last && (rows[ri].head != rows[ri + 1].head || tall)) border("├", "┼", "┤");
    }
    border("└", "┴", "┘");
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

std::vector<Line> render(const std::string& markdown, int width) {
  std::string doc = printable(markdown);
  Renderer r(doc, std::max(10, width));
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
  return std::move(r.out);
}

std::string plain(const Line& l) {
  std::string s;
  for (auto& sp : l.spans) s += sp.text;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

}  // namespace anton::md
