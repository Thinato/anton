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
  if (style & md::Note) e = e | color(pal(Pal::Note));
  if (style & md::Dim) e = e | color(pal(Pal::Dim));
  return e;
}

Pal style_pal(unsigned style) {
  return style & md::Ok ? Pal::Green : style & md::Error ? Pal::Red : style & md::Warn ? Pal::Yellow : Pal::Dim;
}

Pal level_pal(const std::string& level) {
  return level == "ok" ? Pal::Green : level == "error" ? Pal::Red : level == "warn" ? Pal::Yellow : Pal::Dim;
}

// A row result's background: green, red, yellow.
Decorator tint(unsigned style) {
  return bg(style & md::Ok ? Pal::TintOk : style & md::Error ? Pal::TintErr : Pal::TintWarn);
}

int rank(const std::string& level) { return level == "error" ? 3 : level == "warn" ? 2 : level == "ok" ? 1 : 0; }

std::string trim_left(std::string s) {
  s.erase(0, s.find_first_not_of(' '));
  return s;
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

}  // namespace

// A rendered markdown line as one row. Wrapped by md::render rather than
// by paragraph(), so every element is one row and scrolling reaches the end.
Element md_line(const md::Line& l) {
  Elements parts;
  for (auto& sp : l.spans) parts.push_back(md_span(sp.text, sp.style));
  Element e = parts.empty() ? text("") : hbox(std::move(parts));
  if (l.tint) return e | tint(l.tint);
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

const Ui::FileTab* Ui::reader_tab_file() const {
  const Files* fs = reader_files();
  return fs && reader_tab > 0 && reader_tab <= fs->tabs.size() ? &fs->tabs[reader_tab - 1] : nullptr;
}

std::optional<Ui::PluginKey> Ui::line_key() const {
  for (auto& k : plugin_keys())
    if (k.action.line) return k;
  return std::nullopt;
}

// Runs a test table (its directive's line) or row of a file through the
// first plugin action that takes a line.
void Ui::run_line(const std::string& file, int line) {
  if (auto k = line_key()) plugin_action(k->action.key, reader_url, file, line);
  else flash("no plugin runs test tables (02-plugins.md, \"line\" actions)");
}

void Ui::reader_jump(const std::string& path, int src) {
  for (size_t i = 0; i < reader_md.lines.size(); ++i)
    if (reader_md.lines[i].src >= src && reader_md.lines[i].src > 0) {
      reader_scroll[path] = static_cast<int>(i);
      return;
    }
}

// The file's test tables in order; by +1/-1 from the focused one, stopping
// at the ends, and the page scrolls to it.
void Ui::focus_table(int by) {
  const FileTab* ft = reader_tab_file();
  std::vector<int> tests;
  for (auto& d : ft ? ft->dirs : std::vector<md::Directive>{})
    if (!d.import) tests.push_back(d.line);
  if (tests.empty()) return flash("no test tables in this file");
  int& f = reader_focus[ft->path];
  auto it = std::find(tests.begin(), tests.end(), f);
  long at = it == tests.end() ? 0 : std::clamp<long>(it - tests.begin() + by, 0, static_cast<long>(tests.size()) - 1);
  f = tests[static_cast<size_t>(at)];
  reader_jump(ft->path, f);
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
    for (auto& m : marks)
      key += '\0' + std::to_string(m.line) + m.level + (m.banner ? "!" : "") + '\0' + m.glyph + '\0' + m.text;
    for (auto& s : stale) key += '\0' + s;
    if (reader_md.key != key || reader_md.width != w) {
      std::vector<md::Line> lines;
      for (auto& s : stale)
        for (auto& l : md::render(s, w)) {
          for (auto& sp : l.spans) sp.style |= md::Warn;
          l.src = 0;  // not the file's lines: jumps and marks skip it
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
  const std::string path = ft ? ft->path : "";
  auto jump = [this, path](int src) { reader_jump(path, src); };

  // Test tables: their directives, the focused one (J/K, a click), and the
  // plugin action that runs a table or row (▶, r).
  std::vector<const md::Directive*> tests;
  if (ft)
    for (auto& d : ft->dirs)
      if (!d.import) tests.push_back(&d);
  int focus = 0;
  if (!tests.empty()) {
    int& f = reader_focus[path];
    if (std::none_of(tests.begin(), tests.end(), [&](const md::Directive* d) { return d->line == f; })) f = tests[0]->line;
    focus = f;
  }
  const auto runner = line_key();
  // A test table's name, as temoc-ui gives it: the nearest heading above it
  // (below the table before), else its fixture.
  auto table_name = [&](const md::Directive* d) {
    int after = 0;
    for (auto* t : tests)
      if (t->line < d->line) after = t->line;
    std::string name;
    int seen = 0;
    for (auto& l : lines)
      if (l.heading > 0 && l.src > after && l.src < d->line && l.src != seen) {  // a wrapped heading's first line
        seen = l.src;
        name = trim_left(md::plain(l));
        name = trim_left(name.substr(std::min(name.size(), name.find_first_not_of('#'))));
      }
    return name.empty() ? d->target.substr(0, d->target.find('#')) : name;
  };

  // -- the outline ---------------------------------------------------------------
  // The headings (levels 1-3), for ON THIS PAGE.
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
  int top_src = 0;
  for (int i = scroll; i < n && !top_src; ++i) top_src = lines[static_cast<size_t>(i)].src;
  Elements outline;
  auto headings = [&](size_t room) {
    size_t here = 0;
    for (size_t i = 0; i < es.size(); ++i)
      if (es[i].src <= top_src) here = i;
    for (size_t i = 0; i < es.size() && i < room; ++i) {
      const Entry& e = es[i];
      Element row = e.glyph.empty()
                        ? txt(" " + e.label, Pal::Fg)
                        : hbox({text("  "), txt(e.glyph, e.gp), text(string_width(e.glyph) == 1 ? "  " : " "), txt(e.label)});
      row = row | size(WIDTH, EQUAL, ow);
      if (i == here && es.size() > 1) row = row | bg(Pal::Chip);
      outline.push_back(clickable(row, [jump, src = e.src] { jump(src); }));
    }
  };
  if (ow && ft && ft->spec) {  // temoc-ui's side panel: the file, its tables, what didn't pass, the command
    auto worst_on = [&](int line) {
      const Mark* w = nullptr;
      for (auto& m : marks)
        if (m.line == line && !m.banner && (!w || rank(m.level) > rank(w->level))) w = &m;
      return w;
    };
    auto head = [&](const std::string& s) {
      if (!outline.empty()) outline.push_back(text(""));
      outline.push_back(txt(" " + s, Pal::Dim, true));
    };
    head("THIS FILE");
    const Mark* fm = worst_on(0);
    outline.push_back(fm ? txt(" " + first_line(fm->text), level_pal(fm->level), true) : txt(" not run", Pal::Dim));
    head("TABLES");
    for (auto* d : tests) {
      const Mark* m = worst_on(d->line);
      std::string word = m ? first_line(m->text) : "not run";
      word = word.substr(0, word.find(" · "));
      Pal p = m ? level_pal(m->level) : Pal::Dim;
      std::string g = m && !m->glyph.empty() ? m->glyph : " ";
      int room = ow - 4 - string_width(g) - string_width(word) - 2;
      Element e = hbox({text(" "), txt(g, p), text(" "), fit({{table_name(d)}}, std::max(4, room)), filler(), txt(word + " ", p)}) |
                  size(WIDTH, EQUAL, ow);
      if (d->line == focus) e = e | bg(Pal::Chip);
      outline.push_back(clickable(e, [this, path, l = d->line, jump] {
        reader_focus[path] = l;
        jump(l);
      }));
    }
    struct Fail {
      int line;
      std::string table, text;
    };
    std::vector<Fail> fails;  // error marks on rows, and a table's own failures (its result's later lines)
    for (auto& m : marks) {
      if (m.level != "error" || m.banner || m.line == 0) continue;
      const md::Directive* t = nullptr;
      for (auto* d : tests)
        if (d->line <= m.line) t = d;
      bool own = t && t->line == m.line;
      std::istringstream in(m.text);
      std::string l;
      for (bool first = true; std::getline(in, l); first = false)
        if (!(own && first)) fails.push_back({m.line, t ? table_name(t) : "line", l});
      if (!own && m.text.empty()) fails.push_back({m.line, t ? table_name(t) : "line", "did not pass"});
    }
    std::stable_sort(fails.begin(), fails.end(), [](const Fail& a, const Fail& b) { return a.line < b.line; });
    if (!fails.empty()) head("WHAT DID NOT PASS");
    for (size_t i = 0; i < fails.size(); ++i) {
      if (static_cast<int>(outline.size()) >= rows - 10) {
        outline.push_back(txt(" and " + std::to_string(fails.size() - i) + " more", Pal::Dim));
        break;
      }
      auto go = [jump, l = fails[i].line] { jump(l); };
      outline.push_back(clickable(fit({{" " + fails[i].table + ", line " + std::to_string(fails[i].line)}}, ow) | size(WIDTH, EQUAL, ow), go));
      outline.push_back(clickable(fit({{" " + fails[i].text, Pal::Note}}, ow) | size(WIDTH, EQUAL, ow), go));
    }
    std::string cmd, ref;
    for (size_t i = 0; i < live.size() && cmd.empty(); ++i)
      if (auto it = live[i].prs.find(reader_url); it != live[i].prs.end() && !marks_on(reader_url, path, b.head, nullptr, static_cast<int>(i)).empty())
        cmd = it->second.marks_command, ref = it->second.marks_ref;
    if (!cmd.empty()) {
      head("LAST COMMAND");
      std::string l;
      std::istringstream words(cmd);
      for (std::string w; words >> w;) {
        if (!l.empty() && string_width(l) + 1 + string_width(w) > ow - 2) {
          outline.push_back(txt(" " + l, Pal::Magenta));
          l.clear();
        }
        l += (l.empty() ? "" : " ") + w;
      }
      outline.push_back(txt(" " + l, Pal::Magenta));
      if (!ref.empty())
        outline.push_back(b.head.starts_with(ref) ? txt(" on " + ref.substr(0, 7) + ", the PR's head", Pal::Dim)
                                                  : txt(" on " + ref.substr(0, 7) + "; the PR has moved on", Pal::Yellow));
    }
    outline.push_back(text(""));
    Elements btns = {text(" ")};
    int used = 1;
    auto add_btn = [&](const std::string& key, const std::string& label, std::function<void()> fn) {
      int w = string_width(key) + string_width(label) + 4;  // " key label " and a space
      if (used + w > ow && used > 1) {  // the next row
        outline.push_back(hbox(std::move(btns)));
        btns = {text(" ")};
        used = 1;
      }
      btns.push_back(clickable(button(key, label), std::move(fn)));
      btns.push_back(text(" "));
      used += w;
    };
    for (auto& [pi, a] : plugin_keys())
      add_btn(a.key, a.line ? "Run file" : a.button.empty() ? a.label : a.button,
              [this, key = a.key, file = a.line ? path : ""] { plugin_action(key, reader_url, file); });
    if (runner && focus) add_btn("r", "Run table", [this, path, focus] { run_line(path, focus); });
    outline.push_back(hbox(std::move(btns)));
    if (es.size() > 1 && static_cast<int>(outline.size() + es.size()) + 2 <= rows) {
      head("ON THIS PAGE");
      headings(es.size());
    }
  } else if (ow) {
    if (ft)  // the temoc tables with their result, among the headings
      for (auto* d : tests) {
        const Mark* worst = nullptr;
        for (auto& m : marks)
          if (m.line == d->line && (!worst || rank(m.level) > rank(worst->level))) worst = &m;
        es.push_back({d->line, worst ? worst->glyph : "🧪", d->target.substr(0, d->target.find('#')),
                      worst ? level_pal(worst->level) : Pal::Dim});
      }
    std::stable_sort(es.begin(), es.end(), [](const Entry& a, const Entry& b) { return a.src < b.src; });
    outline.push_back(txt(" ON THIS PAGE", Pal::Dim, true));
    headings(static_cast<size_t>(std::max(0, rows - 9)));
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
  // A test table's header has a ▶ that runs it, and a row shows one while
  // the mouse is over it; the focused table's header is highlighted. A click
  // on a test table's line focuses that table.
  int thumb = n > rows ? std::max(1, rows * rows / n) : 0;
  int thumb_at = n > rows ? scroll * (rows - thumb) / std::max(1, n - rows) : 0;
  Elements body;
  for (int r = 0; r < rows; ++r) {
    Element left;
    if (r < static_cast<int>(status.size())) {
      left = hbox({text("    "), status[static_cast<size_t>(r)]});
    } else if (status.empty() && scroll + r < n) {
      int li = scroll + r;
      md::Line l = lines[static_cast<size_t>(li)];
      bool lead = l.src > 0 && (li == 0 || lines[static_cast<size_t>(li - 1)].src != l.src);  // its source line's first
      bool spec = ft && ft->spec;  // the description's tables have no file to run
      bool header = spec && l.test && l.head && lead, row = spec && l.test && !l.head && lead && l.part == md::Part::Table;
      Element gut;
      if (runner && !l.spans.empty() && (header || (row && reader_hover == li))) {
        l.spans.erase(l.spans.begin());
        gut = hbox({txt("▶", Pal::Accent, true), text(std::string(md::kGutter - 1, ' '))});
        if (l.tint) gut = gut | tint(l.tint);
        gut = clickable(gut, [this, path, at = header ? l.test : l.src] { run_line(path, at); });
      }
      Element e = gut ? hbox({gut, md_line(l)}) : md_line(l);
      if (header && l.test == focus) e = truecolor() ? e | bg(Pal::Sel) : e | inverted;
      left = hbox({text(" "), e});
      if (spec && l.test) {
        Hit& h = hit(left);
        h.click = [this, path, t = l.test] { reader_focus[path] = t; };
        h.hover = [this, li, row] { reader_hover = row ? li : -1; };
      }
    } else {
      left = text("");
    }
    Elements cells = {left | size(WIDTH, EQUAL, pane - 1),
                      n > rows ? txt(r >= thumb_at && r < thumb_at + thumb ? "┃" : "│", r >= thumb_at && r < thumb_at + thumb ? Pal::Dim : Pal::Faint)
                               : text(" ")};
    if (ow) {
      cells.push_back(txt("│", Pal::Faint));
      cells.push_back((r < static_cast<int>(outline.size()) ? outline[static_cast<size_t>(r)] : text("")) | size(WIDTH, EQUAL, ow));
    }
    body.push_back(hbox(std::move(cells)));
  }
  Element page = vbox(std::move(body));
  Hit& ph = hit(page);
  ph.wheel = [this, path](int dir) { reader_scroll[path] += 3 * dir; };
  ph.hover = [this] { reader_hover = -1; };

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
  bool spec = ft && ft->spec;
  for (auto& [i, a] : plugin_keys()) btns.push_back({a.key, a.line && spec ? "Run file" : a.button.empty() ? a.label : a.button});
  if (spec && runner) btns.push_back({"r", "Run table"});
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
      else reader_key(Event::Character(key));
    }));
    foot.push_back(text(" "));
  }
  foot.push_back(filler());
  if (now() < notice_until && !notice.empty()) foot.push_back(txt(notice + " ", notice_bad ? Pal::Red : Pal::Cyan));
  else if (!msg.empty()) foot.push_back(txt(msg + " ", Pal::Red));
  else if (n > rows)
    foot.push_back(hbox({txt("lines " + std::to_string(scroll + 1) + "–" + std::to_string(std::min(n, scroll + rows)) + " of " + std::to_string(n) + " ", Pal::Dim),
                         txt(cfg.mouse ? "· wheel scrolls " : "", Pal::Faint)}));

  Element all = vbox({head, meta, hbox(std::move(tabrow)), hbox(std::move(under)), page | flex, rule(),
                      hbox(std::move(foot))});
  hit(all).hover = [this] { reader_hover = -1; };  // the mouse left the page
  return all;
}

bool Ui::reader_key(const Event& e) {
  const Files* fs = reader_files();
  size_t n = 1 + (fs ? fs->tabs.size() : 0);
  reader_tab = std::min(reader_tab, n - 1);
  std::string path = reader_tab ? fs->tabs[reader_tab - 1].path : "";
  reader_hover = -1;  // a scroll or a new tab moves the rows: the mouse is over another one now
  if (e == Event::Escape || e == Event::Character('q')) mode = Mode::Normal;
  else if (e == Event::Tab || e == Event::Character(']')) reader_tab = (reader_tab + 1) % n;
  else if (e == Event::TabReverse || e == Event::Character('[')) reader_tab = (reader_tab + n - 1) % n;
  else if (scroll_key(e, reader_scroll[path], reader_rows())) {
  } else if (e == Event::Character('w') || e == Event::Return) open(reader_url);
  else if (e == Event::Character('y')) copy(reader_url);
  else if (e == Event::Character('?')) open_palette();
  else if (e == Event::Character('J')) focus_table(1);
  else if (e == Event::Character('K')) focus_table(-1);
  else if (e == Event::Character('r')) {
    const FileTab* ft = reader_tab_file();
    auto f = ft ? reader_focus.find(ft->path) : reader_focus.end();
    if (f == reader_focus.end() || !ft->spec) flash("no test table here");
    else run_line(ft->path, f->second);
  } else if (e.is_character()) {  // T: run this spec file, or on the description every changed spec
    const FileTab* ft = reader_tab_file();
    plugin_action(e.character(), reader_url, ft && ft->spec ? ft->path : "");
  }
  return true;
}

}  // namespace anton
