#pragma once
// What the terminal UI (ui*.cpp) and the desktop one (gui*.cpp) share: the
// views and their config writes, fetching PRs off the UI thread, the
// plugins and what they say, and the reader's data (a PR's description and
// markdown files, marks on them). No drawing here: each UI derives from
// Core and draws it. Everything runs on the UI thread unless it says
// otherwise; worker threads hand results back through post().

#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"
#include "filters.hpp"
#include "github.hpp"
#include "markdown.hpp"
#include "model.hpp"
#include "plugins.hpp"

namespace anton {

inline std::time_t now() { return std::time(nullptr); }

// "2026-11-14", in UTC: temoc's snoozes count days in UTC.
inline std::string today() { return ymd(now()); }

struct Core {
  Config cfg;  // as saved: what config.json says, plus what anton wrote to it
  std::string self_name = "anton";  // the release binary this is, for self_update

  // -- views: the tabs ---------------------------------------------------------
  // A tab is its view as it is now and as last saved (none: never saved).
  // They differ once you change its filters: a yellow • until saved/reverted.
  struct Tab {
    View v;
    std::optional<View> saved;
  };
  std::vector<Tab> tabs;
  std::vector<View> offscreen;  // config views not on screen (--query); kept on save
  size_t active = 0;
  std::string scope;  // "org:x" / "repo:o/n" / "": every view, kept when switching

  // -- data --------------------------------------------------------------------
  std::map<std::string, Pr> cache;                          // by URL
  std::map<std::string, std::vector<std::string>> results;  // search query -> URLs
  std::set<std::string> requested;                          // URLs ever asked for
  std::set<std::string> owners, repos;                      // every one seen: picker source
  std::string viewer;
  std::time_t last_fetch = 0, last_refresh = 0, last_watch = 0, last_update_check = 0;
  std::string update_note;  // stays in the footer once set
  std::string error, notice;
  std::time_t notice_until = 0;
  bool notice_bad = false;  // a failed save: red
  int column_gen = 0;       // bumped per refresh: command columns rerun

  // -- the reader's data -------------------------------------------------------
  std::string reader_url;  // the PR the reader shows
  size_t reader_tab = 0;   // 0: the description, then the PR's files in tab order
  struct Body {  // fetched when the reader opens it and when updatedAt moves
    std::string text, head, error;
    std::time_t updated = -1;
    bool loading = false;
  };
  std::map<std::string, Body> bodies;  // by URL
  struct FileTab {
    std::string path, name, text, error;
    std::vector<md::Directive> dirs;
    bool spec = false;  // has a decisionTable directive
  };
  struct Files {  // the PR's markdown files at its head; specs first
    std::vector<FileTab> tabs;
    std::string error;
    bool loading = false;
  };
  std::map<std::pair<std::string, std::string>, Files> files;
  std::map<size_t, std::string> last_glyph;  // per plugin: its last tag's glyph (🧪), for "🧪 error" once it's down

  // -- plugins -------------------------------------------------------------------
  std::vector<PluginState> live, pending;  // one-shot plugins fold into pending until they end
  std::vector<bool> running;
  std::vector<int> last_code;
  std::unique_ptr<Plugins> plugins;

  // -- fetch worker queue (mu) -----------------------------------------------------
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::string> searches;
  std::set<std::string> want_urls;  // state first, full fields if open
  std::set<std::string> want_full;  // full fields regardless (stubs a view shows)
  bool busy = false;
  std::set<std::string> requested_full;          // UI thread
  std::vector<std::set<std::string>> approvals;  // per plugin: new approvals not yet delivered

  // -- notify views (01-anton.md, "Notify") ----------------------------------------
  std::set<std::string> notified;            // told, or there at the start: never told (again) this session
  std::set<std::string> notify_base;         // there when its view was first checked: judged once fetched, not told
  std::map<std::string, bool> notify_fetch;  // newly in a notify view's list: true once re-fetched
  std::set<std::string> notify_views;        // plugin, list and filter of each notify view seen so far
  std::vector<bool> baselined;               // per plugin: has been ok once (one-shot: has ended once)
  std::function<void(const std::string& title, const std::string& body)> notifier;  // the desktop's; tests swap it

  explicit Core(Config c);
  virtual ~Core() = default;

  // Runs fn on the UI thread; called from any thread.
  virtual void post(std::function<void()> fn) = 0;
  // The list's selection goes back to the top (the rows changed under it).
  virtual void reset_selection() {}
  // A plugin action went out on plugin i's stdin (ok: it was written).
  virtual void action_sent(size_t, const nlohmann::json&, bool) {}

  // The fetch worker and the plugins: once, before the first frame.
  void start();
  // Per frame: the timed refresh, watched PRs, the update check, stubs to fill.
  void tick();

  // -- data --------------------------------------------------------------------
  std::string effective(const View& v) const;
  void request_search(const std::string& q);
  void request_urls(const std::vector<std::string>& urls);
  void request_full(const std::vector<std::string>& urls);
  void poll_watched();
  bool loading();
  void worker();  // the fetch thread
  void on_fetched(const std::string& q, const Fetch& f, bool is_search);  // a fetch's result, on the UI thread
  std::vector<std::string> list_urls(const View& v) const;
  void refresh();
  std::optional<std::vector<Pr>> rows(size_t vi, std::string* err = nullptr) const;
  std::vector<std::string> stubs_to_fill() const;
  Seen seen() const;
  std::vector<std::string> plugin_lists(const std::string& plugin) const;

  // -- plugins -------------------------------------------------------------------
  int plugin_index(const std::string& name) const;
  bool one_shot(size_t i) const { return cfg.plugins[i].rerun != "exit"; }
  virtual void on_plugin_line(size_t i, const std::string& line, bool is_err);
  virtual void on_plugin_exit(size_t i, int code);
  void notify_approved();
  void check_notify();  // tells PRs newly in a notify view
  // file (and line) go along when the action takes them ("line": true).
  bool plugin_action(const std::string& key, const std::string& url, const std::string& file = "", int line = 0);
  bool plugin_up(size_t i) const;
  std::optional<Status> status_of(const std::string& url) const;
  std::vector<Tag> tags_of(const std::string& url) const;
  std::vector<std::pair<std::string, std::string>> notes_of(const std::string& url) const;
  bool has_note(const std::string& url) const;
  struct PluginKey {  // a plugin's action, for buttons and the palette
    size_t plugin;
    Action action;
  };
  std::vector<PluginKey> plugin_keys() const;

  // -- people, titles -------------------------------------------------------------
  std::string shown_title(const std::string& t) const;
  const Person* person(const std::string& login) const;
  const Person* tag_person(const Tag& t) const;
  std::string who(const std::string& login) const;

  // -- views ---------------------------------------------------------------------
  View& view() { return tabs[active].v; }
  bool modified(size_t i) const;
  void apply_query(const std::string& q);  // the active view's filter becomes q
  void set_sort(Sort s);
  std::vector<View> saved_views() const;
  bool write_config(const std::string& done = "");  // flashes the error; false on one
  void commit_view(size_t i);  // saves tab i's filters (it has a name of its own)
  void save_as_new(size_t i, const std::string& name);
  void revert_view(size_t i);
  size_t add_view();  // a new unsaved search view, switched to
  void duplicate_view(size_t i);
  void move_view(size_t i, int by);
  void delete_view(size_t i);
  std::string rename_view(size_t i, const std::string& name);
  void switch_view(size_t i);
  void edit_view(size_t i, const std::function<void(View&)>& fn);  // as it is and as saved; writes
  void clear_scope();  // x: the org/repo scope goes, from the view's query too
  void open(const std::string& url);
  void copy(const std::string& s);
  void flash(const std::string& s, int secs = 3, bool bad = false);
  void check_update();

  // -- the reader's data -------------------------------------------------------
  void prepare_reader(const Pr& p);  // reader_url, tab 0; reopening retries what failed
  void want_body(const std::string& url, std::time_t updated);
  Files* want_files(const std::string& url);
  const Files* reader_files() const;
  const FileTab* reader_tab_file() const;  // the open file tab, nullptr on the description
  std::vector<Mark> marks_on(const std::string& url, const std::string& path, const std::string& head,
                             std::vector<std::string>* stale = nullptr, int only_plugin = -1) const;
  std::optional<PluginKey> line_key() const;  // the first action that runs a file's line: ▶, r
  void run_line(const std::string& file, int line);
  std::string plugin_problem(size_t i) const;  // "" or why plugin i is broken: health error, exited
  // Line-0 marks atop a tab ("" path: the description): broken plugins,
  // summaries, `not run · T to run`.
  std::vector<Mark> reader_top(const std::string& path, bool spec, const std::vector<Mark>& marks) const;
};

}  // namespace anton
