// The reader (a PR's description and markdown files, with plugin marks and
// an outline) and the note overlay.
#include <algorithm>
#include <thread>

#include <ftxui/screen/string.hpp>

#include "github.hpp"
#include "ui_impl.hpp"

namespace anton {

using namespace ftxui;

namespace {

Element md_span(const std::string& s, unsigned style) {
  Element e = text(s);
  if (style & md::Bold) e = e | bold;
  if (style & md::Italic) e = e | italic;
  if (style & md::Strike) e = e | strikethrough;
  if (style & md::Link) e = e | underlined;
  if (style & md::Code) e = e | color(pal(Pal::Yellow));
  if (style & md::Output) e = e | color(pal(Pal::Cyan));
  if (style & md::Ok) e = e | color(pal(Pal::Green));
  if (style & md::Error) e = e | color(pal(Pal::Red));
  if (style & md::Warn) e = e | color(pal(Pal::Yellow));
  if (style & md::Dim) e = e | color(pal(Pal::Dim));
  return e;
}

Pal style_pal(unsigned style) {
  return style & md::Ok ? Pal::Green : style & md::Error ? Pal::Red : style & md::Warn ? Pal::Yellow : Pal::Dim;
}

int rank(const std::string& level) { return level == "error" ? 3 : level == "warn" ? 2 : level == "ok" ? 1 : 0; }

std::string trim_left(std::string s) {
  s.erase(0, s.find_first_not_of(' '));
  return s;
}

}  // namespace

// A rendered markdown line as one row. Wrapped by md::render rather than
// by paragraph(), so every element is one row and scrolling reaches the end.
Element md_line(const md::Line& l) {
  Elements parts;
  for (auto& sp : l.spans) parts.push_back(md_span(sp.text, sp.style));
  Element e = parts.empty() ? text("") : hbox(std::move(parts));
  if (l.heading == 1) return e | color(pal(Pal::Accent)) | bold;
  if (l.heading == 2) return e | color(pal(Pal::Cyan)) | bold;
  if (l.heading > 2) return e | color(pal(Pal::Cyan));
  return e;
}

// j/k, space/PgDn, PgUp, g/G on a page `rows` tall; false for any other
// key. The renderer clamps.
bool scroll_key(const Event& e, int& scroll, int rows) {
  if (e == Event::Character('j') || e == Event::ArrowDown) ++scroll;
  else if (e == Event::Character('k') || e == Event::ArrowUp) --scroll;
  else if (e == Event::PageDown || e == Event::Character(' ')) scroll += rows;
  else if (e == Event::PageUp) scroll -= rows;
  else if (e == Event::Character('g') || e == Event::Home) scroll = 0;
  else if (e == Event::Character('G') || e == Event::End) scroll = 1 << 30;
  else return false;
  return true;
}

// A plugin's note, usually markdown (an LLM review): rendered like a
// description, each note under its plugin's name when there are several.
Element Ui::render_note() {
  auto notes = notes_of(note_url);
  auto term = Terminal::Size();
  int w = std::min(100, term.dimx - 4), h = term.dimy - 4;
  int tw = std::max(10, w - 2), th = std::max(1, h - 3);  // inside the border, above the status line
  std::string key;
  for (auto& [name, body] : notes) key += name + '\0' + body + '\0';
  if (note_md.key != key || note_md.width != tw) {
    note_md = {key, tw, {}};
    for (auto& [name, body] : notes) {
      if (!note_md.lines.empty()) note_md.lines.emplace_back();
      if (notes.size() > 1) note_md.lines.push_back({{{"[" + name + "]", md::Bold}}, 0, 2});
      for (auto& l : md::render(body, tw, today())) note_md.lines.push_back(std::move(l));
    }
  }
  int n = static_cast<int>(note_md.lines.size());
  note_scroll = std::clamp(note_scroll, 0, std::max(0, n - th));
  Elements shown;
  if (notes.empty()) shown.push_back(txt("(the note is gone)", Pal::Dim));
  for (int i = note_scroll; i < std::min(n, note_scroll + th); ++i) shown.push_back(md_line(note_md.lines[static_cast<size_t>(i)]));
  std::string where = n <= th ? "" : std::to_string(100 * std::min(n, note_scroll + th) / n) + "%   ";
  std::string title = note_url;
  if (auto c = cache.find(note_url); c != cache.end())
    title = c->second.repo + "#" + std::to_string(c->second.number) + "  " + shown_title(c->second.title);
  Element body = vbox(std::move(shown)) | flex;
  hit(body).wheel = [this](int dir) { note_scroll += 3 * dir; };
  Element close = button("esc", "Close");
  close = clickable(close, [this] { mode = Mode::Normal; });
  return window(text(" " + title + " "),
                vbox({body, hbox({txt(where + "j/k scroll  g/G top/end  y copy  ", Pal::Dim), filler(), close})})) |
         bg(Pal::Menu) | size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h) | clear_under | center;
}

bool Ui::note_key(const Event& e) {
  if (e == Event::Escape || e == Event::Character('q') || e == Event::Character('n')) {
    mode = Mode::Normal;
  } else if (scroll_key(e, note_scroll, 10)) {
  } else if (e == Event::Character('y')) {
    std::string all;
    for (auto& [name, body] : notes_of(note_url)) all += (all.empty() ? "" : "\n\n") + body;
    if (all.empty()) flash("the note is gone");
    else copy(all);
  }
  return true;
}

// -- reader ---------------------------------------------------------------------

// Fetches the description off the UI thread, unless it's in for this
// updatedAt already or on its way.
void Ui::want_body(const std::string& url, std::time_t updated) {
  Body& b = bodies[url];
  if (b.loading || b.updated == updated) return;
  b.loading = true;
  std::thread([this, url, updated] {
    std::string err, head, body = fetch_body(url, head, err);
    post([this, url, updated, err, head, body] {
      Body& b = bodies[url];
      b.loading = false;
      b.updated = updated;  // a failure too, or it'd retry every frame; reopening retries
      b.error = err;
      if (err.empty()) {
        b.text = body;
        b.head = head;
      }
    });
  }).detach();
}

// The PR's file tabs at the head its description came with, fetched off
// the UI thread on first ask. nullptr until the head is known.
Ui::Files* Ui::want_files(const std::string& url) {
  std::string head = bodies[url].head;
  if (head.empty()) return nullptr;
  auto key = std::make_pair(url, head);
  auto [it, fresh] = files.try_emplace(key);
  if (!fresh) return &it->second;
  it->second.loading = true;
  std::thread([this, key] {
    std::string err;
    std::vector<FileTab> tabs;
    for (auto& f : fetch_md_files(key.first, key.second, err)) {
      FileTab t{f.path, "", f.text, f.error, md::directives(f.text)};
      t.spec = std::any_of(t.dirs.begin(), t.dirs.end(), [](const md::Directive& d) { return !d.import; });
      tabs.push_back(std::move(t));
    }
    std::stable_partition(tabs.begin(), tabs.end(), [](const FileTab& t) { return t.spec; });
    std::vector<std::string> paths;
    for (auto& t : tabs) paths.push_back(t.path);
    auto names = short_names(paths);
    for (size_t i = 0; i < tabs.size(); ++i) tabs[i].name = names[i];
    post([this, key, tabs, err] { files[key] = Files{tabs, err, false}; });
  }).detach();
  return &it->second;
}

const Ui::Files* Ui::reader_files() const {
  auto b = bodies.find(reader_url);
  if (b == bodies.end()) return nullptr;
  auto f = files.find({reader_url, b->second.head});
  return f == files.end() ? nullptr : &f->second;
}

// Every plugin's marks on one file of a PR (or one plugin's), and, in
// stale, a line for each plugin whose marks are about another commit.
std::vector<Mark> Ui::marks_on(const std::string& url, const std::string& path, const std::string& head,
                               std::vector<std::string>* stale, int only_plugin) const {
  std::vector<Mark> out;
  for (size_t i = 0; i < live.size(); ++i) {
    if (only_plugin >= 0 && static_cast<int>(i) != only_plugin) continue;
    auto it = live[i].prs.find(url);
    if (it == live[i].prs.end()) continue;
    size_t before = out.size();
    for (auto& m : it->second.marks)
      if (m.file == path) out.push_back(m);
    const std::string& ref = it->second.marks_ref;
    if (stale && out.size() > before && !ref.empty() && !head.starts_with(ref))  // a short sha is fine
      stale->push_back(cfg.plugins[i].name + ": results are for " + ref.substr(0, 7) + "; the PR has moved on");
  }
  return out;
}

void Ui::open_reader(const Pr& p) {
  reader_url = p.url;
  reader_tab = 0;
  reader_scroll.clear();
  mode = Mode::Reader;
  close_menus();
  Body& b = bodies[p.url];
  if (!b.error.empty()) b.updated = -1;
  if (auto f = files.find({p.url, b.head}); f != files.end() && !f->second.loading && !f->second.error.empty())
    files.erase(f);  // reopening retries
}

int Ui::reader_rows() const { return std::max(1, Terminal::Size().dimy - 6); }  // header 4, footer 2

// Full screen: ← back, the PR, its tabs, then the description or a file as
// rendered markdown (a file with every plugin's marks), and on a wide
// terminal an outline: headings, temoc tables with their result, the run.
Element Ui::render_reader() {
  auto term = Terminal::Size();
  int W = term.dimx;
  const int ow = W >= 120 ? 34 : 0;  // the outline pane
  int pane = W - (ow ? ow + 1 : 0);   // content, its margin and scrollbar
  int w = std::max(10, std::min(120, pane - 2 - md::kGutter));

  const Pr* p = nullptr;
  if (auto c = cache.find(reader_url); c != cache.end()) {
    p = &c->second;
    want_body(p->url, p->updated);
  }
  const Body& b = bodies[reader_url];
  const Files* fs = want_files(reader_url);
  size_t ntabs = 1 + (fs ? fs->tabs.size() : 0);
  reader_tab = std::min(reader_tab, ntabs - 1);
  const FileTab* ft = reader_tab ? &fs->tabs[reader_tab - 1] : nullptr;
  std::string day = today(), msg = b.text.empty() ? "" : b.error;  // a failed refetch keeps the old text
  int& scroll = reader_scroll[ft ? ft->path : ""];

  // -- the text ----------------------------------------------------------------
  std::vector<Mark> marks;
  std::vector<std::string> stale;
  Elements status;  // instead of the text: loading, an error, empty
  auto gutter = [](std::vector<md::Line> doc) {
    for (auto& l : doc) l.spans.insert(l.spans.begin(), {std::string(md::kGutter, ' '), 0});
    return doc;
  };
  if (!ft) {
    std::string key = '\0' + day + '\0' + b.text;
    if (reader_md.key != key || reader_md.width != w) reader_md = {key, w, gutter(md::render(b.text, w, day))};
    if (b.text.empty() && b.loading) status.push_back(txt("loading…", Pal::Dim));
    else if (b.text.empty() && !b.error.empty()) status.push_back(txt("couldn't load the description: " + b.error, Pal::Red));
    else if (reader_md.lines.empty()) status.push_back(txt("No description.", Pal::Dim));
  } else {
    marks = marks_on(reader_url, ft->path, b.head, &stale);
    std::string key = ft->path + '\0' + day + '\0' + ft->text;
    for (auto& m : marks) key += '\0' + std::to_string(m.line) + m.level + '\0' + m.glyph + '\0' + m.text;
    for (auto& s : stale) key += '\0' + s;
    if (reader_md.key != key || reader_md.width != w) {
      std::vector<md::Line> lines;
      for (auto& s : stale)
        for (auto& l : md::render(s, w)) {
          for (auto& sp : l.spans) sp.style |= md::Warn;
          lines.push_back(std::move(l));
        }
      if (!stale.empty()) lines.emplace_back();
      lines = gutter(lines);
      auto doc = md::render(ft->text, w, day);
      doc = marks.empty() ? gutter(doc) : md::annotate(doc, marks, w + md::kGutter);
      lines.insert(lines.end(), doc.begin(), doc.end());
      reader_md = {key, w, std::move(lines)};
    }
    if (!ft->error.empty()) status.push_back(txt("couldn't load " + ft->path + ": " + ft->error, Pal::Red));
    else if (reader_md.lines.empty()) status.push_back(txt("Empty file.", Pal::Dim));
  }
  const auto& lines = reader_md.lines;
  int rows = reader_rows(), n = status.empty() ? static_cast<int>(lines.size()) : 0;
  scroll = std::clamp(scroll, 0, std::max(0, n - rows));
  auto jump = [this, key = ft ? ft->path : std::string()](int src) {
    for (size_t i = 0; i < reader_md.lines.size(); ++i)
      if (reader_md.lines[i].src >= src && reader_md.lines[i].src > 0) {
        reader_scroll[key] = static_cast<int>(i);
        return;
      }
  };

  // -- the outline ---------------------------------------------------------------
  Elements outline;
  if (ow) {
    struct Entry {
      int src;
      std::string glyph, label;
      Pal gp;
    };
    std::vector<Entry> es;
    std::set<int> seen_src;
    for (auto& l : lines)
      if (l.heading > 0 && l.heading <= 3 && l.src > 0 && seen_src.insert(l.src).second)
        es.push_back({l.src, "", trim_left(md::plain(l)), Pal::Fg});
    if (ft)
      for (auto& d : ft->dirs) {
        if (d.import) continue;
        const Mark* worst = nullptr;
        for (auto& m : marks)
          if (m.line == d.line && (!worst || rank(m.level) > rank(worst->level))) worst = &m;
        Pal gp = !worst ? Pal::Dim : worst->level == "error" ? Pal::Red : worst->level == "warn" ? Pal::Yellow
                 : worst->level == "ok"                      ? Pal::Green
                                                             : Pal::Dim;
        es.push_back({d.line, worst ? worst->glyph : "🧪", d.target.substr(0, d.target.find('#')), gp});
      }
    std::stable_sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.src < b.src; });
    int top_src = 0;
    for (int i = scroll; i < n && !top_src; ++i) top_src = lines[static_cast<size_t>(i)].src;
    size_t here = 0;
    for (size_t i = 0; i < es.size(); ++i)
      if (es[i].src <= top_src) here = i;
    outline.push_back(txt(" ON THIS PAGE", Pal::Dim, true));
    for (size_t i = 0; i < es.size() && static_cast<int>(outline.size()) < rows - 8; ++i) {
      const Entry& e = es[i];
      Element row = e.glyph.empty()
                        ? txt(" " + e.label, Pal::Fg)
                        : hbox({text("  "), txt(e.glyph, e.gp), text(string_width(e.glyph) == 1 ? "  " : " "), txt(e.label)});
      row = row | size(WIDTH, EQUAL, ow);
      if (i == here && es.size() > 1) row = row | bg(Pal::Chip);
      outline.push_back(clickable(row, [jump, src = e.src] { jump(src); }));
    }
    if (ft)  // a block per plugin with marks here: its counts, the commit, its keys
      for (size_t i = 0; i < live.size(); ++i) {
        auto mine = marks_on(reader_url, ft->path, b.head, nullptr, static_cast<int>(i));
        if (mine.empty()) continue;
        std::string name = cfg.plugins[i].name, ref = live[i].prs.at(reader_url).marks_ref;
        for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        outline.push_back(text(""));
        outline.push_back(txt(" " + name, Pal::Dim, true));
        Elements counts = {text(" ")};
        for (auto& c : md::tally(mine, ft->dirs)) counts.push_back(txt(c.glyph + " " + std::to_string(c.n) + "  ", style_pal(c.style)));
        outline.push_back(hbox(std::move(counts)));
        if (!ref.empty()) {
          outline.push_back(txt(" ran on " + ref.substr(0, 7), Pal::Dim));
          outline.push_back(txt(b.head.starts_with(ref) ? " the PR head, results current" : " the PR has moved on since",
                                b.head.starts_with(ref) ? Pal::Dim : Pal::Yellow));
        }
        for (auto& [pi, a] : plugin_keys())
          if (pi == i) {
            outline.push_back(text(""));
            outline.push_back(hbox({text(" "), clickable(button(a.key, a.button.empty() ? a.label : a.button),
                                                         [this, key = a.key] { plugin_action(key, reader_url); })}));
          }
      }
    if (cfg.mouse && es.size() > 1) {
      outline.push_back(text(""));
      outline.push_back(txt(" click a line to jump", Pal::Faint));
    }
  }

  // -- the page ------------------------------------------------------------------
  int thumb = n > rows ? std::max(1, rows * rows / n) : 0;
  int thumb_at = n > rows ? scroll * (rows - thumb) / std::max(1, n - rows) : 0;
  Elements body;
  for (int r = 0; r < rows; ++r) {
    Element left = r < static_cast<int>(status.size()) ? hbox({text("    "), status[static_cast<size_t>(r)]})
                   : status.empty() && scroll + r < n ? hbox({text(" "), md_line(lines[static_cast<size_t>(scroll + r)])})
                                                      : text("");
    Elements row = {left | size(WIDTH, EQUAL, pane - 1),
                    n > rows ? txt(r >= thumb_at && r < thumb_at + thumb ? "┃" : "│", r >= thumb_at && r < thumb_at + thumb ? Pal::Dim : Pal::Faint)
                             : text(" ")};
    if (ow) {
      row.push_back(txt("│", Pal::Faint));
      row.push_back(r < static_cast<int>(outline.size()) ? outline[static_cast<size_t>(r)] : text(""));
    }
    body.push_back(hbox(std::move(row)));
  }
  Element page = vbox(std::move(body));
  hit(page).wheel = [this, key = ft ? ft->path : std::string()](int dir) { reader_scroll[key] += 3 * dir; };

  // -- header: back, the PR, where this text is from, the tabs ----------------------
  std::string where = (ft ? ft->path + " @ " + b.head.substr(0, 7) : reader_url) + " ";
  std::vector<Part> parts;
  if (p) {
    parts.push_back({who(p->author), Pal::Fg, person(p->author)});
    parts.push_back({" · opened " + age(static_cast<long>(now() - p->created)) + " ago · ", Pal::Dim});
    Band band = size_band(p->additions + p->deletions, cfg.green_below, cfg.red_from);
    parts.push_back({"+" + std::to_string(p->additions) + " −" + std::to_string(p->deletions),
                     band == Band::Red ? Pal::Red : band == Band::Yellow ? Pal::Yellow : Pal::Green});
    parts.push_back({" · " + derive_state(*p, status_of(p->url)).text, Pal::Dim});
    for (auto& t : tags_of(p->url)) {
      parts.push_back({" · ", Pal::Dim});
      parts.push_back({(t.glyph.empty() ? "" : t.glyph + " ") + t.text, Pal::Fg, tag_person(t)});
    }
  }
  Element meta = hbox({text(" "), fit(parts, W - string_width(where) - 4), filler(), txt(where, Pal::Faint)});

  std::vector<std::vector<md::Span>> tabs_spans = {{{" Description ", 0}}};
  static const std::vector<FileTab> none;
  for (auto& t : fs ? fs->tabs : none) {
    std::vector<md::Span> sp = {{" " + std::string(t.spec ? "🧪 " : "") + t.name + " ", 0}};
    for (auto& c : md::tally(marks_on(reader_url, t.path, b.head), t.dirs))
      sp.push_back({c.glyph + " " + std::to_string(c.n) + " ", c.style});
    tabs_spans.push_back(std::move(sp));
  }
  std::vector<int> widths;
  for (auto& t : tabs_spans) {
    int k = 1;  // the space after it
    for (auto& s : t) k += string_width(s.text);
    widths.push_back(k);
  }
  int room = pane - 18;  // ‹ › and "Tab next file"
  size_t start = 0;
  int total = 0;
  for (size_t i = 0; i <= reader_tab && i < tabs_spans.size(); ++i) total += widths[i];
  while (start < reader_tab && total > room) total -= widths[start++];
  Elements tabrow;
  int x = 0, active_x = 0, active_w = 0;
  if (start > 0) {
    tabrow.push_back(txt("‹", Pal::Accent));
    x += 1;
  }
  for (size_t i = start; i < tabs_spans.size(); ++i) {
    if (i > reader_tab && x + widths[i] > room) {
      tabrow.push_back(txt("›", Pal::Accent));
      break;
    }
    bool on = i == reader_tab;
    Elements parts;
    for (auto& s : tabs_spans[i]) parts.push_back(s.style ? md_span(s.text, s.style) : txt(s.text, on ? Pal::Fg : Pal::Dim, on));
    Element tab = hbox(std::move(parts));
    if (on) {
      tab = truecolor() ? tab | bg(Pal::Sel) : tab | inverted;
      active_x = x;
      active_w = widths[i] - 1;
    }
    tabrow.push_back(clickable(tab, [this, i] { reader_tab = i; }));
    tabrow.push_back(text(" "));
    x += widths[i];
  }
  if (fs && fs->loading) tabrow.push_back(txt(" loading files…", Pal::Dim));
  if (fs && !fs->error.empty()) tabrow.push_back(txt(" files: " + fs->error, Pal::Red));
  tabrow.push_back(filler());
  if (ntabs > 1) {
    tabrow.push_back(txt("Tab", Pal::Accent, true));
    tabrow.push_back(txt(" next file ", Pal::Faint));
  }
  auto dash = [](int k) {
    std::string s;
    for (int i = 0; i < k; ++i) s += "─";
    return s;
  };
  std::string over;
  for (int i = 0; i < active_w; ++i) over += "▔";
  Elements under = {txt(dash(active_x), Pal::Faint), txt(over, Pal::Accent), txt(dash(std::max(0, pane - active_x - active_w)), Pal::Faint)};
  if (ow) under.push_back(txt("┬" + dash(ow), Pal::Faint));

  Element back = clickable(button("", "← back"), [this] { mode = Mode::Normal; });
  Element head = hbox({back, txt("  " + (p ? p->repo.substr(p->repo.find('/') + 1) + "#" + std::to_string(p->number) : std::string()) + "  ", Pal::Dim),
                       txt(p ? shown_title(p->title) : reader_url, Pal::Fg, true)});

  // -- footer: buttons, then where you are or what just happened -------------------------
  Elements foot = {text(" ")};
  std::vector<std::pair<std::string, std::string>> btns = {{"esc", "Back"}};
  if (ntabs > 1) btns.push_back({"Tab", "Next file"});
  for (auto& [i, a] : plugin_keys()) btns.push_back({a.key, a.button.empty() ? a.label : a.button});
  btns.push_back({"w", "Browser"});
  btns.push_back({"y", "Copy URL"});
  int full = 34;  // with "lines a–b of N · wheel scrolls"
  for (auto& [k, l] : btns) full += string_width(k) + string_width(l) + 4;
  bool labels = full <= W;
  for (auto& [k, l] : btns) {
    std::string key = k;
    foot.push_back(clickable(button(k, labels ? l : ""), [this, key] {
      if (key == "esc") mode = Mode::Normal;
      else if (key == "Tab") reader_key(Event::Tab);
      else if (key == "w") open(reader_url);
      else if (key == "y") copy(reader_url);
      else plugin_action(key, reader_url);
    }));
    foot.push_back(text(" "));
  }
  foot.push_back(filler());
  if (now() < notice_until && !notice.empty()) foot.push_back(txt(notice + " ", notice_bad ? Pal::Red : Pal::Cyan));
  else if (!msg.empty()) foot.push_back(txt(msg + " ", Pal::Red));
  else if (n > rows)
    foot.push_back(hbox({txt("lines " + std::to_string(scroll + 1) + "–" + std::to_string(std::min(n, scroll + rows)) + " of " + std::to_string(n) + " ", Pal::Dim),
                         txt(cfg.mouse ? "· wheel scrolls " : "", Pal::Faint)}));

  return vbox({head, meta, hbox(std::move(tabrow)), hbox(std::move(under)), page | flex, rule(),
               hbox(std::move(foot))});
}

bool Ui::reader_key(const Event& e) {
  const Files* fs = reader_files();
  size_t n = 1 + (fs ? fs->tabs.size() : 0);
  reader_tab = std::min(reader_tab, n - 1);
  std::string path = reader_tab ? fs->tabs[reader_tab - 1].path : "";
  if (e == Event::Escape || e == Event::Character('q')) mode = Mode::Normal;
  else if (e == Event::Tab || e == Event::Character(']')) reader_tab = (reader_tab + 1) % n;
  else if (e == Event::TabReverse || e == Event::Character('[')) reader_tab = (reader_tab + n - 1) % n;
  else if (scroll_key(e, reader_scroll[path], reader_rows())) {
  } else if (e == Event::Character('w') || e == Event::Return) open(reader_url);
  else if (e == Event::Character('y')) copy(reader_url);
  else if (e.is_character()) plugin_action(e.character(), reader_url);  // T: run this PR's specs
  return true;
}

}  // namespace anton
