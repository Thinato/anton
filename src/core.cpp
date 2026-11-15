// The state and logic both UIs share (core.hpp): fetching, plugins, views,
// the reader's data. Moved out of ui.cpp and ui_reader.cpp unchanged but for
// the hooks each UI fills in (post, reset_selection, action_sent).
#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#include "config_write.hpp"
#include "github.hpp"
#include "proc.hpp"
#include "update.hpp"

namespace anton {

Core::Core(Config c) : cfg(std::move(c)) {
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

void Core::start() {
  std::thread([this] { worker(); }).detach();
  if (cfg.plugins.empty()) return;
  plugins = std::make_unique<Plugins>(
      cfg.plugins,
      [this](size_t i, const std::string& line, bool is_err) {
        post([this, i, line, is_err] { on_plugin_line(i, line, is_err); });
      },
      [this](size_t i, int code) { post([this, i, code] { on_plugin_exit(i, code); }); });
  plugins->start();
}

void Core::tick() {
  if (now() - last_refresh >= cfg.refresh_seconds) refresh();
  if (now() - last_watch >= cfg.watch_seconds) poll_watched();
  if (update_note.empty() && now() - last_update_check >= 15 * 60) check_update();
  request_full(stubs_to_fill());
}

// -- fetching -------------------------------------------------------------------

std::string Core::effective(const View& v) const {
  std::string q = with_scope(v.query, scope);
  return v.is_list() ? q : ensure_is_pr(q);
}

void Core::request_search(const std::string& q) {
  std::lock_guard lk(mu);
  if (std::find(searches.begin(), searches.end(), q) == searches.end()) searches.push_back(q);
  busy = true;
  cv.notify_one();
}

void Core::request_urls(const std::vector<std::string>& urls) {
  if (urls.empty()) return;
  std::lock_guard lk(mu);
  for (auto& u : urls) {
    want_urls.insert(u);
    requested.insert(u);
  }
  busy = true;
  cv.notify_one();
}

void Core::request_full(const std::vector<std::string>& urls) {
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
void Core::poll_watched() {
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

bool Core::loading() {
  std::lock_guard lk(mu);
  return busy;
}

void Core::worker() {
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

std::vector<std::string> Core::list_urls(const View& v) const {
  std::vector<std::string> out;
  int pi = plugin_index(v.plugin);
  if (pi < 0) return out;
  for (auto& [url, info] : live[static_cast<size_t>(pi)].prs)
    if (info.lists.count(v.list)) out.push_back(url);
  return out;
}

void Core::refresh() {
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
std::optional<std::vector<Pr>> Core::rows(size_t vi, std::string* err) const {
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
std::vector<std::string> Core::stubs_to_fill() const {
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

Seen Core::seen() const {
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

std::vector<std::string> Core::plugin_lists(const std::string& plugin) const {
  std::set<std::string> out;
  if (int pi = plugin_index(plugin); pi >= 0)
    for (auto& [url, info] : live[static_cast<size_t>(pi)].prs) out.insert(info.lists.begin(), info.lists.end());
  return {out.begin(), out.end()};
}

// -- plugins -------------------------------------------------------------------

int Core::plugin_index(const std::string& name) const {
  for (size_t i = 0; i < cfg.plugins.size(); ++i)
    if (cfg.plugins[i].name == name) return static_cast<int>(i);
  return -1;
}

void Core::on_plugin_line(size_t i, const std::string& line, bool is_err) {
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

void Core::on_plugin_exit(size_t i, int code) {
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
void Core::notify_approved() {
  if (!plugins) return;
  for (size_t i = 0; i < approvals.size(); ++i) {
    if (live[i].health != "ok") continue;
    for (auto it = approvals[i].begin(); it != approvals[i].end();)
      it = plugins->send(i, nlohmann::json({{"event", "approved"}, {"pr", *it}}).dump()) ? approvals[i].erase(it)
                                                                                          : std::next(it);
  }
}

std::vector<Core::PluginKey> Core::plugin_keys() const {
  std::vector<PluginKey> out;
  for (size_t i = 0; i < cfg.plugins.size(); ++i)
    if (!one_shot(i))  // one-shot plugins have no stdin
      for (auto& a : live[i].actions) out.push_back({i, a});
  return out;
}

// A plugin-declared key on a PR. False if no plugin claims it.
bool Core::plugin_action(const std::string& key, const std::string& url, const std::string& file, int line) {
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
    action_sent(i, msg, ok);
    flash(cfg.plugins[i].name + ": " + (ok ? what + " — " + pr : "not running"));
    return true;
  }
  return false;
}

bool Core::plugin_up(size_t i) const {
  if (live[i].health == "error") return false;
  return one_shot(i) ? last_code[i] == 0 : running[i] && live[i].health != "exited";
}

std::optional<Status> Core::status_of(const std::string& url) const {
  for (auto& st : live)
    if (auto it = st.prs.find(url); it != st.prs.end() && it->second.status) return it->second.status;
  return std::nullopt;
}

std::vector<Tag> Core::tags_of(const std::string& url) const {
  std::vector<Tag> out;
  for (auto& st : live)
    if (auto it = st.prs.find(url); it != st.prs.end())
      out.insert(out.end(), it->second.tags.begin(), it->second.tags.end());
  return out;
}

std::vector<std::pair<std::string, std::string>> Core::notes_of(const std::string& url) const {
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < live.size(); ++i)
    if (auto it = live[i].prs.find(url); it != live[i].prs.end() && !it->second.note.empty())
      out.emplace_back(cfg.plugins[i].name, it->second.note);
  return out;
}

bool Core::has_note(const std::string& url) const { return !notes_of(url).empty(); }

// -- people, titles -------------------------------------------------------------

std::string Core::shown_title(const std::string& t) const {
  return cfg.title_strip.empty() ? t : std::regex_replace(t, cfg.title_re, "");
}

const Person* Core::person(const std::string& login) const {
  auto it = cfg.people.find(lower(login));
  return it != cfg.people.end() ? &it->second : nullptr;
}

// A Slack tag names someone by their Slack name: the person whose "name" it
// is, or you.
const Person* Core::tag_person(const Tag& t) const {
  if (t.text == "you") return person(viewer);
  for (auto& [login, p] : cfg.people)
    if (!p.name.empty() && lower(p.name) == lower(t.text)) return &p;
  return nullptr;
}

std::string Core::who(const std::string& login) const {
  const Person* p = person(login);
  return p && !p->name.empty() ? p->name : login;
}

// -- views ---------------------------------------------------------------------

// Its filters differ from what config.json has (or it was never saved).
bool Core::modified(size_t i) const {
  const Tab& t = tabs[i];
  if (!t.saved) return true;
  const View& s = *t.saved;
  return t.v.plugin != s.plugin || t.v.list != s.list || t.v.sort_or_default() != s.sort_or_default() ||
         !same_query(with_scope(t.v.query, scope), s.query);
}

// Chips and the query row both end here. The org/repo scope is session-wide
// (01-anton.md): while one is set, the scope qualifiers in q become it.
void Core::apply_query(const std::string& q) {
  if (!scope.empty()) {
    std::vector<std::string> sc;
    for (auto& t : tokenize(q))
      if (t[0] != '-' && kScopeKeys.count(qualifier_key(t))) sc.push_back(t);
    scope = join(sc);
  }
  view().query = q;
  reset_selection();
  refresh();
}

void Core::set_sort(Sort s) {
  view().sort = s;
  reset_selection();
}

std::vector<View> Core::saved_views() const {
  std::vector<View> out = offscreen;
  for (auto& t : tabs)
    if (t.saved) out.push_back(*t.saved);
  return out;
}

bool Core::write_config(const std::string& done) {
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

void Core::commit_view(size_t i) {
  View s = tabs[i].v;
  s.query = with_scope(s.query, scope);
  auto old = tabs[i].saved;
  tabs[i].saved = s;
  if (!write_config("saved " + s.name + " to config.json")) tabs[i].saved = old;
  else tabs[i].v = s;
}

void Core::save_as_new(size_t i, const std::string& name) {
  Tab t{tabs[i].v, std::nullopt};
  t.v.name = name;
  t.v.query = with_scope(t.v.query, scope);
  t.saved = t.v;
  if (tabs[i].saved) tabs[i].v = *tabs[i].saved;  // the changes move to the new view
  tabs.insert(tabs.begin() + static_cast<long>(i) + 1, t);
  active = i + 1;
  reset_selection();
  write_config("saved " + name + " to config.json");
  refresh();
}

// Back to what config.json has. The org/repo scope goes too: it's what
// keeps every tab modified while it's set.
void Core::revert_view(size_t i) {
  if (!tabs[i].saved) return;
  tabs[i].v = *tabs[i].saved;
  scope.clear();
  reset_selection();
  refresh();
}

static std::string unique_name(const std::vector<Core::Tab>& tabs, const std::string& base) {
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + " " + std::to_string(n);
    if (std::none_of(tabs.begin(), tabs.end(), [&](const Core::Tab& t) { return t.v.name == name; })) return name;
  }
}

size_t Core::add_view() {
  tabs.push_back({View{unique_name(tabs, "New view"), "is:open", "", "", {}}, std::nullopt});
  switch_view(tabs.size() - 1);
  return tabs.size() - 1;
}

void Core::duplicate_view(size_t i) {
  Tab t{tabs[i].v, std::nullopt};
  t.v.name = unique_name(tabs, t.v.name + " copy");
  t.v.query = with_scope(t.v.query, scope);
  t.saved = t.v;
  tabs.insert(tabs.begin() + static_cast<long>(i) + 1, t);
  if (active > i) ++active;
  write_config("duplicated as " + t.v.name);
}

void Core::move_view(size_t i, int by) {
  long j = static_cast<long>(i) + by;
  if (j < 0 || j >= static_cast<long>(tabs.size())) return;
  std::swap(tabs[i], tabs[static_cast<size_t>(j)]);
  if (active == i) active = static_cast<size_t>(j);
  else if (active == static_cast<size_t>(j)) active = i;
  write_config();
}

void Core::delete_view(size_t i) {
  if (tabs.size() == 1) return flash("anton needs one view; this is the last", 3, true);
  std::string name = tabs[i].v.name;
  tabs.erase(tabs.begin() + static_cast<long>(i));
  if (cfg.start_view == name) cfg.start_view.clear();
  if (active > i || active == tabs.size()) --active;
  reset_selection();
  write_config("deleted " + name);
  refresh();
}

std::string Core::rename_view(size_t i, const std::string& raw) {
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

void Core::switch_view(size_t i) {
  if (i >= tabs.size() || i == active) return;
  active = i;
  reset_selection();
  refresh();
}

void Core::clear_scope() {
  scope.clear();
  view().query = remove_qualifiers(view().query, kScopeKeys);
  reset_selection();
  refresh();
}

void Core::open(const std::string& url) {
  std::thread([url] { run({"gh", "pr", "view", "--web", url}); }).detach();
  flash("opening " + url);
}

void Core::copy(const std::string& s) {
  std::thread([this, s] {
    for (auto argv : std::vector<std::vector<std::string>>{{"pbcopy"}, {"wl-copy"}, {"xclip", "-selection", "clipboard"}})
      if (run(argv, s).code == 0) return post([this] { flash("copied"); });
    post([this] { flash("no clipboard tool (pbcopy, wl-copy, xclip)"); });
  }).detach();
}

void Core::flash(const std::string& s, int secs, bool bad) {
  notice = s;
  notice_until = now() + secs;
  notice_bad = bad;
}

// At startup and every 15 min (every merge is a release), until there's
// something to say. Detached: anton _Exits on quit, so nothing joins it.
void Core::check_update() {
  last_update_check = now();
  std::thread([this, install = cfg.auto_update, self = self_name] {
    std::string note = self_update(install, self);
    if (!note.empty()) post([this, note] { update_note = note; });
  }).detach();
}

void Core::edit_view(size_t i, const std::function<void(View&)>& fn) {
  if (i >= tabs.size()) return;
  fn(tabs[i].v);
  if (tabs[i].saved) fn(*tabs[i].saved);
  else tabs[i].saved = tabs[i].v;
  write_config("saved");
  if (i == active) {
    reset_selection();
    refresh();
  }
}


// -- the reader's data ------------------------------------------------------------

// Fetches the description off the UI thread, unless it's in for this
// updatedAt already or on its way.
void Core::want_body(const std::string& url, std::time_t updated) {
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
Core::Files* Core::want_files(const std::string& url) {
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

const Core::Files* Core::reader_files() const {
  auto b = bodies.find(reader_url);
  if (b == bodies.end()) return nullptr;
  auto f = files.find({reader_url, b->second.head});
  return f == files.end() ? nullptr : &f->second;
}

const Core::FileTab* Core::reader_tab_file() const {
  const Files* fs = reader_files();
  return fs && reader_tab > 0 && reader_tab <= fs->tabs.size() ? &fs->tabs[reader_tab - 1] : nullptr;
}

std::optional<Core::PluginKey> Core::line_key() const {
  for (auto& k : plugin_keys())
    if (k.action.line) return k;
  return std::nullopt;
}

// Runs a test table (its directive's line) or row of a file through the
// first plugin action that takes a line.
void Core::run_line(const std::string& file, int line) {
  if (auto k = line_key()) plugin_action(k->action.key, reader_url, file, line);
  else flash("no plugin runs test tables (02-plugins.md, \"line\" actions)");
}


std::string Core::plugin_problem(size_t i) const {
  const PluginState& st = live[i];
  const std::string& name = cfg.plugins[i].name;
  if (st.health == "error") return name + ": " + (st.health_text.empty() ? "error" : st.health_text);
  if (st.health == "exited")
    return name + " exited (" + st.health_text + ")" + (st.last_stderr.empty() ? "" : ": " + st.last_stderr);
  return "";
}

// A broken plugin can't say anything about a file, so anton says it, on
// every tab. The description gets each plugin's summary of the PR; a spec
// file a plugin's summary while that plugin has nothing on any file of the
// PR yet (a run checking out), else `not run · T to run` when nothing has
// a line-0 mark on it.
std::vector<Mark> Core::reader_top(const std::string& path, bool spec, const std::vector<Mark>& marks) const {
  std::vector<Mark> out;
  std::vector<bool> broken(live.size());
  for (size_t i = 0; i < live.size(); ++i)
    if (std::string p = plugin_problem(i); !p.empty()) {
      broken[i] = true;
      out.push_back({path, 0, "error", "!", p, true});
    }
  if (!path.empty() && !spec) return out;
  bool stated = std::any_of(marks.begin(), marks.end(), [](const Mark& m) { return m.line == 0; });
  for (size_t i = 0; i < live.size(); ++i) {
    auto it = live[i].prs.find(reader_url);
    if (broken[i] || it == live[i].prs.end() || !it->second.marks_summary) continue;
    if (!path.empty() && !it->second.marks.empty()) continue;
    Mark m = *it->second.marks_summary;
    m.file = path, m.banner = m.banner || m.level == "error";
    out.push_back(m);
    stated = true;
  }
  if (!path.empty() && !stated && out.empty())
    if (auto k = line_key()) out.push_back({path, 0, "dim", "·", "not run · " + k->action.key + " to run"});
  return out;
}

// Every plugin's marks on one file of a PR (or one plugin's), and, in
// stale, a line for each plugin whose marks are about another commit.
std::vector<Mark> Core::marks_on(const std::string& url, const std::string& path, const std::string& head,
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

void Core::prepare_reader(const Pr& p) {
  reader_url = p.url;
  reader_tab = 0;
  Body& b = bodies[p.url];
  if (!b.error.empty()) b.updated = -1;
  if (auto f = files.find({p.url, b.head}); f != files.end() && !f->second.loading && !f->second.error.empty())
    files.erase(f);  // reopening retries
}

}  // namespace anton
