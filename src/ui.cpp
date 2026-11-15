// The PR list: fetching, plugins, views, the filter bar, menus and the
// palette, mouse dispatch. See ui_impl.hpp for how the UI files split.
#include "ui.hpp"

#include <signal.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/screen/string.hpp>

#include "config_write.hpp"
#include "github.hpp"
#include "proc.hpp"
#include "ui_impl.hpp"
#include "update.hpp"

namespace anton {

using namespace ftxui;

std::time_t now() { return std::time(nullptr); }

// "2026-11-14", in UTC: temoc's snoozes count days in UTC.
std::string today() { return ymd(now()); }

// Case-insensitive subsequence match: "bcore" finds "brick-so/core".
bool fuzzy(const std::string& hay, const std::string& needle) {
  size_t i = 0;
  for (char c : hay)
    if (i < needle.size() && std::tolower(static_cast<unsigned char>(c)) ==
                                 std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
  return i == needle.size();
}

namespace {

Element cell(const std::string& s, int w) { return text(s) | size(WIDTH, EQUAL, w); }

Element cell_right(const std::string& s, int w) { return hbox(filler(), text(s)) | size(WIDTH, EQUAL, w); }

int width(const std::string& s) { return string_width(s); }

// At most w cells, "…" where it's cut.
std::string clip(const std::string& s, int w) {
  if (width(s) <= w) return s;
  std::string out;
  int used = 0;
  for (auto& g : Utf8ToGlyphs(s)) {
    int gw = width(g);
    if (used + gw > w - 1) break;
    out += g;
    used += gw;
  }
  return out + "…";
}

std::string repeat(const std::string& s, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += s;
  return out;
}

std::string capital(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

std::string short_repo(const std::string& repo) { return repo.substr(repo.find('/') + 1); }

std::string no_scheme(const std::string& url) {
  auto p = url.find("://");
  return p == std::string::npos ? url : url.substr(p + 3);
}

Element at(Element e, int x, int y) {
  return vbox({emptyElement() | size(HEIGHT, EQUAL, std::max(0, y)),
               hbox({emptyElement() | size(WIDTH, EQUAL, std::max(0, x)), std::move(e)})});
}

}  // namespace

bool LineEdit::key(const Event& e) {
  auto prev = [&](size_t i) {
    do --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
    return i;
  };
  auto next = [&](size_t i) {
    do ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
    return i;
  };
  if (e == Event::ArrowLeft) {
    if (cur > 0) cur = prev(cur);
  } else if (e == Event::ArrowRight) {
    if (cur < s.size()) cur = next(cur);
  } else if (e == Event::Home || e == Event::CtrlA) {
    cur = 0;
  } else if (e == Event::End || e == Event::CtrlE) {
    cur = s.size();
  } else if (e == Event::Backspace) {
    if (cur > 0) {
      size_t p = prev(cur);
      s.erase(p, cur - p);
      cur = p;
    }
  } else if (e == Event::Delete) {
    if (cur < s.size()) s.erase(cur, next(cur) - cur);
  } else if (e == Event::CtrlU) {
    s.erase(0, cur);
    cur = 0;
  } else if (e == Event::CtrlW) {
    size_t p = cur;
    while (p > 0 && s[p - 1] == ' ') --p;
    while (p > 0 && s[p - 1] != ' ') --p;
    s.erase(p, cur - p);
    cur = p;
  } else if (e.is_character()) {
    s.insert(cur, e.character());
    cur += e.character().size();
  } else {
    return false;
  }
  return true;
}

Ui::Ui(Config c) : cfg(std::move(c)) {
  if (!cfg.adhoc_query.empty()) {
    offscreen = cfg.views;
    tabs.push_back({View{"Query", cfg.adhoc_query, "", "", {}}, std::nullopt});
  } else {
    for (auto& v : cfg.views) tabs.push_back({v, v});
    for (size_t i = 0; i < tabs.size(); ++i)
      if (tabs[i].v.name == cfg.start_view) active = i;
  }
  live.resize(cfg.plugins.size());
  pending.resize(cfg.plugins.size());
  running.assign(cfg.plugins.size(), true);
  last_code.assign(cfg.plugins.size(), 0);
  approvals.resize(cfg.plugins.size());
}

void Ui::post(std::function<void()> fn) {
  screen.Post(std::move(fn));
  screen.PostEvent(Event::Custom);
}

// -- fetching -------------------------------------------------------------------

std::string Ui::effective(const View& v) const {
  std::string q = with_scope(v.query, scope);
  return v.is_list() ? q : ensure_is_pr(q);
}

void Ui::request_search(const std::string& q) {
  std::lock_guard lk(mu);
  if (std::find(searches.begin(), searches.end(), q) == searches.end()) searches.push_back(q);
  busy = true;
  cv.notify_one();
}

void Ui::request_urls(const std::vector<std::string>& urls) {
  if (urls.empty()) return;
  std::lock_guard lk(mu);
  for (auto& u : urls) {
    want_urls.insert(u);
    requested.insert(u);
  }
  busy = true;
  cv.notify_one();
}

void Ui::request_full(const std::vector<std::string>& urls) {
  if (urls.empty()) return;
  std::lock_guard lk(mu);
  for (auto& u : urls) {
    want_full.insert(u);
    requested_full.insert(u);
  }
  busy = true;
  cv.notify_one();
}

// Watched PRs that are still open: one quiet full fetch, no "loading…".
void Ui::poll_watched() {
  std::vector<std::string> urls;
  for (size_t i = 0; i < live.size(); ++i)
    for (auto& [url, info] : live[i].prs)
      if (auto c = cache.find(url); info.watch && c != cache.end() && c->second.state == "OPEN")
        urls.push_back(url);
  last_watch = now();
  if (urls.empty()) return;
  std::lock_guard lk(mu);
  want_full.insert(urls.begin(), urls.end());
  cv.notify_one();
}

bool Ui::loading() {
  std::lock_guard lk(mu);
  return busy;
}

void Ui::worker() {
  for (;;) {
    std::string q;
    std::vector<std::string> urls, full;
    {
      std::unique_lock lk(mu);
      cv.wait(lk, [&] { return !searches.empty() || !want_urls.empty() || !want_full.empty(); });
      // URL work first, batching everything wanted so far.
      urls.assign(want_urls.begin(), want_urls.end());
      full.assign(want_full.begin(), want_full.end());
      want_urls.clear();
      want_full.clear();
      if (urls.empty() && full.empty()) {
        q = searches.front();
        searches.pop_front();
      }
    }
    Fetch f;
    bool is_search = urls.empty() && full.empty();
    try {
      if (is_search) {
        f = search_prs(q);
      } else {
        // States are cheap; only open PRs (and requested stubs) get the slow
        // full fetch. The rest are cached as partial stubs.
        Fetch states = fetch_states(urls);
        for (auto& s : states.prs)
          if (s.state == "OPEN") full.push_back(s.url);
        f = fetch_prs(full);
        std::set<std::string> have;
        for (auto& pr : f.prs) have.insert(pr.url);
        for (auto& s : states.prs)
          if (!have.count(s.url)) f.prs.push_back(s);
        if (f.error.empty()) f.error = states.error;
      }
    } catch (const std::exception& e) {  // odd JSON, no threads left: report, keep going
      f.error = std::string("fetch failed: ") + e.what();
    }
    post([this, q, f = std::move(f), is_search] {
      if (!f.viewer.empty()) viewer = f.viewer;
      std::vector<std::string> got;
      for (auto& pr : f.prs) {
        got.push_back(pr.url);
        if (!pr.owner.empty()) owners.insert(pr.owner);
        if (!pr.repo.empty()) repos.insert(pr.repo);
        auto it = cache.find(pr.url);
        // A new approval: one seen happen, not one that predates anton --
        // replaying old ones would undo a 👀 you put back for a re-review.
        if (!pr.partial && pr.my_review == "APPROVED" && it != cache.end() && !it->second.partial &&
            it->second.my_review_at != pr.my_review_at)
          for (size_t i = 0; i < approvals.size(); ++i)
            if (!one_shot(i)) approvals[i].insert(pr.url);
        if (pr.partial && it != cache.end() && !it->second.partial) {
          it->second.state = pr.state;  // keep the full data we already have
          it->second.closed = pr.closed;
        } else {
          cache[pr.url] = pr;
        }
      }
      notify_approved();
      error = f.error;
      if (!f.error.empty()) return;  // a failed refresh keeps the old rows
      if (is_search) results[q] = got;
      last_fetch = now();
    });
    std::lock_guard lk(mu);
    busy = !searches.empty() || !want_urls.empty() || !want_full.empty();
  }
}

std::vector<std::string> Ui::list_urls(const View& v) const {
  std::vector<std::string> out;
  int pi = plugin_index(v.plugin);
  if (pi < 0) return out;
  for (auto& [url, info] : live[static_cast<size_t>(pi)].prs)
    if (info.lists.count(v.list)) out.push_back(url);
  return out;
}

void Ui::refresh() {
  const View& v = view();
  if (v.is_list()) {
    std::vector<std::string> stale;  // merged/closed PRs never change again
    for (auto& u : list_urls(v))
      if (auto c = cache.find(u); c == cache.end() || c->second.state == "OPEN") stale.push_back(u);
    request_urls(stale);
  } else {
    request_search(effective(v));
  }
  if (last_refresh == 0)  // the first time: every search view, so each tab has its count
    for (auto& t : tabs)
      if (!t.v.is_list()) request_search(effective(t.v));
  if (plugins) plugins->refresh();
  requested_full.clear();  // stubs whose fill failed get another try
  ++column_gen;
  last_refresh = now();
}

// nullopt when this view has no data yet (search never answered).
std::optional<std::vector<Pr>> Ui::rows(size_t vi, std::string* err) const {
  const View& v = tabs[vi].v;
  std::string q = effective(v);
  std::vector<Pr> out;
  if (!v.is_list()) {
    auto it = results.find(q);
    if (it == results.end()) return std::nullopt;
    for (auto& u : it->second)
      if (auto c = cache.find(u); c != cache.end()) out.push_back(c->second);
  } else {
    std::string e;
    auto preds = parse_local(q, e);
    if (!preds) {
      if (err) *err = e;
      return out;
    }
    for (auto& u : list_urls(v))
      if (auto c = cache.find(u); c != cache.end() && !c->second.partial && local_match(c->second, *preds, viewer))
        out.push_back(c->second);
  }
  Sort sort = v.sort_or_default();
  if (sort == Sort::List && v.is_list() && !out.empty()) {  // non-empty: the plugin exists
    // Plugin order first; PRs it gave no order go last, newest update first.
    sort_prs(out, Sort::Updated);
    const auto& prs = live[static_cast<size_t>(plugin_index(v.plugin))].prs;
    auto key = [&](const Pr& p) {
      auto it = prs.find(p.url);
      return it != prs.end() && it->second.order ? *it->second.order : -HUGE_VAL;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Pr& a, const Pr& b) { return key(a) > key(b); });
  } else {
    sort_prs(out, sort == Sort::List ? Sort::Updated : sort);
  }
  return out;
}

// Stubs on the active list that could match once their full fields are
// known: judged on the predicates a stub can answer (state, org, repo).
std::vector<std::string> Ui::stubs_to_fill() const {
  const View& v = tabs[active].v;
  std::string e;
  auto preds = v.is_list() ? parse_local(effective(v), e) : std::nullopt;
  if (!preds) return {};
  std::vector<Pred> known;
  for (auto& p : *preds)
    if (p.key == "org" || p.key == "user" || p.key == "repo" || p.key == "recent" ||
        (p.key == "is" && p.value != "draft"))
      known.push_back(p);
  std::vector<std::string> out;
  for (auto& u : list_urls(v))
    if (auto c = cache.find(u);
        c != cache.end() && c->second.partial && !requested_full.count(u) && local_match(c->second, known, viewer))
      out.push_back(u);
  return out;
}

Seen Ui::seen() const {
  Seen s;
  s.owners = owners;
  s.repos = repos;
  for (auto& [url, p] : cache) {
    if (p.partial) continue;
    for (auto& l : p.labels) ++s.labels[l];
    if (!p.author.empty()) ++s.authors[p.author];
  }
  return s;
}

std::vector<std::string> Ui::plugin_lists(const std::string& plugin) const {
  std::set<std::string> out;
  if (int pi = plugin_index(plugin); pi >= 0)
    for (auto& [url, info] : live[static_cast<size_t>(pi)].prs) out.insert(info.lists.begin(), info.lists.end());
  return {out.begin(), out.end()};
}

// -- plugins -------------------------------------------------------------------

int Ui::plugin_index(const std::string& name) const {
  for (size_t i = 0; i < cfg.plugins.size(); ++i)
    if (cfg.plugins[i].name == name) return static_cast<int>(i);
  return -1;
}

void Ui::on_plugin_line(size_t i, const std::string& line, bool is_err) {
  if (is_err) {
    live[i].last_stderr = line;
    flash(cfg.plugins[i].name + ": " + line, 10);  // e.g. a bad setting
    return;
  }
  if (!running[i]) {  // first line after a restart: forget "exited"
    running[i] = true;
    live[i].health.clear();
    live[i].health_text.clear();
  }
  PluginState& st = one_shot(i) ? pending[i] : live[i];
  bool was_ok = st.health == "ok";
  std::string url = apply_line(st, line);
  if (!was_ok && st.health == "ok") notify_approved();  // ready (again): catch it up
  if (auto it = st.prs.find(url); it != st.prs.end() && !it->second.tags.empty() && !it->second.tags[0].glyph.empty())
    last_glyph[i] = it->second.tags[0].glyph;
  if (one_shot(i)) {  // health shows live even while the PRs wait for the run to end
    live[i].health = st.health;
    live[i].health_text = st.health_text;
    live[i].bad_lines = st.bad_lines;
  }
  if (!url.empty() && !cache.count(url) && !requested.count(url) && !st.prs[url].lists.empty())
    request_urls({url});
}

void Ui::on_plugin_exit(size_t i, int code) {
  last_code[i] = code;
  std::string err = live[i].last_stderr;
  if (one_shot(i)) {
    PluginState next = std::move(pending[i]);
    next.last_stderr = err;
    live[i] = std::move(next);
    pending[i] = PluginState{};
    for (auto& [url, info] : live[i].prs)
      if (!info.lists.empty() && !cache.count(url) && !requested.count(url)) request_urls({url});
  } else {
    running[i] = false;
    live[i] = PluginState{};  // it re-sends what is still true on restart
    live[i].last_stderr = err;
    live[i].health = "exited";
    live[i].health_text = "exit code " + std::to_string(code);
  }
}

// Delivers new approvals (anton-slkx swaps your 👀 for 👍) to plugins that
// are up; the rest wait for the plugin's next health ok.
void Ui::notify_approved() {
  if (!plugins) return;
  for (size_t i = 0; i < approvals.size(); ++i) {
    if (live[i].health != "ok") continue;
    for (auto it = approvals[i].begin(); it != approvals[i].end();)
      it = plugins->send(i, nlohmann::json({{"event", "approved"}, {"pr", *it}}).dump()) ? approvals[i].erase(it)
                                                                                          : std::next(it);
  }
}

std::vector<Ui::PluginKey> Ui::plugin_keys() const {
  std::vector<PluginKey> out;
  for (size_t i = 0; i < cfg.plugins.size(); ++i)
    if (!one_shot(i))  // one-shot plugins have no stdin
      for (auto& a : live[i].actions) out.push_back({i, a});
  return out;
}

// A plugin-declared key on a PR. False if no plugin claims it.
bool Ui::plugin_action(const std::string& key, const std::string& url, const std::string& file, int line) {
  for (auto& [i, a] : plugin_keys()) {
    if (a.key != key) continue;
    if (url.empty()) return true;
    std::string pr = url;
    if (auto c = cache.find(url); c != cache.end()) pr = c->second.repo + "#" + std::to_string(c->second.number);
    nlohmann::json msg = {{"action", a.id}, {"pr", url}};
    std::string what = a.label.empty() ? a.id : a.label;
    if (a.line && !file.empty()) {
      msg["file"] = file;
      what = "run " + file;
      if (line > 0) {
        msg["line"] = line;
        what += ":" + std::to_string(line);
      }
    }
    bool ok = plugins && plugins->send(i, msg.dump());
    flash(cfg.plugins[i].name + ": " + (ok ? what + " — " + pr : "not running"));
    return true;
  }
  return false;
}

bool Ui::plugin_up(size_t i) const {
  if (live[i].health == "error") return false;
  return one_shot(i) ? last_code[i] == 0 : running[i] && live[i].health != "exited";
}

std::optional<Status> Ui::status_of(const std::string& url) const {
  for (auto& st : live)
    if (auto it = st.prs.find(url); it != st.prs.end() && it->second.status) return it->second.status;
  return std::nullopt;
}

std::vector<Tag> Ui::tags_of(const std::string& url) const {
  std::vector<Tag> out;
  for (auto& st : live)
    if (auto it = st.prs.find(url); it != st.prs.end())
      out.insert(out.end(), it->second.tags.begin(), it->second.tags.end());
  return out;
}

std::vector<std::pair<std::string, std::string>> Ui::notes_of(const std::string& url) const {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < live.size(); ++i)
    if (auto it = live[i].prs.find(url); it != live[i].prs.end() && !it->second.note.empty())
      out.emplace_back(cfg.plugins[i].name, it->second.note);
  return out;
}

bool Ui::has_note(const std::string& url) const { return !notes_of(url).empty(); }

// -- people, titles -------------------------------------------------------------

std::string Ui::shown_title(const std::string& t) const {
  return cfg.title_strip.empty() ? t : std::regex_replace(t, cfg.title_re, "");
}

const Person* Ui::person(const std::string& login) const {
  auto it = cfg.people.find(lower(login));
  return it != cfg.people.end() ? &it->second : nullptr;
}

// A Slack tag names someone by their Slack name: the person whose "name" it
// is, or you.
const Person* Ui::tag_person(const Tag& t) const {
  if (t.text == "you") return person(viewer);
  for (auto& [login, p] : cfg.people)
    if (!p.name.empty() && lower(p.name) == lower(t.text)) return &p;
  return nullptr;
}

Element Ui::paint(Element e, const Person* p) {
  if (!p) return e;
  if (p->rgb >= 0 && (truecolor() || p->ansi < 0))  // no ansi: FTXUI picks the nearest
    return e | color(Color::RGB(p->rgb >> 16, (p->rgb >> 8) & 0xff, p->rgb & 0xff));
  if (p->ansi >= 0) return e | color(p->ansi < 16 ? Color(Color::Palette16(p->ansi)) : Color(Color::Palette256(p->ansi)));
  return e;
}

Element Ui::fit(const std::vector<Part>& parts, int w) {
  Elements out;
  int used = 0;
  for (auto& p : parts) {
    if (used >= w) break;
    std::string s = width(p.s) > w - used ? clip(p.s, w - used) : p.s;
    used += width(s);
    out.push_back(p.who ? paint(text(s), p.who) : txt(s, p.p));
  }
  return hbox(std::move(out));
}

std::string Ui::who(const std::string& login) const {
  const Person* p = person(login);
  return p && !p->name.empty() ? p->name : login;
}

// -- views ---------------------------------------------------------------------

// Its filters differ from what config.json has (or it was never saved).
bool Ui::modified(size_t i) const {
  const Tab& t = tabs[i];
  if (!t.saved) return true;
  const View& s = *t.saved;
  return t.v.plugin != s.plugin || t.v.list != s.list || t.v.sort_or_default() != s.sort_or_default() ||
         !same_query(with_scope(t.v.query, scope), s.query);
}

// Chips and the query row both end here. The org/repo scope is session-wide
// (01-anton.md): while one is set, the scope qualifiers in q become it.
void Ui::apply_query(const std::string& q) {
  if (!scope.empty()) {
    std::vector<std::string> sc;
    for (auto& t : tokenize(q))
      if (t[0] != '-' && kScopeKeys.count(qualifier_key(t))) sc.push_back(t);
    scope = join(sc);
  }
  view().query = q;
  selected = list_top = 0;
  refresh();
}

void Ui::set_sort(Sort s) {
  view().sort = s;
  selected = list_top = 0;
}

std::vector<View> Ui::saved_views() const {
  std::vector<View> out = offscreen;
  for (auto& t : tabs)
    if (t.saved) out.push_back(*t.saved);
  return out;
}

bool Ui::write_config(const std::string& done) {
  Config c = cfg;
  c.views = saved_views();
  if (std::string err = save_config(c); !err.empty()) {
    flash(err, 10, true);
    return false;
  }
  cfg.views = c.views;
  if (!done.empty()) flash(done);
  return true;
}

void Ui::save_view(size_t i, bool named) {
  if (!tabs[i].saved && !named) {  // never saved: it needs a name of its own first
    prompt("Save view as", tabs[i].v.name, "the tab's name; saved to config.json",
           [this, i](const std::string& name) {
             if (std::string err = rename_view(i, name); !err.empty()) return err;
             save_view(i, true);
             return std::string();
           });
    return;
  }
  View s = tabs[i].v;
  s.query = with_scope(s.query, scope);
  auto old = tabs[i].saved;
  tabs[i].saved = s;
  if (!write_config("saved " + s.name + " to config.json")) tabs[i].saved = old;
  else tabs[i].v = s;
}

void Ui::save_as_prompt(size_t i) {
  prompt("Save as new view", tabs[i].v.name + " copy", "a new tab with these filters; this one reverts",
         [this, i](const std::string& raw) {
           std::string name = printable(raw);
           if (name.empty()) return std::string("a view needs a name");
           for (auto& t : tabs)
             if (t.v.name == name) return "there's a view named " + name + " already";
           save_as_new(i, name);
           return std::string();
         });
}

void Ui::save_as_new(size_t i, const std::string& name) {
  Tab t{tabs[i].v, std::nullopt};
  t.v.name = name;
  t.v.query = with_scope(t.v.query, scope);
  t.saved = t.v;
  if (tabs[i].saved) tabs[i].v = *tabs[i].saved;  // the changes move to the new view
  tabs.insert(tabs.begin() + static_cast<long>(i) + 1, t);
  active = i + 1;
  selected = list_top = 0;
  write_config("saved " + name + " to config.json");
  refresh();
}

// Back to what config.json has. The org/repo scope goes too: it's what
// keeps every tab modified while it's set.
void Ui::revert_view(size_t i) {
  if (!tabs[i].saved) return;
  tabs[i].v = *tabs[i].saved;
  scope.clear();
  selected = list_top = 0;
  refresh();
}

static std::string unique_name(const std::vector<Ui::Tab>& tabs, const std::string& base) {
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + " " + std::to_string(n);
    if (std::none_of(tabs.begin(), tabs.end(), [&](const Ui::Tab& t) { return t.v.name == name; })) return name;
  }
}

void Ui::new_view() {
  tabs.push_back({View{unique_name(tabs, "New view"), "is:open", "", "", {}}, std::nullopt});
  switch_view(tabs.size() - 1);
  start_query();
}

void Ui::duplicate_view(size_t i) {
  Tab t{tabs[i].v, std::nullopt};
  t.v.name = unique_name(tabs, t.v.name + " copy");
  t.v.query = with_scope(t.v.query, scope);
  t.saved = t.v;
  tabs.insert(tabs.begin() + static_cast<long>(i) + 1, t);
  if (active > i) ++active;
  write_config("duplicated as " + t.v.name);
}

void Ui::move_view(size_t i, int by) {
  long j = static_cast<long>(i) + by;
  if (j < 0 || j >= static_cast<long>(tabs.size())) return;
  std::swap(tabs[i], tabs[static_cast<size_t>(j)]);
  if (active == i) active = static_cast<size_t>(j);
  else if (active == static_cast<size_t>(j)) active = i;
  write_config();
}

void Ui::delete_view(size_t i) {
  if (tabs.size() == 1) return flash("anton needs one view; this is the last", 3, true);
  std::string name = tabs[i].v.name;
  tabs.erase(tabs.begin() + static_cast<long>(i));
  if (cfg.start_view == name) cfg.start_view.clear();
  if (active > i || active == tabs.size()) --active;
  selected = list_top = 0;
  write_config("deleted " + name);
  refresh();
}

std::string Ui::rename_view(size_t i, const std::string& raw) {
  std::string name = printable(raw);
  if (name.empty()) return "a view needs a name";
  for (size_t j = 0; j < tabs.size(); ++j)
    if (j != i && tabs[j].v.name == name) return "there's a view named " + name + " already";
  if (name == tabs[i].v.name) return "";
  // ponytail: the file's entry is found by name, so a rename drops keys anton
  // doesn't know from that view's entry; carry an original name if one appears.
  if (cfg.start_view == tabs[i].v.name) cfg.start_view = name;
  tabs[i].v.name = name;
  if (tabs[i].saved) {
    tabs[i].saved->name = name;
    write_config("renamed to " + name);
  }
  return "";
}

void Ui::switch_view(size_t i) {
  if (i >= tabs.size() || i == active) return;
  active = i;
  selected = list_top = 0;
  refresh();
}

void Ui::open(const std::string& url) {
  std::thread([url] { run({"gh", "pr", "view", "--web", url}); }).detach();
  flash("opening " + url);
}

void Ui::copy(const std::string& s) {
  std::thread([this, s] {
    for (auto argv : std::vector<std::vector<std::string>>{{"pbcopy"}, {"wl-copy"}, {"xclip", "-selection", "clipboard"}})
      if (run(argv, s).code == 0) return post([this] { flash("copied"); });
    post([this] { flash("no clipboard tool (pbcopy, wl-copy, xclip)"); });
  }).detach();
}

void Ui::flash(const std::string& s, int secs, bool bad) {
  notice = s;
  notice_until = now() + secs;
  notice_bad = bad;
}

// At startup and every 15 min (every merge is a release), until there's
// something to say. Detached: anton _Exits on quit, so nothing joins it.
void Ui::check_update() {
  last_update_check = now();
  std::thread([this, install = cfg.auto_update] {
    std::string note = self_update(install);
    if (!note.empty()) post([this, note] { update_note = note; });
  }).detach();
}

// -- hits and menus ------------------------------------------------------------

Hit& Ui::hit(Element& e) {
  hits.emplace_back();
  e = e | reflect(hits.back().box);
  return hits.back();
}

Element Ui::clickable(Element e, std::function<void()> f) {
  hit(e).click = std::move(f);
  return e;
}

void Ui::push_menu(Menu m) {
  auto term = Terminal::Size();
  if (m.x >= 0) m.x = std::clamp(m.x, 0, std::max(0, term.dimx - m.w));
  menus.push_back(std::move(m));
}

void Ui::open_chip_menu(const Chip& c) {
  Menu m;
  m.title = c.label;
  m.anchor = "chip:" + c.label + ":" + join(c.tokens);
  const Box& b = anchors[m.anchor];
  m.x = b.x_min;
  m.y = b.y_max + 1;
  m.w = 54;
  if (!menus.empty()) {  // from the folded "filters" menu: beside it
    m.x = menus.back().x + menus.back().w;
    m.y = menus.back().y;
  }
  auto opts = chip_options(c.fam, seen());
  bool list = view().is_list();
  m.items = [this, c, opts, list] {
    std::string q = with_scope(view().query, scope);
    std::vector<MenuItem> items;
    bool fixed = c.fam == "state" || c.fam == "review" || c.fam == "author" || c.fam == "repo";
    for (auto& o : opts) {
      MenuItem it;
      it.label = o.label;
      it.radio = true;
      it.on = o.token.empty() ? !c.set : has_token(q, o.token);
      bool ok = list ? o.local : o.search;
      it.hint = !ok ? (list ? "search views only" : "list views only") : !o.hint.empty() ? o.hint : o.token;
      if (ok)
        it.run = [this, c, tok = o.token, fixed] {
          std::string now_q = with_scope(view().query, scope);
          apply_query(fixed || c.fam == "draft" ? with_family(now_q, c.fam, tok) : with_token(without(now_q, c.tokens), tok));
        };
      items.push_back(it);
    }
    if (!fixed && c.set) {
      if (!items.empty()) items.push_back({MenuItem::Sep, "", "", {}});
      MenuItem rm{MenuItem::Normal, "remove", "✕", [this, c] { apply_query(without(with_scope(view().query, scope), c.tokens)); }};
      items.push_back(rm);
    }
    return items;
  };
  if (list) {
    std::string p = view().plugin;
    m.foot = {"this view filters the " + p + " list", "locally; greyed ones need GitHub"};
  }
  push_menu(m);
}

void Ui::open_add_filter() {
  Menu m;
  m.title = "add filter";
  m.anchor = "chip:+";
  const Box& b = anchors[chips_folded ? "chip:filters" : m.anchor];
  m.x = b.x_min;
  m.y = b.y_max + 1;
  m.w = 34;
  if (!menus.empty()) {
    m.x = menus.back().x + menus.back().w;
    m.y = menus.back().y;
  }
  bool list = view().is_list();
  m.items = [this, list] {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < add_kinds().size(); ++i) {
      const AddKind& k = add_kinds()[i];
      bool ok = list ? k.local : k.search;
      MenuItem it{MenuItem::Normal, k.label, ok ? k.prefix : "search views only", {}};
      it.sub = true;
      if (ok)
        it.run = [this, i] {
          const Menu& self = menus.back();
          open_add_value(add_kinds()[i], self.x + self.w);
        };
      items.push_back(it);
    }
    return items;
  };
  m.foot = {"or type GitHub search syntax", "in the query bar: /"};
  push_menu(m);
}

void Ui::open_add_value(const AddKind& k, int x) {
  Menu m;
  m.title = k.label;
  m.x = x;
  m.y = menus.empty() ? 2 : menus.back().y;
  m.w = 37;
  m.input = k.prefix != "is:draft";
  auto opts = add_values(k, seen(), now());
  bool list = view().is_list();
  m.items = [this, opts, list, k] {
    std::vector<MenuItem> items;
    const std::string typed = menus.empty() ? "" : menus.back().edit.s;
    for (auto& o : opts) {
      if (lower(o.label).find(lower(typed)) == std::string::npos) continue;
      bool ok = list ? o.local : o.search;
      MenuItem it{MenuItem::Normal, o.label, ok ? o.hint : "search views only", {}};
      if (ok)
        it.run = [this, tok = o.token, draft = k.prefix == "is:draft"] {
          std::string q = with_scope(view().query, scope);
          apply_query(draft ? with_family(q, "draft", tok) : with_token(q, tok));
        };
      items.push_back(it);
      if (items.size() >= 12) break;
    }
    return items;
  };
  if (m.input) {
    m.submit = [this, k](const std::string& typed) {
      std::string tok = typed_token(k, typed);
      if (tok.empty()) return std::string("type a value");
      apply_query(with_token(with_scope(view().query, scope), tok));
      return std::string();
    };
    m.foot = {"⏎ takes what you typed"};
  }
  push_menu(m);
}

void Ui::open_sort_menu() {
  Menu m;
  m.title = "sort";
  m.anchor = "chip:sort";
  m.w = 30;
  const Box& b = anchors[m.anchor];
  m.x = b.x_max + 1 - m.w;
  m.y = b.y_max + 1;
  m.items = [this] {
    std::vector<MenuItem> items;
    for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
      if (s == Sort::List && !view().is_list()) continue;
      static const char* what[] = {"the plugin's order", "last updated first", "newest first (AGE)",
                                   "biggest first (SIZE)", "by repo (REPO)"};
      MenuItem it{MenuItem::Normal, s == Sort::List ? "list order" : sort_name(s), what[static_cast<int>(s)],
                  [this, s] { set_sort(s); }};
      it.radio = true;
      it.on = view().sort_or_default() == s;
      items.push_back(it);
    }
    return items;
  };
  push_menu(m);
}

void Ui::open_list_menu() {
  Menu m;
  m.title = "list";
  m.anchor = "chip:list";
  const Box& b = anchors[m.anchor];
  m.x = b.x_min;
  m.y = b.y_max + 1;
  m.w = 40;
  m.items = [this] {
    std::vector<MenuItem> items;
    for (auto& l : plugin_lists(view().plugin)) {
      MenuItem it{MenuItem::Normal, l, "", [this, l] {
                    view().list = l;
                    selected = list_top = 0;
                    refresh();
                  }};
      it.radio = true;
      it.on = view().list == l;
      items.push_back(it);
    }
    if (items.empty()) items.push_back({MenuItem::Normal, view().list, "nothing listed yet", {}});
    return items;
  };
  m.foot = {"the lists the " + view().plugin + " plugin offers now"};
  push_menu(m);
}

void Ui::open_folded_menu() {
  Menu m;
  m.title = "filters";
  m.anchor = "chip:filters";
  const Box& b = anchors[m.anchor];
  m.x = b.x_min;
  m.y = b.y_max + 1;
  m.w = 34;
  m.items = [this] {
    std::vector<MenuItem> items;
    if (view().is_list())
      items.push_back({MenuItem::Normal, "list " + view().plugin + " · " + view().list, "", [this] { open_list_menu(); }});
    for (auto& c : chips(with_scope(view().query, scope))) {
      MenuItem it{MenuItem::Normal, c.label + " " + c.value, c.set ? "set" : "", [this, c] { open_chip_menu(c); }};
      it.sub = true;
      items.push_back(it);
    }
    MenuItem add{MenuItem::Normal, "+ filter", "f", [this] { open_add_filter(); }};
    add.sub = true;
    items.push_back(add);
    return items;
  };
  push_menu(m);
}

void Ui::open_tab_menu(size_t i) {
  if (i >= tabs.size()) return;
  Menu m;
  m.anchor = "tab:" + std::to_string(i);
  const Box& b = anchors[m.anchor];
  m.x = b.x_min;
  m.y = b.y_max + 1;
  m.w = 29;
  m.items = [this, i] {
    std::vector<MenuItem> it;
    if (i >= tabs.size()) return it;
    bool mod = modified(i), saved = tabs[i].saved.has_value();
    auto item = [](std::string label, std::string key, std::function<void()> fn) {
      return MenuItem{MenuItem::Normal, std::move(label), std::move(key), std::move(fn)};
    };
    it.push_back(item("Save filters", "S", mod ? std::function<void()>([this, i] { save_view(i); }) : nullptr));
    it.push_back(item("Save as new view…", "", [this, i] { save_as_prompt(i); }));
    it.push_back(item("Revert to saved", "", mod && saved ? std::function<void()>([this, i] { revert_view(i); }) : nullptr));
    it.push_back({MenuItem::Sep, "", "", {}});
    it.push_back(item("Rename…", "", [this, i] {
      prompt("Rename view", tabs[i].v.name, "", [this, i](const std::string& name) { return rename_view(i, name); });
    }));
    it.push_back(item("Duplicate", "", [this, i] { duplicate_view(i); }));
    it.push_back(item("Move left", "<", i > 0 ? std::function<void()>([this, i] { move_view(i, -1); }) : nullptr));
    it.push_back(item("Move right", ">", i + 1 < tabs.size() ? std::function<void()>([this, i] { move_view(i, 1); }) : nullptr));
    it.push_back({MenuItem::Sep, "", "", {}});
    MenuItem del = item("Delete view…", "", tabs.size() > 1 ? std::function<void()>([this, i] {
      confirm("Delete " + tabs[i].v.name + "?", "Delete " + tabs[i].v.name, [this, i] { delete_view(i); });
    }) : nullptr);
    del.danger = true;
    it.push_back(del);
    return it;
  };
  push_menu(m);
}

std::vector<MenuItem> Ui::commands(const Pr* cur) {
  struct Cmd {
    const char* group;
    std::string label, key;
    std::function<void()> run;
  };
  std::vector<Cmd> c;
  if (mode == Mode::Reader) {  // the reader's keys, on the PR it shows
    const FileTab* ft = reader_tab_file();
    bool spec = ft && ft->spec;
    std::string url = reader_url, path = spec ? ft->path : "";
    auto key = [this](Event e) { return [this, e] { reader_key(e); }; };
    c.push_back({"READER", "Back to the list", "esc", key(Event::Escape)});
    c.push_back({"READER", "Next file", "Tab", key(Event::Tab)});
    c.push_back({"READER", "Previous file", "⇧Tab", key(Event::TabReverse)});
    if (spec) {
      c.push_back({"READER", "Next test table", "J", key(Event::Character('J'))});
      c.push_back({"READER", "Previous test table", "K", key(Event::Character('K'))});
      if (line_key()) c.push_back({"READER", "Run the focused test table", "r", key(Event::Character('r'))});
    }
    for (auto& [i, a] : plugin_keys()) {
      std::string name = " · " + cfg.plugins[i].name;
      if (a.line && spec) {
        c.push_back({"READER", "Run this file" + name, a.key, [this, k = a.key, url, path] { plugin_action(k, url, path); }});
        c.push_back({"READER", capital(a.label.empty() ? a.id : a.label) + name, "", [this, k = a.key, url] { plugin_action(k, url); }});
      } else {
        c.push_back({"READER", capital(a.label.empty() ? a.id : a.label) + name, a.key, [this, k = a.key, url] { plugin_action(k, url); }});
      }
    }
    c.push_back({"READER", "Open in browser", "w", key(Event::Character('w'))});
    c.push_back({"READER", "Copy URL", "y", key(Event::Character('y'))});
  }
  if (cur && mode != Mode::Reader) {
    Pr pr = *cur;
    c.push_back({"THIS PR", "Read description and files", "⏎", [this, pr] { open_reader(pr); }});
    c.push_back({"THIS PR", "Open in browser", "w", [this, pr] { open(pr.url); }});
    c.push_back({"THIS PR", "Copy URL", "y", [this, pr] { copy(pr.url); }});
    if (has_note(pr.url)) c.push_back({"THIS PR", "Read the note", "n", [this, pr] { act("n", &pr); }});
    for (auto& [i, a] : plugin_keys())
      c.push_back({"THIS PR", capital(a.label.empty() ? a.id : a.label) + " · " + cfg.plugins[i].name, a.key,
                   [this, key = a.key, url = pr.url] { plugin_action(key, url); }});
  }
  if (mode != Mode::Reader) {  // the list behind the reader is not what the keys act on
    c.push_back({"LIST", "Add filter…", "f", [this] { open_add_filter(); }});
    c.push_back({"LIST", "Edit query", "/", [this] { start_query(); }});
    c.push_back({"LIST", "Sort by…", "", [this] { open_sort_menu(); }});
    c.push_back({"LIST", "Next sort", "s", [this] { act("s", nullptr); }});
    c.push_back({"LIST", "Pick org", "o", [this] { open_picker(false); }});
    c.push_back({"LIST", "Pick repo", "p", [this] { open_picker(true); }});
    c.push_back({"LIST", "Clear org/repo", "x", [this] { act("x", nullptr); }});
    c.push_back({"LIST", "Refresh", "r", [this] { refresh(); }});
    for (size_t i = 0; i < tabs.size() && i < 9; ++i)
      c.push_back({"VIEWS", "Go to " + tabs[i].v.name, std::to_string(i + 1), [this, i] { switch_view(i); }});
    c.push_back({"VIEWS", "Next view", "Tab", [this] { switch_view((active + 1) % tabs.size()); }});
    c.push_back({"VIEWS", "Previous view", "⇧Tab", [this] { switch_view((active + tabs.size() - 1) % tabs.size()); }});
    c.push_back({"VIEWS", "New view", "+", [this] { new_view(); }});
    if (modified(active)) {
      c.push_back({"VIEWS", "Save filters to view", "S", [this] { save_view(active); }});
      if (tabs[active].saved) c.push_back({"VIEWS", "Revert view to saved", "", [this] { revert_view(active); }});
    }
    c.push_back({"VIEWS", "View menu: rename, duplicate, delete…", "m", [this] { open_tab_menu(active); }});
    c.push_back({"VIEWS", "Move view left", "<", [this] { move_view(active, -1); }});
  c.push_back({"VIEWS", "Move view right", ">", [this] { move_view(active, 1); }});
  }
  c.push_back({"ANTON", "Settings", ",", [this] { open_settings(); }});
  c.push_back({"ANTON", "Open config.json in $EDITOR", "e", [this] { edit_config(); }});
  c.push_back({"ANTON", "Quit", "q", [this] { screen.Exit(); }});

  std::vector<MenuItem> out;
  const std::string typed = menus.empty() ? "" : menus.back().edit.s;
  std::string group;
  for (auto& cmd : c) {
    if (!fuzzy(cmd.label + " " + cmd.group, typed)) continue;
    if (group != cmd.group) out.push_back({MenuItem::Header, group = cmd.group, "", {}});
    out.push_back({MenuItem::Normal, cmd.label, cmd.key, cmd.run});
  }
  if (out.empty()) out.push_back({MenuItem::Normal, "nothing matches", "", {}});
  return out;
}

void Ui::open_palette() {
  Menu m;
  m.title = "commands";
  m.input = true;
  m.placeholder = "type to filter · every action, with its key";
  m.w = 70;
  m.y = 3;
  m.items = [this] {
    auto rs = rows(active).value_or(std::vector<Pr>{});
    const Pr* cur = rs.empty() || mode != Mode::Normal ? nullptr : &rs[static_cast<size_t>(std::clamp(selected, 0, static_cast<int>(rs.size()) - 1))];
    return commands(cur);
  };
  m.foot = {"↑↓ move  ⏎ run  esc close · every key works without this menu"};
  push_menu(m);
}

void Ui::open_picker(bool repo) {
  Menu m;
  m.title = repo ? "repo" : "org";
  m.input = true;
  m.placeholder = "type to filter, or a name";
  m.w = 44;
  m.y = 4;
  m.items = [this, repo] {
    std::vector<MenuItem> items;
    const std::string typed = menus.empty() ? "" : menus.back().edit.s;
    for (auto& c : repo ? repos : owners) {
      if (c.empty() || !fuzzy(c, typed)) continue;
      items.push_back({MenuItem::Normal, c, "", [this, repo, c] {
                         scope = (repo ? "repo:" : "org:") + c;
                         selected = list_top = 0;
                         refresh();
                       }});
      if (items.size() >= 10) break;
    }
    return items;
  };
  m.submit = [this, repo](const std::string& typed) {
    if (typed.empty()) return std::string("nothing seen yet; type one");
    scope = (repo ? "repo:" : "org:") + typed;
    selected = list_top = 0;
    refresh();
    return std::string();
  };
  m.foot = {"⏎ uses what you typed when nothing matches"};
  push_menu(m);
}

void Ui::prompt(const std::string& title, const std::string& initial, const std::string& hint,
                std::function<std::string(const std::string&)> accept) {
  Menu m;
  m.title = title;
  m.input = true;
  m.edit.set(initial);
  m.w = 52;
  m.y = 5;
  m.submit = std::move(accept);
  m.foot = {"⏎ ok  esc cancel"};
  if (!hint.empty()) m.foot.insert(m.foot.begin(), hint);
  push_menu(m);
}

void Ui::confirm(const std::string& title, const std::string& yes, std::function<void()> fn) {
  Menu m;
  m.title = title;
  m.w = 40;
  m.y = 5;
  m.items = [yes, fn] {
    MenuItem y{MenuItem::Normal, yes, "", fn};
    y.danger = true;
    return std::vector<MenuItem>{y, {MenuItem::Normal, "Cancel", "esc", [] {}}};
  };
  push_menu(m);
}

void Ui::run_item(size_t depth, int idx) {
  if (depth >= menus.size() || !menus[depth].items) return;
  auto items = menus[depth].items();
  if (idx < 0 || idx >= static_cast<int>(items.size()) || !items[static_cast<size_t>(idx)].selectable()) return;
  MenuItem it = items[static_cast<size_t>(idx)];
  if (it.sub) {  // opens the next menu beside this one
    menus.resize(depth + 1);
    menus[depth].sel = idx;
    it.run();
    return;
  }
  close_menus();
  it.run();
}

bool Ui::menu_key(const Event& e) {
  size_t d = menus.size() - 1;
  auto items = menus[d].items ? menus[d].items() : std::vector<MenuItem>{};
  int n = static_cast<int>(items.size());
  auto step = [&](int dir) {
    Menu& m = menus[d];
    for (int k = 1; k <= n; ++k) {
      int i = ((m.sel < 0 ? (dir > 0 ? -1 : 0) : m.sel) + dir * k + n * 2) % std::max(1, n);
      if (items[static_cast<size_t>(i)].selectable()) {
        m.sel = i;
        return;
      }
    }
  };
  Menu& m = menus[d];
  bool sel_ok = m.sel >= 0 && m.sel < n && items[static_cast<size_t>(m.sel)].selectable();
  if (e == Event::Escape) {
    menus.pop_back();
  } else if (e == Event::ArrowDown || e == Event::CtrlN || (!m.input && e == Event::Character('j'))) {
    step(1);
  } else if (e == Event::ArrowUp || e == Event::CtrlP || (!m.input && e == Event::Character('k'))) {
    step(-1);
  } else if (e == Event::ArrowLeft && d > 0 && (!m.input || m.edit.s.empty())) {
    menus.pop_back();
  } else if ((e == Event::ArrowRight || e == Event::Tab) && sel_ok && items[static_cast<size_t>(m.sel)].sub) {
    run_item(d, m.sel);
  } else if (e == Event::Return) {
    if (sel_ok) {
      run_item(d, m.sel);
    } else if (m.submit) {
      auto submit = m.submit;
      std::string err = submit(m.edit.s);
      if (!err.empty()) menus[d].error = err;
      else menus.erase(menus.begin(), menus.begin() + static_cast<long>(d) + 1);  // keep any it opened
    }
  } else if (m.input && m.edit.key(e)) {
    m.sel = -1;  // the first match, once rendered
    m.top = 0;
    m.error.clear();
  } else if (!m.input && e.is_character()) {  // a row's key: S, <, > in the tab menu
    for (int i = 0; i < n; ++i)
      if (items[static_cast<size_t>(i)].selectable() && items[static_cast<size_t>(i)].hint == e.character()) {
        run_item(d, i);
        break;
      }
  }
  return true;
}

Element Ui::render_menu(Menu& m, size_t depth, std::function<void(int)> pick, std::function<void(int)> hover) {
  if (!pick) pick = [this, depth](int i) { run_item(depth, i); };
  if (!hover)
    hover = [this, depth](int i) {
      if (depth < menus.size()) menus[depth].sel = i;
    };
  auto term = Terminal::Size();
  auto items = m.items ? m.items() : std::vector<MenuItem>{};
  int n = static_cast<int>(items.size());
  bool grouped = std::any_of(items.begin(), items.end(), [](const MenuItem& i) { return i.kind == MenuItem::Header; });
  if (m.sel < 0 || m.sel >= n || !items[static_cast<size_t>(m.sel)].selectable()) {
    m.sel = -1;
    for (int i = 0; i < n && m.sel < 0; ++i)
      if (items[static_cast<size_t>(i)].selectable()) m.sel = i;
  }
  int w = std::min(m.w, term.dimx), inner = w - 2;
  int x = m.x < 0 ? (term.dimx - w) / 2 : std::min(m.x, term.dimx - w);
  auto side = [&](Element content) {
    return hbox({txt("│", Pal::Border), std::move(content) | size(WIDTH, EQUAL, inner), txt("│", Pal::Border)});
  };
  auto divider = [&] { return txt("├" + repeat("─", inner) + "┤", Pal::Border); };
  Elements rows;
  std::string title = m.title.empty() ? "" : " " + clip(m.title, inner - 4) + " ";
  rows.push_back(hbox({txt(title.empty() ? "┌" : "┌─", Pal::Border), txt(title, Pal::Fg, true),
                       txt(repeat("─", inner - width(title) - (title.empty() ? 0 : 1)) + "┐", Pal::Border)}));
  if (m.input) {
    Elements in = {txt(" › ", Pal::Accent, true)};
    std::string before = m.edit.s.substr(0, m.edit.cur), after = m.edit.s.substr(m.edit.cur);
    in.push_back(txt(before));
    in.push_back(text(" ") | bg(Pal::Accent));
    if (m.edit.s.empty()) in.push_back(txt(clip(m.placeholder, inner - 5), Pal::Faint));
    in.push_back(txt(after));
    rows.push_back(side(hbox(std::move(in))));
    if (n > 0) rows.push_back(side(txt(repeat("─", inner), Pal::Faint)));
  }
  int extra = (m.foot.empty() ? 0 : static_cast<int>(m.foot.size()) + 1) + (m.error.empty() ? 0 : 1);
  int room = std::max(3, term.dimy - m.y - 2 - (m.input ? 2 : 0) - extra);
  if (m.sel >= 0) m.top = std::clamp(m.top, m.sel - room + 1, m.sel);
  m.top = std::clamp(m.top, 0, std::max(0, n - room));
  for (int i = m.top; i < std::min(n, m.top + room); ++i) {
    const MenuItem& it = items[static_cast<size_t>(i)];
    if (it.kind == MenuItem::Sep) {
      rows.push_back(divider());
      continue;
    }
    if (it.kind == MenuItem::Header) {
      rows.push_back(side(txt(" " + it.label, Pal::Dim, true)));
      continue;
    }
    bool on = it.selectable(), cur = i == m.sel;
    Elements r = {text(grouped ? "   " : " ")};
    if (it.radio) r.push_back(txt(it.on ? "● " : "○ ", !on ? Pal::Faint : it.on ? Pal::Accent : Pal::Dim));
    std::string hint = it.hint + (it.sub ? " ›" : "");
    int room_label = inner - 2 - (it.radio ? 2 : 0) - (hint.empty() ? 0 : width(hint) + 2);
    r.push_back(txt(clip(it.label, std::max(4, room_label)), !on ? Pal::Faint : it.danger ? Pal::Red : Pal::Fg, cur));
    r.push_back(filler());
    if (!it.hint.empty()) r.push_back(txt(it.hint, !on ? Pal::Faint : it.hint.size() <= 3 ? Pal::Accent : Pal::Dim, it.hint.size() <= 3));
    if (it.sub) r.push_back(txt(" ›", Pal::Accent, true));
    r.push_back(text(" "));
    Element row = side(cur ? hbox(std::move(r)) | bg(Pal::MenuSel) : hbox(std::move(r)));
    if (on) {
      Hit& h = hit(row);
      h.click = [pick, i] { pick(i); };
      h.hover = [hover, i] { hover(i); };
    }
    rows.push_back(row);
  }
  if (!m.error.empty()) rows.push_back(side(txt(" " + clip(m.error, inner - 2), Pal::Red)));
  if (!m.foot.empty()) {
    rows.push_back(divider());
    for (auto& f : m.foot) rows.push_back(side(txt(" " + clip(f, inner - 2), Pal::Dim)));
  }
  rows.push_back(txt("└" + repeat("─", inner) + "┘", Pal::Border));
  Element box = vbox(std::move(rows)) | bg(Pal::Menu) | clear_under;
  Hit& h = hit(box);
  h.click = [] {};  // inside the menu, not a click-away
  h.wheel = [this, depth](int dir) {
    if (depth + 1 == menus.size()) menu_key(dir > 0 ? Event::ArrowDown : Event::ArrowUp);
    else if (mode == Mode::Query) query_key(dir > 0 ? Event::ArrowDown : Event::ArrowUp);
  };
  return at(box, x, m.y);
}

// -- the PR list ------------------------------------------------------------------

Element Ui::render() {
  hits.clear();
  controls.clear();
  if (now() - last_refresh >= cfg.refresh_seconds) refresh();
  if (now() - last_watch >= cfg.watch_seconds) poll_watched();
  if (update_note.empty() && now() - last_update_check >= 15 * 60) check_update();
  std::string view_err;
  auto rs = rows(active, &view_err).value_or(std::vector<Pr>{});
  selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rs.size()) - 1));
  request_full(stubs_to_fill());

  Elements layers;
  if (mode == Mode::Settings) layers.push_back(render_settings());
  else if (mode == Mode::Reader) layers.push_back(render_reader());
  else layers.push_back(!menus.empty() && menus[0].x < 0 ? render_main(rs, view_err) | dim : render_main(rs, view_err));
  if (mode == Mode::Note) layers.push_back(render_note());
  if (mode == Mode::Query) {  // completions under the word being typed
    size_t ws = word_start(qedit.s, qedit.cur);
    auto comps = complete(qedit.s.substr(ws, qedit.cur - ws), seen(), view().is_list());
    if (!comps.empty()) {
      comp_sel = std::clamp(comp_sel, 0, static_cast<int>(comps.size()) - 1);
      Menu pop;
      pop.w = 38;
      pop.x = std::max(0, anchors["query"].x_min + 3 + width(qedit.s.substr(0, ws)) - 1);
      pop.y = anchors["query"].y_min + 1;
      pop.sel = comp_sel;
      pop.items = [comps] {
        std::vector<MenuItem> items;
        for (auto& c : comps) items.push_back({MenuItem::Normal, c.label, c.hint, [] {}});
        return items;
      };
      // Not on the menu stack: keys stay with the query row (Tab takes it).
      layers.push_back(render_menu(
          pop, menus.size(),
          [this](int i) {
            comp_sel = i;
            query_key(Event::Tab);
          },
          [this](int i) { comp_sel = i; }));
    }
  }
  if (!menus.empty()) {
    Element backdrop = emptyElement();
    hit(backdrop).backdrop = true;
    layers.push_back(backdrop);
    for (size_t d = 0; d < menus.size(); ++d) layers.push_back(render_menu(menus[d], d));
  }
  return dbox(std::move(layers));
}

Element Ui::render_main(const std::vector<Pr>& rs, const std::string& view_err) {
  auto term = Terminal::Size();
  int W = term.dimx, H = term.dimy;
  bool narrow = W < 100;
  Elements top = {render_tabbar(W, narrow), render_filterbar(W, narrow)};
  if (!narrow || mode == Mode::Query || modified(active)) top.push_back(render_queryrow(W));
  top.push_back(rule());
  const Pr* cur = rs.empty() ? nullptr : &rs[static_cast<size_t>(selected)];
  int bottom = cur ? 7 : 3;  // rule, 3 detail lines, rule, buttons, message
  list_rows = std::max(1, H - static_cast<int>(top.size()) - 1 - bottom);

  const View& v = view();
  int pi = v.is_list() ? plugin_index(v.plugin) : -1;
  Element body;
  if (v.is_list() && (pi < 0 || !plugin_up(static_cast<size_t>(pi))) && rs.empty())
    body = txt(" " + v.plugin + ": not running", Pal::Red) | center | flex;
  else if (rs.empty() && (!rows(active) || loading()))
    body = txt("loading…", Pal::Dim) | center | flex;
  else if (rs.empty())
    body = render_empty(W) | flex;
  else
    body = render_table(rs, list_rows, true) | flex;

  Elements all = std::move(top);
  all.push_back(body);
  all.push_back(rule());
  if (cur) {
    all.push_back(render_detail(*cur, W));
    all.push_back(rule());
  }
  all.push_back(render_buttons(cur, W));
  all.push_back(render_message(view_err, W));
  return vbox(std::move(all));
}

Element Ui::render_tabbar(int W, bool narrow) {
  Elements left = {txt(" anton ", Pal::Ink, true) | bg(Pal::Accent), text(" ")};
  Elements right;
  if (narrow) {
    right.push_back(clickable(txt(" ? ", Pal::Accent, true), [this] { open_palette(); }));
  } else {
    right.push_back(clickable(hbox({txt(",", Pal::Accent, true), txt(" settings", Pal::Dim)}), [this] { open_settings(); }));
    right.push_back(text("   "));
    right.push_back(clickable(hbox({txt("?", Pal::Accent, true), txt(" commands ", Pal::Dim)}), [this] { open_palette(); }));
  }
  int right_w = narrow ? 3 : 24, avail = W - 8 - 4 - right_w;

  struct T {
    std::string name, count;
    bool mod;
  };
  std::vector<T> ts;
  for (size_t i = 0; i < tabs.size(); ++i) {
    auto r = rows(i);
    ts.push_back({tabs[i].v.name, r ? std::to_string(r->size()) : "", modified(i)});
  }
  auto w_of = [&](size_t i) {
    return width(ts[i].name) + (ts[i].mod ? 1 : 0) + (ts[i].count.empty() ? 1 : 2 + width(ts[i].count)) + 2;
  };
  auto total = [&] {
    int t = 0;
    for (size_t i = 0; i < ts.size(); ++i) t += w_of(i);
    return t;
  };
  if (total() > avail)  // shorten the others: "Review requests" -> "Review", "Code review" -> "Code rev"
    for (size_t i = 0; i < ts.size(); ++i) {
      if (i == active) continue;
      std::string& n = ts[i].name;
      auto sp = n.find(' ');
      n = sp != std::string::npos && sp >= 5 ? n.substr(0, sp) : clip(n, 9);
      if (n.size() > 3 && n.substr(n.size() - 3) == "…") n = n.substr(0, n.size() - 3);
      while (!n.empty() && n.back() == ' ') n.pop_back();
    }
  size_t first = 0, last = ts.size();  // tabs that still don't fit scroll, the active one kept on
  while (total() > avail && last - first > 1) {
    int t = 0;
    for (size_t i = first; i < last; ++i) t += w_of(i);
    if (t <= avail - 4) break;
    if (active > first) ++first;
    else --last;
  }
  if (first > 0) left.push_back(txt("‹ ", Pal::Accent));
  for (size_t i = first; i < last; ++i) {
    bool on = i == active;
    Elements t = {text(" "), txt(ts[i].name, on ? Pal::Fg : Pal::Dim, on)};
    if (ts[i].mod) t.push_back(txt("•", Pal::Yellow, true));
    if (!ts[i].count.empty())
      t.push_back(txt(" " + ts[i].count, on ? Pal::Accent : ts[i].count == "0" ? Pal::Faint : Pal::Dim));
    t.push_back(text(" "));
    Element tab = hbox(std::move(t));
    if (on) tab = truecolor() ? tab | bg(Pal::Sel) : tab | inverted;
    tab = tab | reflect(anchors["tab:" + std::to_string(i)]);
    Hit& h = hit(tab);
    h.click = [this, i] { switch_view(i); };
    h.right = [this, i] { open_tab_menu(i); };
    left.push_back(tab);
    left.push_back(text(" "));
  }
  if (last < ts.size()) left.push_back(txt("› ", Pal::Accent));
  left.push_back(clickable(txt(" + ", Pal::Accent, true), [this] { new_view(); }));
  left.push_back(filler());
  for (auto& r : right) left.push_back(r);
  return hbox(std::move(left));
}

Element Ui::render_filterbar(int W, bool narrow) {
  const View& v = view();
  std::string q = mode == Mode::Query ? qedit.s : with_scope(v.query, scope);  // the chips follow the typing
  auto cs = chips(q);
  bool open = !menus.empty();
  auto anchored = [&](const std::string& key) { return open && menus[0].anchor == key; };

  std::string sort_label = v.sort_or_default() == Sort::List ? "list order" : sort_name(v.sort_or_default());
  Element sort_chip = hbox({txt(" sort ", Pal::Dim), txt(sort_label), txt(" ▾ ", Pal::Dim)}) |
                      bg(anchored("chip:sort") ? Pal::MenuSel : Pal::Chip) | reflect(anchors["chip:sort"]);
  sort_chip = clickable(sort_chip, [this] { open_sort_menu(); });
  int need = 1 + 9 + width(" sort " + sort_label + " ▾ ");  // margin, " + filter", sort
  if (v.is_list()) need += width(" list " + v.plugin + " · " + v.list + " ▾ ") + 1;
  for (auto& c : cs) need += width(" " + c.label + " " + c.value + " ▾ ") + 1;
  Element add = hbox({txt(" + ", Pal::Accent, true), txt(need < W ? "filter " : "filter", Pal::Dim)}) |
                bg(anchored("chip:+") ? Pal::MenuSel : Pal::Chip) | reflect(anchors["chip:+"]);
  add = clickable(add, [this] { open_add_filter(); });
  for (size_t i = cs.size(); i-- > 0 && need > W;)  // too wide: chips at their default go first
    if (!cs[i].set) {
      need -= width(" " + cs[i].label + " " + cs[i].value + " ▾ ") + 1;
      cs.erase(cs.begin() + static_cast<long>(i));
    }
  Elements row = {text(" ")};
  chips_folded = narrow || need > W;
  if (chips_folded) {  // still too wide: one chip for all of them
    int set = 0;
    std::string summary;
    for (auto& c : cs)
      if (c.set) {
        ++set;
        summary += (summary.empty() ? "" : " · ") + (c.fam == "state" || c.fam == "text" ? c.value : c.label + " " + c.value);
      }
    Element f = hbox({txt(" filters ", Pal::Dim), txt(std::to_string(set), set ? Pal::Cyan : Pal::Fg), txt(" ▾ ", Pal::Dim)}) |
                bg(anchored("chip:filters") ? Pal::MenuSel : set ? Pal::ChipOn : Pal::Chip) | reflect(anchors["chip:filters"]);
    row.push_back(clickable(f, [this] { open_folded_menu(); }));
    row.push_back(txt("  " + clip(summary, std::max(0, W - 40)), Pal::Dim));
    row.push_back(filler());
    row.push_back(sort_chip);
    return hbox(std::move(row));
  }
  if (v.is_list()) {
    Element l = hbox({txt(" list ", Pal::Dim), txt(v.plugin + " · " + v.list), txt(" ▾ ", Pal::Dim)}) |
                bg(anchored("chip:list") ? Pal::MenuSel : Pal::Chip) | reflect(anchors["chip:list"]);
    row.push_back(clickable(l, [this] { open_list_menu(); }));
    row.push_back(text(" "));
  }
  for (auto& c : cs) {
    std::string key = "chip:" + c.label + ":" + join(c.tokens);
    Pal back = anchored(key) ? Pal::MenuSel : c.set ? Pal::ChipOn : Pal::Chip;
    Element body = hbox({txt(" " + c.label + " ", Pal::Dim), txt(c.value, c.set ? Pal::Cyan : Pal::Fg)}) | bg(back);
    Element tail = txt(c.set ? " ✕ " : " ▾ ", c.set ? Pal::Cyan : Pal::Dim) | bg(back);
    body = clickable(body, [this, c] { open_chip_menu(c); });
    if (c.set) tail = clickable(tail, [this, c] { apply_query(without(with_scope(view().query, scope), c.tokens)); });
    else tail = clickable(tail, [this, c] { open_chip_menu(c); });
    row.push_back(hbox({body, tail}) | reflect(anchors[key]));
    row.push_back(text(" "));
  }
  row.push_back(add);
  row.push_back(filler());
  row.push_back(sort_chip);
  return hbox(std::move(row));
}

// Qualifier keys dim, values bright; at `cursor` (a byte offset), a block.
Element Ui::colored_query(const std::string& q, int cursor) {
  Elements out;
  std::string run;
  Pal run_pal = Pal::Fg;
  auto flush = [&] {
    if (!run.empty()) out.push_back(txt(run, run_pal));
    run.clear();
  };
  auto block = [](const std::string& s) {
    return text(s) | (truecolor() ? bgcolor(pal(Pal::Accent)) | color(pal(Pal::Ink)) : inverted);
  };
  bool quoted = false, in_key = false, at_start = true;
  for (size_t i = 0; i < q.size();) {
    size_t n = 1;
    while (i + n < q.size() && (static_cast<unsigned char>(q[i + n]) & 0xC0) == 0x80) ++n;
    char c = q[i];
    Pal p = Pal::Fg;
    if (c == ' ' && !quoted) {
      at_start = true;
      in_key = false;
    } else {
      if (at_start) {  // a token with a ':' before any space or quote starts with its key
        size_t colon = q.find(':', i);
        in_key = colon < q.find(' ', i) && colon < q.find('"', i);
        at_start = false;
      }
      if (c == '"') quoted = !quoted;
      p = in_key ? Pal::Dim : Pal::Fg;
      if (c == ':') in_key = false;
    }
    if (static_cast<int>(i) == cursor) {
      flush();
      out.push_back(block(q.substr(i, n)));
    } else {
      if (p != run_pal) flush();
      run_pal = p;
      run += q.substr(i, n);
    }
    i += n;
  }
  flush();
  if (cursor >= static_cast<int>(q.size())) out.push_back(block(" "));
  return hbox(std::move(out));
}

Element Ui::render_queryrow(int W) {
  if (mode == Mode::Query) {
    Elements r = {txt(" / ", Pal::Accent, true), colored_query(qedit.s, static_cast<int>(qedit.cur)), filler()};
    if (W >= 100)
      for (auto [k, l] : {std::pair{"Tab", " complete  "}, {"⏎", " apply  "}, {"esc", " cancel  "}, {"↑↓", " history "}}) {
        r.push_back(txt(k, Pal::Accent, true));
        r.push_back(txt(l, Pal::Dim));
      }
    return hbox(std::move(r)) | bg(Pal::Menu) | reflect(anchors["query"]);
  }
  const View& v = view();
  std::string q = with_scope(v.query, scope);
  bool wide = W >= 110;
  Elements right;
  if (modified(active)) {
    bool saved = tabs[active].saved.has_value();
    right.push_back(txt(wide ? "● " + clip(v.name, 20) + (saved ? " changed  " : " isn't saved  ") : "● ", Pal::Yellow));
    right.push_back(clickable(button("S", "Save"), [this] { save_view(active); }));
    right.push_back(text(" "));
    if (saved && wide) {
      right.push_back(clickable(button("", "Save as new view"), [this] { save_as_prompt(active); }));
      right.push_back(text(" "));
    }
    if (saved) right.push_back(clickable(button("", "Revert"), [this] { revert_view(active); }));
    else right.push_back(clickable(button("", "Discard"), [this] { delete_view(active); }));
    right.push_back(text(" "));
  } else if (wide) {
    right = {txt("click or ", Pal::Faint), txt("/", Pal::Accent, true), txt(" to edit ", Pal::Faint)};
  }
  int right_w = 0;
  for (auto& e : right) {
    e->ComputeRequirement();
    right_w += e->requirement().min_x;
  }
  Element left = hbox({txt(" / ", Pal::Accent, true), colored_query(clip(q, std::max(8, W - right_w - 5)))});
  Elements r = {clickable(left, [this] { start_query(); })};
  if (v.is_list()) {
    std::string note = "   applied to the PRs the " + v.plugin + " plugin lists";
    if (width(q) + 4 + width(note) + right_w < W) r.push_back(txt(note, Pal::Dim));
  }
  r.push_back(filler());
  for (auto& e : right) r.push_back(e);
  return hbox(std::move(r)) | reflect(anchors["query"]);
}

void Ui::start_query() {
  if (mode == Mode::Reader || mode == Mode::Settings) mode = Mode::Normal;
  qedit.set(with_scope(view().query, scope));
  comp_sel = 0;
  hist_pos = -1;
  mode = Mode::Query;
}

bool Ui::query_key(const Event& e) {
  size_t ws = word_start(qedit.s, qedit.cur);
  auto comps = complete(qedit.s.substr(ws, qedit.cur - ws), seen(), view().is_list());
  if (e == Event::Return) {
    std::string q = join(tokenize(qedit.s));
    if (history.empty() || history.back() != q) history.push_back(q);
    mode = Mode::Normal;
    apply_query(q);
  } else if (e == Event::Escape) {
    mode = Mode::Normal;
  } else if (e == Event::Tab) {
    if (!comps.empty()) {
      const std::string& tok = comps[static_cast<size_t>(std::clamp(comp_sel, 0, static_cast<int>(comps.size()) - 1))].token;
      bool more = tok.back() == ':';
      qedit.s.replace(ws, qedit.cur - ws, tok + (more ? "" : " "));
      qedit.cur = ws + tok.size() + (more ? 0 : 1);
      comp_sel = 0;
    }
  } else if ((e == Event::ArrowDown || e == Event::ArrowUp) && !comps.empty()) {
    int n = static_cast<int>(comps.size());
    comp_sel = (comp_sel + (e == Event::ArrowDown ? 1 : n - 1)) % n;
  } else if (e == Event::ArrowUp) {
    if (history.empty()) return true;
    hist_pos = hist_pos < 0 ? static_cast<int>(history.size()) - 1 : std::max(0, hist_pos - 1);
    qedit.set(history[static_cast<size_t>(hist_pos)]);
  } else if (e == Event::ArrowDown) {
    if (hist_pos < 0) return true;
    if (++hist_pos >= static_cast<int>(history.size())) {
      hist_pos = -1;
      qedit.set(with_scope(view().query, scope));
    } else {
      qedit.set(history[static_cast<size_t>(hist_pos)]);
    }
  } else if (qedit.key(e)) {
    comp_sel = 0;
  }
  return true;
}

Element Ui::state_cell(const Shown& s) {
  Element e = cell(s.text, 12);
  switch (s.kind) {
    case Kind::Merged: return e | color(pal(Pal::Magenta));
    case Kind::Done: return e | color(pal(Pal::Dim));
    case Kind::Conflict: return e | bold | color(pal(Pal::Red));
    case Kind::Custom: return e | bold | color(pal(Pal::Cyan));
    case Kind::Draft: return e | color(pal(Pal::Dim));
    case Kind::Failing: return e | color(pal(Pal::Red));
    case Kind::Changes: return e | color(pal(Pal::Yellow));
    case Kind::Approved: return e | color(pal(Pal::Green));
    case Kind::Mine: return e | color(pal(Pal::Green));
    case Kind::Review: return e;
  }
  return e;
}

Element Ui::tags_cell(const std::vector<Tag>& tags, int w) {
  Elements parts;
  int used = 0;
  size_t shown = 0;
  for (auto& t : tags) {
    std::string one = t.glyph.empty() ? t.text : t.glyph + " " + t.text;
    int add = width(one) + (shown ? 1 : 0);
    int rest = static_cast<int>(tags.size() - shown - 1);
    if (used + add + (rest > 0 ? 3 : 0) > w && shown > 0) break;
    if (shown) parts.push_back(text(" "));
    parts.push_back(paint(text(one), tag_person(t)));
    used += add;
    ++shown;
  }
  if (shown < tags.size()) parts.push_back(txt(" +" + std::to_string(tags.size() - shown), Pal::Dim));
  return hbox(std::move(parts)) | size(WIDTH, EQUAL, w);
}

Element Ui::author_cell(const std::string& login) { return paint(cell(who(login), 12), person(login)); }

void Ui::start_column_workers() {
  if (column_workers) return;
  column_workers = true;
  for (int i = 0; i < kColumnJobs; ++i) std::thread([this] { column_worker(); }).detach();
}

// Runs a command column for a PR off the UI thread; the cell fills in when
// it's done.
void Ui::run_column(const Column& col, const Pr& p, ColumnCell& cc) {
  start_column_workers();
  cc.running = true;
  cc.gen = column_gen;
  cc.updated = p.updated;
  nlohmann::json j = pr_json(p);
  std::vector<std::string> argv;
  for (auto& a : col.command) argv.push_back(fill(a, j));
  std::pair<std::string, std::string> key{col.name, p.url};
  {
    std::lock_guard lk(col_mu);
    col_jobs.push_back([this, argv, key, input = j.dump() + "\n", timeout = col.timeout] {
      // ponytail: quitting anton mid-run leaves a hung command to finish on
      // its own; track pids and kill them on exit if that ever bites.
      Result r = run(argv, input, true, timeout);
      std::string err = r.timed_out  ? "timed out after " + std::to_string(timeout) + "s"
                        : r.code < 0 ? r.err
                        : r.code != 0 ? "exit " + std::to_string(r.code) + ": " + cell_text(r.err)
                                      : "";
      std::string out = err.empty() ? cell_text(r.out) : "";
      post([this, key, out, err] {
        ColumnCell& cc = column_cells[key];
        cc.running = false;
        cc.done = true;
        cc.error = err;
        if (err.empty()) cc.text = out;
      });
    });
  }
  col_cv.notify_one();
}

void Ui::column_worker() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lk(col_mu);
      col_cv.wait(lk, [&] { return !col_jobs.empty(); });
      job = std::move(col_jobs.front());
      col_jobs.pop_front();
    }
    job();
  }
}

Element Ui::command_cell(const Column& col, const Pr& p) {
  if (p.partial) return cell("", col.width);  // its fields aren't in yet
  ColumnCell& cc = column_cells[{col.name, p.url}];
  if (!cc.running && (cc.gen != column_gen || cc.updated != p.updated)) run_column(col, p, cc);
  if (!cc.done) return cell("…", col.width) | color(pal(Pal::Dim));
  if (!cc.error.empty()) return cell("✗", col.width) | color(pal(Pal::Red));
  return cell(cc.text, col.width);
}

// The table: rows [list_top, list_top + height) when live (the PR list),
// all of rs otherwise (the settings preview, which takes no clicks).
Element Ui::render_table(const std::vector<Pr>& rs, int height, bool live) {
  std::set<std::string> row_owners;
  int num_w = 2;
  for (auto& p : rs) {
    row_owners.insert(p.owner);
    num_w = std::max(num_w, static_cast<int>(std::to_string(p.number).size()));
  }
  bool show_owner = row_owners.size() > 1;
  Sort sort = view().sort_or_default();
  int repo_w = sort == Sort::Repo ? 6 : 4;  // "REPO ↓"
  for (auto& p : rs) repo_w = std::max(repo_w, std::min(24, width(show_owner ? p.repo : short_repo(p.repo))));
  const int tags_w = 13;

  // gap: spaces after a column (STATE's text never fills its 12, AGE is
  // right-aligned against it).
  auto gap = [](const Column& c) {
    return c.name == "state" || c.name == "title" ? 0 : c.name == "author" || c.name == "tags" ? 1 : 2;
  };
  std::vector<const Column*> cols;
  for (auto& c : cfg.columns)
    if (!c.hidden) cols.push_back(&c);
  // A narrow terminal leaves TITLE some room: #, then TAGS, then AGE step aside.
  auto cell_w = [&](const Column& c) {
    const std::string& m = c.name;
    int w = !c.command.empty() ? c.width : m == "repo" ? repo_w : m == "number" ? num_w : m == "size" ? 11
            : m == "state" ? 12 : m == "age" ? 4 : m == "author" ? 12 : m == "tags" ? tags_w : 0;
    return w + gap(c);
  };
  bool has_title = std::any_of(cols.begin(), cols.end(), [](const Column* c) { return c->command.empty() && c->name == "title"; });
  for (const char* drop : {"number", "tags", "age"}) {
    int used = 3;  // the selection mark and the scrollbar
    for (auto* c : cols) used += cell_w(*c);
    if (!has_title || Terminal::Size().dimx - used >= 20) break;
    cols.erase(std::remove_if(cols.begin(), cols.end(), [&](const Column* c) { return c->command.empty() && c->name == drop; }),
               cols.end());
  }
  bool flex_col = false;
  Elements head = {text("  ")};
  for (auto* c : cols) {
    std::string n = c->command.empty() ? c->name : "";
    // the header sorts by it: REPO, SIZE, AGE (created); List stands for none
    Sort by = n == "repo" ? Sort::Repo : n == "size" ? Sort::Size : n == "age" ? Sort::Created : Sort::List;
    bool sorted = by != Sort::List && by == sort;
    std::string arrow = sorted ? " ↓" : "";
    Element h = !c->command.empty() ? cell(c->name, c->width)
                : n == "repo"       ? cell("REPO" + arrow, repo_w)
                : n == "number"     ? cell_right("#", num_w)
                : n == "size"       ? cell("SIZE" + arrow, 11)
                : n == "state"      ? cell("STATE", 12)
                : n == "age"        ? cell_right(sorted ? "↓AGE" : "AGE", 4)  // right-aligned: the arrow goes first
                : n == "author"     ? cell("AUTHOR", 12)
                : n == "tags"       ? cell("TAGS", tags_w)
                                    : text("TITLE") | flex;
    h = sorted ? h | color(pal(Pal::Fg)) | bold : h | color(pal(Pal::Dim));
    if (live && by != Sort::List) h = clickable(h, [this, by] { set_sort(by); });
    head.push_back(h);
    flex_col = flex_col || n == "title";
    head.push_back(text(std::string(static_cast<size_t>(gap(*c)), ' ')));
  }
  if (!flex_col) head.push_back(filler());
  head.push_back(text(" "));

  int n = static_cast<int>(rs.size()), first = 0, last = n;
  if (live) {
    list_top = std::clamp(list_top, selected - height + 1, selected);
    list_top = std::clamp(list_top, 0, std::max(0, n - height));
    first = list_top;
    last = std::min(n, list_top + height);
  }
  int thumb = n > height ? std::max(1, height * height / n) : 0;
  int thumb_at = n > height ? first * (height - thumb) / std::max(1, n - height) : 0;
  Elements lines = {hbox(std::move(head))};
  std::time_t t = now();
  for (int i = first; i < last; ++i) {
    const Pr& p = rs[static_cast<size_t>(i)];
    bool sel = live && i == selected, done = p.state != "OPEN";
    Elements row = {sel ? txt("▌ ", Pal::Accent) : text("  ")};
    for (auto* c : cols) {
      const std::string& name = c->name;
      if (!c->command.empty()) {
        row.push_back(command_cell(*c, p));
      } else if (name == "repo") {
        row.push_back(cell(show_owner ? p.repo : short_repo(p.repo), repo_w));
      } else if (name == "number") {
        row.push_back(cell_right(std::to_string(p.number), num_w) | color(pal(Pal::Dim)));
      } else if (name == "size") {
        Pal sc = Pal::Green;
        switch (size_band(p.additions + p.deletions, cfg.green_below, cfg.red_from)) {
          case Band::Green: sc = Pal::Green; break;
          case Band::Yellow: sc = Pal::Yellow; break;
          case Band::Red: sc = Pal::Red; break;
        }
        row.push_back(p.partial ? cell("", 11)
                                : hbox({cell_right("+" + compact(p.additions), 5), text(" "),
                                        cell("-" + compact(p.deletions), 5)}) | color(pal(sc)));
      } else if (name == "state") {
        row.push_back(state_cell(derive_state(p, status_of(p.url))));
      } else if (name == "age") {
        row.push_back(cell_right(p.created ? age(static_cast<long>(t - p.created)) : "", 4) | color(pal(Pal::Dim)));
      } else if (name == "author") {
        row.push_back(author_cell(p.author));
      } else if (name == "tags") {
        row.push_back(tags_cell(tags_of(p.url), tags_w));
      } else {
        row.push_back(txt(p.partial ? "…" : shown_title(p.title), done ? Pal::Dim : Pal::Fg, sel) | flex);
      }
      row.push_back(text(std::string(static_cast<size_t>(gap(*c)), ' ')));
    }
    if (!flex_col) row.push_back(filler());
    int k = i - first;
    row.push_back(n > height ? txt(k >= thumb_at && k < thumb_at + thumb ? "┃" : "│", k >= thumb_at && k < thumb_at + thumb ? Pal::Dim : Pal::Faint)
                             : text(" "));
    Element line = hbox(std::move(row));
    if (sel) line = truecolor() ? line | bg(Pal::Sel) : line | inverted;
    if (live) {
      Hit& h = hit(line);
      h.click = [this, i] { selected = i; };
      h.dbl = [this, i] {
        auto now_rows = rows(active).value_or(std::vector<Pr>{});
        if (i < static_cast<int>(now_rows.size())) open_reader(now_rows[static_cast<size_t>(i)]);
      };
    }
    lines.push_back(line);
  }
  Element table = vbox(std::move(lines));
  if (live)
    hit(table).wheel = [this, n](int dir) {
      list_top = std::clamp(list_top + 3 * dir, 0, std::max(0, n - list_rows));
      selected = std::clamp(selected, list_top, std::min(n - 1, list_top + list_rows - 1));
    };
  return table;
}

Element Ui::render_detail(const Pr& p, int W) {
  Element one = fit({{" " + short_repo(p.repo) + "#" + std::to_string(p.number) + "  ", Pal::Dim}}, W);
  one = hbox({one, txt(clip(p.partial ? "…" : shown_title(p.title), std::max(4, W - width(short_repo(p.repo)) - 10)), Pal::Fg, true)});
  std::vector<Part> two;
  if (p.partial) {
    two.push_back({"loading…", Pal::Dim});
  } else {
    two.push_back({who(p.author), Pal::Fg, person(p.author)});
    two.push_back({" · opened " + age(static_cast<long>(now() - p.created)) + " ago · ", Pal::Dim});
    Band band = size_band(p.additions + p.deletions, cfg.green_below, cfg.red_from);
    two.push_back({"+" + std::to_string(p.additions) + " −" + std::to_string(p.deletions),
                   band == Band::Red ? Pal::Red : band == Band::Yellow ? Pal::Yellow : Pal::Green});
    if (p.changed_files)
      two.push_back({" in " + std::to_string(p.changed_files) + (p.changed_files == 1 ? " file" : " files"), Pal::Dim});
    if (p.checks == "SUCCESS") two.push_back({" · checks passing", Pal::Dim});
    else if (p.checks == "FAILURE" || p.checks == "ERROR") two.push_back({" · checks failing", Pal::Red});
    else if (!p.checks.empty()) two.push_back({" · checks running", Pal::Yellow});
    std::pair<std::string, Pal> review = p.state == "MERGED"                        ? std::pair{"merged", Pal::Magenta}
                                         : p.state == "CLOSED"                      ? std::pair{"closed", Pal::Dim}
                                         : p.mergeable == "CONFLICTING"             ? std::pair{"⚠ conflict", Pal::Red}
                                         : p.draft                                  ? std::pair{"draft", Pal::Dim}
                                         : p.review_decision == "APPROVED"          ? std::pair{"approved", Pal::Green}
                                         : p.review_decision == "CHANGES_REQUESTED" ? std::pair{"changes requested", Pal::Yellow}
                                                                                    : std::pair{"review required", Pal::Dim};
    two.push_back({" · ", Pal::Dim});
    two.push_back({review.first, review.second});
    std::string ls;
    for (auto& l : p.labels) ls += (ls.empty() ? " · " : ", ") + l;
    two.push_back({ls, Pal::Cyan});
  }
  std::string url = W >= 100 ? no_scheme(p.url) + " " : "";
  Element line2 = hbox({text(" "), fit(two, W - width(url) - 3), filler(),
                        clickable(txt(url, Pal::Faint), [this, u = p.url] { open(u); })});

  std::vector<Part> three;
  for (size_t i = 0; i < live.size(); ++i) {
    auto it = live[i].prs.find(p.url);
    if (it == live[i].prs.end() || (!it->second.status && it->second.tags.empty())) continue;
    three.push_back({cfg.plugins[i].name + " ", Pal::Dim});
    if (it->second.status) three.push_back({it->second.status->glyph + " " + it->second.status->text + " ", Pal::Cyan});
    for (auto& t : it->second.tags) {
      three.push_back({(t.glyph.empty() ? "" : t.glyph + " ") + t.text, Pal::Fg, tag_person(t)});
      three.push_back({(t.hint.empty() ? "" : " " + t.hint) + " ", Pal::Dim});
    }
    three.push_back({"    "});
  }
  for (auto& c : cfg.columns)
    if (auto it = column_cells.find({c.name, p.url}); !c.command.empty() && it != column_cells.end() && !it->second.error.empty())
      three.push_back({"[" + c.name + "] ✗ " + it->second.error + "    ", Pal::Red});
  bool note = has_note(p.url);
  Elements line3 = {text(" "), fit(three, W - (note ? 14 : 2))};
  if (note) line3.push_back(clickable(txt("n read note", Pal::Cyan), [this, p] { act("n", &p); }));
  return vbox({one, line2, hbox(std::move(line3))});
}

Element Ui::render_buttons(const Pr* cur, int W) {
  std::vector<std::pair<std::string, std::string>> items;
  if (cur) {
    items = {{"⏎", "Read"}, {"w", "Browser"}, {"y", "Copy URL"}};
    if (has_note(cur->url)) items.push_back({"n", "Note"});
    for (auto& [i, a] : plugin_keys())
      items.push_back({a.key, capital(!a.button.empty() ? a.button : a.label.empty() ? a.id : a.label)});
  } else {
    items = {{"?", "Commands"}, {"r", "Refresh"}, {",", "Settings"}};
  }
  Elements right;
  if (W >= 100)
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      right.push_back(txt(cfg.plugins[i].name + " ", Pal::Dim));
      right.push_back(txt("●  ", plugin_up(i) ? Pal::Green : Pal::Red));
    }
  std::string fresh = loading()         ? "↻ loading…"
                      : last_fetch == 0 ? ""
                      : now() - last_fetch < 60 ? "↻ just now"
                                                : "↻ " + age(static_cast<long>(now() - last_fetch)) + " ago";
  right.push_back(txt(" " + fresh + " ", Pal::Dim));
  int right_w = 0;
  for (auto& e : right) {
    e->ComputeRequirement();
    right_w += e->requirement().min_x;
  }
  int full = 1;
  for (auto& [k, l] : items) full += width(k) + width(l) + 4;
  bool keys_only = full > W - right_w;  // narrow: just the keys
  Elements row = {text(" ")};
  for (auto& [k, l] : items) {
    if (keys_only && width(k) + 3 > W - right_w - 2) break;
    row.push_back(clickable(button(k, keys_only ? "" : clip(l, 16)), [this, key = k] {
      auto rs = rows(active).value_or(std::vector<Pr>{});
      const Pr* c = rs.empty() ? nullptr : &rs[static_cast<size_t>(std::clamp(selected, 0, static_cast<int>(rs.size()) - 1))];
      act(key, c);
    }));
    row.push_back(text(" "));
  }
  row.push_back(filler());
  for (auto& e : right) row.push_back(e);
  return hbox(std::move(row));
}

Element Ui::render_message(const std::string& view_err, int W) {
  Elements row;
  if (now() < notice_until && !notice.empty()) {
    row.push_back(txt(" " + notice, notice_bad ? Pal::Red : Pal::Cyan));
  } else if (std::string e = !view_err.empty() ? view_err : error; !e.empty()) {
    row.push_back(txt(" " + e, Pal::Red));
  } else if (mode == Mode::Query) {
    row.push_back(txt(" chips and query are the same filter: ", Pal::Dim));
    row.push_back(txt("edit either one and the other follows"));
  } else if (modified(active) && tabs[active].saved) {
    row.push_back(txt(" Save writes this view into config.json; the rest of the file stays as it is", Pal::Dim));
  } else if (W >= 100) {
    row.push_back(txt(" tip ", Pal::Accent));
    row.push_back(txt(cfg.mouse ? "click a header to sort · wheel scrolls · double-click reads · right-click a tab for options"
                                : "? lists every command and its key · , opens settings",
                      Pal::Dim));
  }
  row.push_back(filler());
  if (!update_note.empty()) row.push_back(txt("⬆ " + update_note + " ", Pal::Green));
  return hbox(std::move(row));
}

// Nothing matches: say so, show the query, offer the changes that would
// show something.
Element Ui::render_empty(int W) {
  std::string q = with_scope(view().query, scope);
  bool open = has_token(q, "is:open");
  Elements fixes;
  std::string last;
  for (auto& t : tokenize(q))
    if (family(t) != "state" && family(t) != "pr") last = t;
  if (!last.empty()) {
    std::string fam = family(last);
    fixes.push_back(clickable(button("", "Drop " + (fam == "text" ? std::string("\"") + clip(last, 12) + "\"" : fam)),
                              [this, last] { apply_query(without(with_scope(view().query, scope), {last})); }));
    fixes.push_back(text("  "));
  }
  if (open) {
    fixes.push_back(clickable(button("", "Include closed"), [this] {
      apply_query(without(with_scope(view().query, scope), {"is:open"}));
    }));
    fixes.push_back(text("  "));
  }
  fixes.push_back(clickable(button("/", "Edit query"), [this] { start_query(); }));
  Elements v = {filler(), txt(open ? "No open PRs match this view." : "No PRs match this view.", Pal::Fg, true) | hcenter,
                text(""), colored_query(clip(q, W - 4)) | hcenter, text(""), text(""), hbox(std::move(fixes)) | hcenter};
  if (modified(active) && tabs[active].saved) {
    v.push_back(text(""));
    v.push_back(text(""));
    v.push_back(hbox({txt("or ", Pal::Dim), clickable(txt("Revert", Pal::Accent) | underlined, [this] { revert_view(active); }),
                      txt(" to what " + view().name + " had saved", Pal::Dim)}) |
                hcenter);
  }
  v.push_back(filler());
  return vbox(std::move(v));
}

// -- events ----------------------------------------------------------------------

bool Ui::on_event(Event e) {
  if (e == Event::CtrlC) {  // quits from any mode, through Loop's normal exit
    screen.Exit();
    return true;
  }
  if (e == Event::Custom) return false;
  if (e.is_mouse()) return on_mouse(e.mouse());
  if (!menus.empty()) return menu_key(e);
  switch (mode) {
    case Mode::Reader: return reader_key(e);
    case Mode::Note: return note_key(e);
    case Mode::Query: return query_key(e);
    case Mode::Settings: return settings_key(e);
    case Mode::Normal: return normal_key(e);
  }
  return false;
}

// The last frame's hits, topmost layer first. Within a layer the smallest
// box with the handler wins (a ✕ inside its chip, a row inside the table).
// An open menu's backdrop splits layers: a click on it closes the menus and
// goes on to what's under it; wheel and hover stop there.
bool Ui::on_mouse(Mouse m) {
  if (!cfg.mouse) return false;
  enum { Click, Right, Wheel, Hover } kind;
  if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
    if (m.motion != Mouse::Pressed) return true;
    kind = Wheel;
  } else if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
    kind = Click;
  } else if (m.button == Mouse::Right && m.motion == Mouse::Pressed) {
    kind = Right;
  } else if (m.motion == Mouse::Moved) {
    if (m.button == Mouse::Left && drag_group >= 0) {  // dragging a row to reorder
      for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (it->drag == drag_group && it->index != drag_index && it->drop && it->box.Contain(m.x, m.y)) {
          auto drop = it->drop;
          int to = it->index;
          drop(drag_index);
          drag_index = to;
          break;
        }
      return true;
    }
    kind = Hover;
  } else {
    if (m.motion == Mouse::Released) drag_group = -1;
    return true;
  }
  auto has = [&](const Hit& h) {
    return kind == Click ? bool(h.click) : kind == Right ? bool(h.right) : kind == Wheel ? bool(h.wheel) : bool(h.hover);
  };
  auto area = [](const Box& b) { return (b.x_max - b.x_min + 1) * (b.y_max - b.y_min + 1); };
  auto it = hits.rbegin();
  while (it != hits.rend()) {
    Hit* best = nullptr;
    for (; it != hits.rend() && !it->backdrop; ++it)
      if (it->box.Contain(m.x, m.y) && has(*it) && (!best || area(it->box) < area(best->box))) best = &*it;
    if (best) {
      Hit h = *best;  // the handler may change what hits holds
      if (kind == Click) {
        auto t = std::chrono::steady_clock::now();
        bool dbl = h.dbl && t - last_click < std::chrono::milliseconds(400) && m.y == last_click_y &&
                   std::abs(m.x - last_click_x) <= 2;
        last_click = dbl ? std::chrono::steady_clock::time_point{} : t;
        last_click_x = m.x;
        last_click_y = m.y;
        if (h.drag >= 0) {
          drag_group = h.drag;
          drag_index = h.index;
        }
        dbl ? h.dbl() : h.click();
      } else if (kind == Right) {
        h.right();
      } else if (kind == Wheel) {
        h.wheel(m.button == Mouse::WheelDown ? 1 : -1);
      } else {
        h.hover();
      }
      return true;
    }
    if (it == hits.rend()) break;
    if (kind != Click && kind != Right) return true;  // wheel/hover don't reach under a menu
    close_menus();
    ++it;
  }
  return true;
}

void Ui::act(const std::string& key, const Pr* cur) {
  if (key == "⏎") {
    if (cur) open_reader(*cur);
  } else if (key == "w") {
    if (cur) open(cur->url);
  } else if (key == "y") {
    if (cur) copy(cur->url);
  } else if (key == "n") {
    if (cur && has_note(cur->url)) {
      note_url = cur->url;
      note_scroll = 0;
      mode = Mode::Note;
    } else if (cur) {
      flash("no note on this PR");
    }
  } else if (key == "?") {
    open_palette();
  } else if (key == "r") {
    refresh();
  } else if (key == ",") {
    open_settings();
  } else if (key == "s") {  // list order only exists for list views
    Sort s = view().sort_or_default();
    do s = static_cast<Sort>((static_cast<int>(s) + 1) % 5);
    while (s == Sort::List && !view().is_list());
    set_sort(s);
  } else if (key == "x") {
    scope.clear();
    view().query = remove_qualifiers(view().query, kScopeKeys);
    selected = list_top = 0;
    refresh();
  } else if (cur) {
    plugin_action(key, cur->url);
  }
}

bool Ui::normal_key(const Event& e) {
  auto rs = rows(active).value_or(std::vector<Pr>{});
  int last = static_cast<int>(rs.size()) - 1;
  const Pr* cur = rs.empty() ? nullptr : &rs[static_cast<size_t>(std::clamp(selected, 0, last))];
  auto ch = [&](const char* c) { return e == Event::Character(c); };

  if (ch("q")) screen.Exit();
  else if (ch("j") || e == Event::ArrowDown) selected = std::min(selected + 1, std::max(0, last));
  else if (ch("k") || e == Event::ArrowUp) selected = std::max(selected - 1, 0);
  else if (e == Event::PageDown) selected = std::min(selected + list_rows, std::max(0, last));
  else if (e == Event::PageUp) selected = std::max(selected - list_rows, 0);
  else if (ch("g") || e == Event::Home) selected = 0;
  else if (ch("G") || e == Event::End) selected = std::max(0, last);
  else if (e == Event::Return) act("⏎", cur);
  else if (ch("w") || ch("y") || ch("n") || ch("r") || ch("s") || ch("x") || ch("?") || ch(",")) act(e.character(), cur);
  else if (ch("/")) start_query();
  else if (ch("o") || ch("p")) open_picker(ch("p"));
  else if (ch("f")) open_add_filter();
  else if (ch("S")) save_view(active);
  else if (ch("m")) open_tab_menu(active);
  else if (ch("<")) move_view(active, -1);
  else if (ch(">")) move_view(active, 1);
  else if (ch("+")) new_view();
  else if (ch("e")) edit_config();
  else if (e == Event::Tab) switch_view((active + 1) % tabs.size());
  else if (e == Event::TabReverse) switch_view((active + tabs.size() - 1) % tabs.size());
  else if (e.is_character() && e.character().size() == 1 && e.character()[0] >= '1' && e.character()[0] <= '9')
    switch_view(static_cast<size_t>(e.character()[0] - '1'));
  else if (e.is_character() && plugin_action(e.character(), cur ? cur->url : "")) {
  } else return false;
  return true;
}

void run_ui(Config cfg) {
  auto ui = std::make_unique<Ui>(std::move(cfg));
  Ui* u = ui.get();

  std::thread([u] { u->worker(); }).detach();
  std::thread([u] {  // ticks the clock: "↻ 40s ago" and the auto refresh
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      u->screen.PostEvent(Event::Custom);
    }
  }).detach();

  if (!u->cfg.plugins.empty()) {
    u->plugins = std::make_unique<Plugins>(
        u->cfg.plugins,
        [u](size_t i, const std::string& line, bool is_err) {
          u->post([u, i, line, is_err] { u->on_plugin_line(i, line, is_err); });
        },
        [u](size_t i, int code) { u->post([u, i, code] { u->on_plugin_exit(i, code); }); });
    u->plugins->start();
  }

  auto root = Renderer([u] { return u->render(); }) | CatchEvent([u](Event e) { return u->on_event(e); });
  // Ctrl-C is a key event (handled above) so the loop exits normally and
  // stops the plugins below. SIGTERM/SIGHUP kill anton from outside: FTXUI
  // restores the terminal, then re-raises into this handler, which takes the
  // plugins down with it -- they live in their own process groups and would
  // otherwise outlive anton.
  u->screen.ForceHandleCtrlC(false);
  u->screen.TrackMouse(u->cfg.mouse);  // off: the terminal selects text as usual
  for (int sig : {SIGTERM, SIGHUP}) {
    struct sigaction sa {};
    sa.sa_handler = [](int s) {
      Plugins::kill_all_from_signal();
      ::signal(s, SIG_DFL);
      ::raise(s);
    };
    sigaction(sig, &sa, nullptr);
  }
  u->screen.Loop(root);

  if (u->plugins) u->plugins->stop();
  std::fflush(stdout);
  std::_Exit(0);
}

}  // namespace anton
