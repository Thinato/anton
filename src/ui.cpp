#include "ui.hpp"

#include <signal.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "github.hpp"
#include "model.hpp"
#include "plugins.hpp"
#include "proc.hpp"
#include "update.hpp"

namespace anton {
namespace {

using namespace ftxui;

enum class Mode { Normal, Query, Picker, Help };

std::time_t now() { return std::time(nullptr); }

// Case-insensitive subsequence match: "bcore" finds "brick-so/core".
bool fuzzy(const std::string& hay, const std::string& needle) {
  size_t i = 0;
  for (char c : hay)
    if (i < needle.size() && std::tolower(static_cast<unsigned char>(c)) ==
                                 std::tolower(static_cast<unsigned char>(needle[i])))
      ++i;
  return i == needle.size();
}

Element cell(const std::string& s, int w) { return text(s) | size(WIDTH, EQUAL, w); }

Element cell_right(const std::string& s, int w) {
  return hbox(filler(), text(s)) | size(WIDTH, EQUAL, w);
}

struct Ui {
  Config cfg;
  App screen = App::Fullscreen();

  // -- state, touched only on the UI thread ---------------------------------
  std::vector<View> views;
  size_t active = 0;
  std::string scope;  // "org:x" / "repo:o/n" / ""
  std::vector<Sort> sorts;  // per view
  std::map<std::string, Pr> cache;                           // by URL
  std::map<std::string, std::vector<std::string>> results;   // search query -> URLs
  std::set<std::string> requested;                           // URLs ever asked for
  std::set<std::string> owners, repos;                       // every one seen: picker source
  std::string viewer;
  std::time_t last_fetch = 0, last_refresh = 0, last_watch = 0, last_update_check = 0;
  std::string update_note;  // stays in the footer once set
  std::string error, notice;
  std::time_t notice_until = 0;
  int selected = 0;
  Mode mode = Mode::Normal;
  std::string query_buf, pick_buf;
  int query_cursor = 0, pick_sel = 0;
  bool pick_repo = false;
  Component query_input, pick_input;

  // Per plugin. Long-running ("exit") plugins fold lines into `live` as they
  // come; one-shot ones fold into `pending`, swapped in when the run ends, so
  // their tags don't blink out between runs.
  std::vector<PluginState> live, pending;
  std::vector<bool> running;
  std::vector<int> last_code;
  std::unique_ptr<Plugins> plugins;

  // -- fetch worker queue (mu) ----------------------------------------------
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> searches;
  std::set<std::string> want_urls;  // state first, full fields if open
  std::set<std::string> want_full;  // full fields regardless (stubs a view shows)
  bool busy = false;
  std::set<std::string> requested_full;  // UI thread
  std::vector<std::set<std::string>> approvals;  // per plugin: new approvals not yet delivered

  explicit Ui(Config c) : cfg(std::move(c)), views(cfg.views) {
    for (auto& v : views) sorts.push_back(v.is_list() ? Sort::List : Sort::Updated);
    live.resize(cfg.plugins.size());
    pending.resize(cfg.plugins.size());
    running.assign(cfg.plugins.size(), true);
    last_code.assign(cfg.plugins.size(), 0);
    approvals.resize(cfg.plugins.size());
    InputOption qo;
    qo.content = &query_buf;
    qo.multiline = false;
    qo.cursor_position = &query_cursor;
    query_input = Input(qo);
    InputOption po;
    po.content = &pick_buf;
    po.multiline = false;
    po.on_change = [this] { pick_sel = 0; };
    pick_input = Input(po);
  }

  // Runs fn on the UI thread and redraws. Safe from any thread.
  void post(std::function<void()> fn) {
    screen.Post(std::move(fn));
    screen.PostEvent(Event::Custom);
  }

  // -- fetching ---------------------------------------------------------------

  std::string effective(const View& v) const {
    std::string q = with_scope(v.query, scope);
    return v.is_list() ? q : ensure_is_pr(q);
  }

  void request_search(const std::string& q) {
    std::lock_guard lk(mu);
    if (std::find(searches.begin(), searches.end(), q) == searches.end()) searches.push_back(q);
    busy = true;
    cv.notify_one();
  }

  void request_urls(const std::vector<std::string>& urls) {
    if (urls.empty()) return;
    std::lock_guard lk(mu);
    for (auto& u : urls) {
      want_urls.insert(u);
      requested.insert(u);
    }
    busy = true;
    cv.notify_one();
  }

  void request_full(const std::vector<std::string>& urls) {
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
  void poll_watched() {
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

  bool loading() {
    std::lock_guard lk(mu);
    return busy;
  }

  void worker() {
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
          owners.insert(pr.owner);
          repos.insert(pr.repo);
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
          }
          else
            cache[pr.url] = pr;
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

  std::vector<std::string> list_urls(const View& v) const {
    std::vector<std::string> out;
    int pi = plugin_index(v.plugin);
    if (pi < 0) return out;
    for (auto& [url, info] : live[static_cast<size_t>(pi)].prs)
      if (info.lists.count(v.list)) out.push_back(url);
    return out;
  }

  void refresh() {
    const View& v = views[active];
    if (v.is_list()) {
      std::vector<std::string> stale;  // merged/closed PRs never change again
      for (auto& u : list_urls(v))
        if (auto c = cache.find(u); c == cache.end() || c->second.state == "OPEN") stale.push_back(u);
      request_urls(stale);
    } else
      request_search(effective(v));
    if (plugins) plugins->refresh();
    requested_full.clear();  // stubs whose fill failed get another try
    last_refresh = now();
  }

  // -- plugins ----------------------------------------------------------------

  int plugin_index(const std::string& name) const {
    for (size_t i = 0; i < cfg.plugins.size(); ++i)
      if (cfg.plugins[i].name == name) return static_cast<int>(i);
    return -1;
  }

  std::string shown_title(const std::string& t) const {
    return cfg.title_strip.empty() ? t : std::regex_replace(t, cfg.title_re, "");
  }
  // -- people (config "people") ----------------------------------------------

  const Person* person(const std::string& login) const {
    auto it = cfg.people.find(lower(login));
    return it != cfg.people.end() ? &it->second : nullptr;
  }
  // A Slack tag names someone by their Slack name: the person whose "name" it
  // is, or you.
  const Person* tag_person(const Tag& t) const {
    if (t.text == "you") return person(viewer);
    for (auto& [login, p] : cfg.people)
      if (!p.name.empty() && lower(p.name) == lower(t.text)) return &p;
    return nullptr;
  }
  static Element paint(Element e, const Person* p) {
    if (!p) return e;
    static const bool truecolor = Terminal::ColorSupport() == Terminal::TrueColor;
    if (p->rgb >= 0 && (truecolor || p->ansi < 0))  // no ansi: FTXUI picks the nearest
      return e | color(Color::RGB(p->rgb >> 16, (p->rgb >> 8) & 0xff, p->rgb & 0xff));
    if (p->ansi >= 0)
      return e | color(p->ansi < 16 ? Color(Color::Palette16(p->ansi)) : Color(Color::Palette256(p->ansi)));
    return e;
  }

  bool one_shot(size_t i) const { return cfg.plugins[i].rerun != "exit"; }

  void on_plugin_line(size_t i, const std::string& line, bool is_err) {
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
    if (one_shot(i)) {  // health shows live even while the PRs wait for the run to end
      live[i].health = st.health;
      live[i].health_text = st.health_text;
      live[i].bad_lines = st.bad_lines;
    }
    if (!url.empty() && !cache.count(url) && !requested.count(url) && !st.prs[url].lists.empty())
      request_urls({url});
  }

  void on_plugin_exit(size_t i, int code) {
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
  void notify_approved() {
    if (!plugins) return;
    for (size_t i = 0; i < approvals.size(); ++i) {
      if (live[i].health != "ok") continue;
      for (auto it = approvals[i].begin(); it != approvals[i].end();)
        it = plugins->send(i, nlohmann::json({{"event", "approved"}, {"pr", *it}}).dump())
                 ? approvals[i].erase(it)
                 : std::next(it);
    }
  }

  // A plugin-declared key on the selected PR. False if no plugin claims it.
  bool plugin_action(const std::string& key, const Pr* cur) {
    for (size_t i = 0; i < cfg.plugins.size(); ++i)
      for (auto& a : one_shot(i) ? std::vector<Action>{} : live[i].actions) {  // no stdin
        if (a.key != key) continue;
        if (!cur) return true;
        std::string name = cfg.plugins[i].name, pr = cur->repo + "#" + std::to_string(cur->number);
        bool ok = plugins && plugins->send(i, nlohmann::json({{"action", a.id}, {"pr", cur->url}}).dump());
        flash(name + ": " + (ok ? (a.label.empty() ? a.id : a.label) + " — " + pr : "not running"));
        return true;
      }
    return false;
  }

  bool plugin_up(size_t i) const {
    if (live[i].health == "error") return false;
    return one_shot(i) ? last_code[i] == 0 : running[i] && live[i].health != "exited";
  }

  std::optional<Status> status_of(const std::string& url) const {
    for (auto& st : live)
      if (auto it = st.prs.find(url); it != st.prs.end() && it->second.status)
        return it->second.status;
    return std::nullopt;
  }

  std::vector<Tag> tags_of(const std::string& url) const {
    std::vector<Tag> out;
    for (auto& st : live)
      if (auto it = st.prs.find(url); it != st.prs.end())
        out.insert(out.end(), it->second.tags.begin(), it->second.tags.end());
    return out;
  }

  // -- rows -------------------------------------------------------------------

  // nullopt when this view has no data yet (search never answered).
  std::optional<std::vector<Pr>> rows(size_t vi, std::string* err = nullptr) const {
    const View& v = views[vi];
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
        if (auto c = cache.find(u); c != cache.end() && !c->second.partial &&
                                    local_match(c->second, *preds, viewer))
          out.push_back(c->second);
    }
    if (sorts[vi] == Sort::List && !out.empty()) {  // non-empty: the plugin exists
      // Plugin order first; PRs it gave no order go last, newest update first.
      sort_prs(out, Sort::Updated);
      const auto& prs = live[static_cast<size_t>(plugin_index(v.plugin))].prs;
      auto key = [&](const Pr& p) {
        auto it = prs.find(p.url);
        return it != prs.end() && it->second.order ? *it->second.order : -HUGE_VAL;
      };
      std::stable_sort(out.begin(), out.end(), [&](const Pr& a, const Pr& b) { return key(a) > key(b); });
    } else {
      sort_prs(out, sorts[vi]);
    }
    return out;
  }

  // Stubs on the active list that could match once their full fields are
  // known: judged on the predicates a stub can answer (state, org, repo).
  std::vector<std::string> stubs_to_fill() const {
    const View& v = views[active];
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
      if (auto c = cache.find(u); c != cache.end() && c->second.partial &&
                                  !requested_full.count(u) && local_match(c->second, known, viewer))
        out.push_back(u);
    return out;
  }

  // -- rendering --------------------------------------------------------------

  Element render_tabs() {
    Elements tabs;
    for (size_t i = 0; i < views.size(); ++i) {
      auto r = rows(i);
      std::string label = " " + views[i].name + (r ? " (" + std::to_string(r->size()) + ")" : "") + " ";
      Element t = text(label);
      tabs.push_back(i == active ? t | bold | inverted : t | dim);
      tabs.push_back(text("│") | dim);
    }
    tabs.push_back(filler());
    if (!scope.empty()) tabs.push_back(text(" " + scope + " ") | color(Color::Cyan));
    return hbox(std::move(tabs));
  }

  Element state_cell(const Shown& s) {
    Element e = cell(s.text, 12);
    switch (s.kind) {
      case Kind::Merged: return e | color(Color::Magenta);
      case Kind::Done: return e | dim;
      case Kind::Conflict: return e | bold | color(Color::Red);
      case Kind::Custom: return e | bold | color(Color::Cyan);
      case Kind::Draft: return e | dim;
      case Kind::Failing: return e | color(Color::Red);
      case Kind::Changes: return e | color(Color::Yellow);
      case Kind::Approved: return e | color(Color::Green);
      case Kind::Mine: return e | color(Color::Blue);
      case Kind::Review: return e;
    }
    return e;
  }

  Element tags_cell(const std::vector<Tag>& tags, int w) {
    Elements parts;
    int used = 0;
    size_t shown = 0;
    for (auto& t : tags) {
      std::string one = t.glyph.empty() ? t.text : t.glyph + " " + t.text;
      int add = string_width(one) + (shown ? 1 : 0);
      int rest = static_cast<int>(tags.size() - shown - 1);
      if (used + add + (rest > 0 ? 3 : 0) > w && shown > 0) break;
      if (shown) parts.push_back(text(" "));
      parts.push_back(paint(text(one), tag_person(t)));
      used += add;
      ++shown;
    }
    if (shown < tags.size()) parts.push_back(text(" +" + std::to_string(tags.size() - shown)));
    return hbox(std::move(parts)) | size(WIDTH, EQUAL, w);
  }

  Element author_cell(const std::string& login) {
    const Person* who = person(login);
    return paint(cell(who && !who->name.empty() ? who->name : login, 12), who);
  }

  Element render_table(const std::vector<Pr>& rs) {
    std::set<std::string> row_owners;
    int num_w = 2;
    for (auto& p : rs) {
      row_owners.insert(p.owner);
      num_w = std::max(num_w, static_cast<int>(std::to_string(p.number).size()));
    }
    bool show_owner = row_owners.size() > 1;
    int repo_w = 4;
    for (auto& p : rs) {
      std::string r = show_owner ? p.repo : p.repo.substr(p.repo.find('/') + 1);
      repo_w = std::max(repo_w, std::min(24, string_width(r)));
    }
    const int tags_w = 16;

    auto header = hbox({text(" "), cell("REPO", repo_w), text("  "), cell_right("#", num_w),
                        text("  "), cell_right("SIZE", 11), text("  "), cell("STATE", 12),
                        cell_right("AGE", 4), text("  "), cell("AUTHOR", 12), text(" "),
                        cell("TAGS", tags_w), text(" "), text("TITLE") | flex}) |
                  bold | dim;

    Elements lines;
    std::time_t t = now();
    for (size_t i = 0; i < rs.size(); ++i) {
      const Pr& p = rs[i];
      std::string repo = show_owner ? p.repo : p.repo.substr(p.repo.find('/') + 1);
      Color size_color = Color::Green;
      switch (size_band(p.additions + p.deletions, cfg.green_below, cfg.red_from)) {
        case Band::Green: size_color = Color::Green; break;
        case Band::Yellow: size_color = Color::Yellow; break;
        case Band::Red: size_color = Color::Red; break;
      }
      Element size_el = p.partial ? cell("", 11)
                                  : hbox({cell_right("+" + compact(p.additions), 5), text(" "),
                                          cell_right("-" + compact(p.deletions), 5)}) |
                                        color(size_color);
      Element line = hbox({text(" "), cell(repo, repo_w), text("  "),
                           cell_right(std::to_string(p.number), num_w), text("  "), size_el,
                           text("  "), state_cell(derive_state(p, status_of(p.url))),
                           cell_right(p.created ? age(static_cast<long>(t - p.created)) : "", 4),
                           text("  "), author_cell(p.author), text(" "),
                           tags_cell(tags_of(p.url), tags_w), text(" "),
                           text(p.partial ? "…" : shown_title(p.title)) | flex});
      if (static_cast<int>(i) == selected) line = line | inverted | focus;
      lines.push_back(line);
    }
    return vbox({header, vbox(std::move(lines)) | vscroll_indicator | yframe | flex});
  }

  Element render_detail(const std::vector<Pr>& rs) {
    if (rs.empty()) return text("");
    const Pr& p = rs[static_cast<size_t>(selected)];
    Elements parts = {text(" " + p.url) | dim};
    for (size_t i = 0; i < live.size(); ++i) {
      auto it = live[i].prs.find(p.url);
      if (it == live[i].prs.end()) continue;
      const std::string tag = "  [" + cfg.plugins[i].name + "] ";
      if (it->second.status)
        parts.push_back(text(tag + it->second.status->glyph + " " + it->second.status->text));
      for (auto& t : it->second.tags)
        parts.push_back(text(tag + t.glyph + " " + t.text + (t.hint.empty() ? "" : " — " + t.hint)));
    }
    return hbox(std::move(parts));
  }

  Element render_footer() {
    Elements f = {text(" r refresh  / query  o org  p repo  x clear  s sort:" +
                       std::string(sort_name(sorts[active])) + "  ⏎ open  y copy  ? help  q quit") |
                  dim};
    f.push_back(filler());
    if (!update_note.empty()) f.push_back(text("⬆ " + update_note + "  ") | color(Color::Green));
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      bool up = plugin_up(i);
      f.push_back(text(cfg.plugins[i].name + (up ? " ● " : " ○ ")) |
                  color(up ? Color::Green : Color::Red));
    }
    std::string fresh = loading()          ? "↻ loading…"
                        : last_fetch == 0  ? ""
                        : now() - last_fetch < 60
                            ? "↻ just now"
                            : "↻ " + age(static_cast<long>(now() - last_fetch)) + " ago";
    f.push_back(text(fresh + " "));
    return hbox(std::move(f));
  }

  Element render_message(const std::string& view_err) {
    if (now() < notice_until && !notice.empty()) return text(" " + notice) | color(Color::Cyan);
    std::string e = !view_err.empty() ? view_err : error;
    if (!e.empty()) return text(" " + e) | color(Color::Red);
    return text("");
  }

  std::vector<std::string> pick_candidates() const {
    std::vector<std::string> out;
    for (auto& c : pick_repo ? repos : owners)
      if (!c.empty() && fuzzy(c, pick_buf)) out.push_back(c);
    return out;
  }

  Element render_picker() {
    auto cands = pick_candidates();
    Elements items;
    for (size_t i = 0; i < cands.size() && i < 10; ++i) {
      Element e = text(" " + cands[i] + " ");
      items.push_back(static_cast<int>(i) == pick_sel ? e | inverted : e);
    }
    if (cands.empty())
      items.push_back(text(pick_buf.empty() ? " (nothing seen yet; type one)" : " ⏎ use as typed") | dim);
    return window(text(pick_repo ? " repo " : " org "),
                  vbox({hbox({text("› "), pick_input->Render()}), separator(), vbox(items)})) |
           size(WIDTH, GREATER_THAN, 40) | clear_under | center;
  }

  Element render_help() {
    Elements l = {
        text("keys") | bold,
        text("  j/k ↑/↓ g/G   move            ⏎  open in browser   y  copy URL"),
        text("  r             refresh         /  edit query        s  cycle sort"),
        text("  o / p         pick org / repo x  clear org/repo    1-9 Tab  views"),
        text("  q             quit            ?  this help (any key closes)"),
        text(""),
        text("view: " + views[active].name + "   query: " + effective(views[active])) | dim,
        text(std::string("anton ") + kVersion) | dim,
    };
    if (!cfg.plugins.empty()) {
      l.push_back(text(""));
      l.push_back(text("plugins") | bold);
    }
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      std::string cmd;
      for (auto& a : cfg.plugins[i].command) cmd += (cmd.empty() ? "" : " ") + a;
      const PluginState& st = live[i];
      l.push_back(text("  " + cfg.plugins[i].name + "  " + (plugin_up(i) ? "●" : "○") + " " +
                       (st.health.empty() ? "-" : st.health) +
                       (st.health_text.empty() ? "" : ": " + st.health_text)));
      l.push_back(text("    $ " + cmd) | dim);
      l.push_back(text("    " + std::to_string(st.prs.size()) + " PRs, " +
                       std::to_string(st.bad_lines) + " bad lines") |
                  dim);
      for (auto& a : st.actions)
        l.push_back(text("    " + a.key + "  " + (a.label.empty() ? a.id : a.label)));
      if (!st.last_stderr.empty()) l.push_back(text("    stderr: " + st.last_stderr) | dim);
    }
    return window(text(" anton "), vbox(std::move(l))) | clear_under | center;
  }

  Element render() {
    if (now() - last_refresh >= cfg.refresh_seconds) refresh();
    if (now() - last_watch >= cfg.watch_seconds) poll_watched();
    if (update_note.empty() && now() - last_update_check >= 6 * 3600) check_update();
    std::string view_err;
    auto rs = rows(active, &view_err).value_or(std::vector<Pr>{});
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(rs.size()) - 1));
    request_full(stubs_to_fill());

    Element body;
    const View& v = views[active];
    int pi = v.is_list() ? plugin_index(v.plugin) : -1;
    if (v.is_list() && (pi < 0 || !plugin_up(static_cast<size_t>(pi))) && rs.empty())
      body = text(" " + v.plugin + ": not running") | color(Color::Red) | center | flex;
    else if (rs.empty())
      body = text(loading() ? "loading…" : "no PRs") | dim | center | flex;
    else
      body = render_table(rs);

    Element main = vbox({render_tabs(), separator(), body, separator(), render_detail(rs),
                         mode == Mode::Query ? hbox({text(" query › ") | bold, query_input->Render()})
                                             : render_message(view_err),
                         render_footer()});
    if (mode == Mode::Picker) return dbox({main, render_picker()});
    if (mode == Mode::Help) return dbox({main, render_help()});
    return main;
  }

  // -- actions ----------------------------------------------------------------

  // At startup and every 6h, until there's something to say. Detached: anton
  // _Exits on quit, so nothing joins it.
  void check_update() {
    last_update_check = now();
    std::thread([this, install = cfg.auto_update] {
      std::string note = self_update(install);
      if (!note.empty()) post([this, note] { update_note = note; });
    }).detach();
  }

  void flash(const std::string& s, int secs = 3) {
    notice = s;
    notice_until = now() + secs;
  }

  void switch_view(size_t i) {
    if (i >= views.size() || i == active) return;
    active = i;
    selected = 0;
    refresh();
  }

  void open(const std::string& url) {
    std::thread([url] { run({"gh", "pr", "view", "--web", url}); }).detach();
    flash("opening " + url);
  }

  void copy(const std::string& url) {
    std::thread([this, url] {
      for (auto argv : std::vector<std::vector<std::string>>{
               {"pbcopy"}, {"wl-copy"}, {"xclip", "-selection", "clipboard"}})
        if (run(argv, url).code == 0) return post([this] { flash("copied"); });
      post([this] { flash("no clipboard tool (pbcopy, wl-copy, xclip)"); });
    }).detach();
  }

  bool on_event(Event e) {
    if (e.is_mouse()) return false;
    if (e == Event::CtrlC) {  // quits from any mode, through Loop's normal exit
      screen.Exit();
      return true;
    }
    if (e == Event::Custom) return false;

    if (mode == Mode::Help) {
      mode = Mode::Normal;
      return true;
    }
    if (mode == Mode::Query) {
      if (e == Event::Return) {
        views[active].query = query_buf;
        mode = Mode::Normal;
        selected = 0;
        refresh();
      } else if (e == Event::Escape) {
        mode = Mode::Normal;
      } else {
        query_input->OnEvent(e);
      }
      return true;
    }
    if (mode == Mode::Picker) {
      auto cands = pick_candidates();
      int n = std::min(10, static_cast<int>(cands.size()));
      if (e == Event::Return) {
        std::string choice = n > 0 ? cands[static_cast<size_t>(std::min(pick_sel, n - 1))] : pick_buf;
        if (!choice.empty()) {
          scope = (pick_repo ? "repo:" : "org:") + choice;
          selected = 0;
          refresh();
        }
        mode = Mode::Normal;
      } else if (e == Event::Escape) {
        mode = Mode::Normal;
      } else if (e == Event::ArrowDown || e == Event::CtrlN) {
        if (n > 0) pick_sel = (pick_sel + 1) % n;
      } else if (e == Event::ArrowUp || e == Event::CtrlP) {
        if (n > 0) pick_sel = (pick_sel + n - 1) % n;
      } else {
        pick_sel = 0;  // on_change is posted, so it may land after a queued Return
        pick_input->OnEvent(e);
      }
      return true;
    }

    std::string err;
    auto rs = rows(active, &err).value_or(std::vector<Pr>{});
    int last = static_cast<int>(rs.size()) - 1;
    const Pr* cur = rs.empty() ? nullptr : &rs[static_cast<size_t>(std::clamp(selected, 0, last))];

    if (e == Event::Character('q') || e == Event::CtrlC) {
      screen.Exit();
    } else if (e == Event::Character('j') || e == Event::ArrowDown) {
      selected = std::min(selected + 1, std::max(0, last));
    } else if (e == Event::Character('k') || e == Event::ArrowUp) {
      selected = std::max(selected - 1, 0);
    } else if (e == Event::PageDown) {
      selected = std::min(selected + 10, std::max(0, last));
    } else if (e == Event::PageUp) {
      selected = std::max(selected - 10, 0);
    } else if (e == Event::Character('g') || e == Event::Home) {
      selected = 0;
    } else if (e == Event::Character('G') || e == Event::End) {
      selected = std::max(0, last);
    } else if (e == Event::Return) {
      if (cur) open(cur->url);
    } else if (e == Event::Character('y')) {
      if (cur) copy(cur->url);
    } else if (e == Event::Character('r')) {
      refresh();
    } else if (e == Event::Character('/')) {
      query_buf = views[active].query;
      query_cursor = static_cast<int>(query_buf.size());
      mode = Mode::Query;
    } else if (e == Event::Character('o') || e == Event::Character('p')) {
      pick_repo = e == Event::Character('p');
      pick_buf.clear();
      pick_sel = 0;
      mode = Mode::Picker;
    } else if (e == Event::Character('x')) {
      scope.clear();
      views[active].query = remove_qualifiers(views[active].query, kScopeKeys);
      selected = 0;
      refresh();
    } else if (e == Event::Character('s')) {
      // List only exists for list views.
      Sort& s = sorts[active];
      do s = static_cast<Sort>((static_cast<int>(s) + 1) % 5);
      while (s == Sort::List && !views[active].is_list());
    } else if (e == Event::Tab) {
      switch_view((active + 1) % views.size());
    } else if (e == Event::TabReverse) {
      switch_view((active + views.size() - 1) % views.size());
    } else if (e == Event::Character('?')) {
      mode = Mode::Help;
    } else if (e.is_character() && e.character().size() == 1 && e.character()[0] >= '1' &&
               e.character()[0] <= '9') {
      switch_view(static_cast<size_t>(e.character()[0] - '1'));
    } else if (e.is_character() && plugin_action(e.character(), cur)) {
    } else {
      return false;
    }
    return true;
  }
};

}  // namespace

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

  auto root = Renderer([u] { return u->render(); }) |
              CatchEvent([u](Event e) { return u->on_event(e); });
  // Ctrl-C is a key event (handled above) so the loop exits normally and
  // stops the plugins below. SIGTERM/SIGHUP kill anton from outside: FTXUI
  // restores the terminal, then re-raises into this handler, which takes the
  // plugins down with it -- they live in their own process groups and would
  // otherwise outlive anton.
  u->screen.ForceHandleCtrlC(false);
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
