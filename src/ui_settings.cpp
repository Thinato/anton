// Settings (`,`): General, Views, Columns, each change written to
// config.json as it's made; and `e`, config.json in $EDITOR.
#include <signal.h>
#include <spawn.h>
#include <cerrno>
#include <sys/wait.h>

#include <filesystem>

#include <ftxui/screen/string.hpp>

#include "config_write.hpp"
#include "ui_impl.hpp"

extern char** environ;

namespace anton {

using namespace ftxui;

namespace {

std::string padded(const std::string& s, int w) {
  int n = string_width(s);
  return n >= w ? s : s + std::string(static_cast<size_t>(w - n), ' ');
}

std::string clipped(const std::string& s, int w) {
  if (string_width(s) <= w) return s;
  std::string out;
  int used = 0;
  for (auto& g : Utf8ToGlyphs(s)) {
    if (used + string_width(g) > w - 1) break;
    out += g;
    used += string_width(g);
  }
  return out + "…";
}

std::string tilde(const std::string& path) {
  const char* home = std::getenv("HOME");
  return home && *home && path.starts_with(home) ? "~" + path.substr(std::strlen(home)) : path;
}

// A program on the terminal itself (anton's screen is put away meanwhile).
int run_tty(const std::vector<std::string>& argv) {
  std::vector<char*> args;
  for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t def;
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);  // anton ignores it; an editor shouldn't
  posix_spawnattr_setsigdefault(&attr, &def);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
  pid_t pid;
  int rc = posix_spawnp(&pid, args[0], nullptr, &attr, args.data(), environ);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) return -1;
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

const char* column_about(const Column& c) {
  static const std::map<std::string, const char*> about = {
      {"repo", "owner shown only when results span owners"}, {"number", "the PR number"},
      {"size", "+additions −deletions, colored by size"},    {"state", "GitHub state, or a plugin status"},
      {"age", "since the PR was opened"},                     {"author", "names and colors from people"},
      {"tags", "what plugins say: 👀 alice"},                 {"title", "takes the width that is left"}};
  auto it = about.find(c.name);
  return it == about.end() ? "" : it->second;
}

}  // namespace

void Ui::open_settings() {
  close_menus();
  mode = Mode::Settings;
  set_focus = set_scroll = 0;
  set_view = active;
}

int Ui::control(std::function<void()> act, std::function<void(int)> adj, int row) {
  controls.push_back({std::move(act), std::move(adj), {}, row});
  return static_cast<int>(controls.size()) - 1;
}

Element Ui::render_settings() {
  auto term = Terminal::Size();
  int W = term.dimx, H = term.dimy, pane_w = W - 24;
  Elements L;  // the page, one element per row
  auto row = [&] { return static_cast<int>(L.size()); };
  auto focus_then = [this](int id, std::function<void()> act) {  // a click also moves the focus there
    return [this, id, act] {
      set_focus = id;
      act();
    };
  };
  auto focused = [&](Element e, int id) {
    return id == set_focus ? (truecolor() ? e | bg(Pal::MenuSel) : e | inverted) : e;
  };
  auto field = [&](const std::string& v, int w, int id, std::function<void()> act) {
    return clickable(focused(txt(" " + padded(clipped(v, w), w) + " ") | bg(Pal::Chip), id), focus_then(id, act));
  };
  auto dropdown = [&](const std::string& v, int id, std::function<void()> act, const std::string& key) {  // key: its menu hangs there
    return clickable(focused(hbox({txt(" " + v + " "), txt("▾ ", Pal::Dim)}) | bg(Pal::Chip), id) | reflect(anchors[key]), focus_then(id, act));
  };
  auto check = [&](bool on, const std::string& label, int id, std::function<void()> act) {
    return clickable(focused(hbox({txt(on ? "[x]" : "[ ]", on ? Pal::Accent : Pal::Dim), txt(" " + label)}), id), focus_then(id, act));
  };
  auto title = [&](const std::string& t) { L.push_back(txt(t, Pal::Accent, true)); };
  auto hr = [&] {
    std::string line;
    for (int i = 0; i < pane_w - 2; ++i) line += "─";
    L.push_back(txt(line, Pal::Faint));
  };
  auto number = [&](const char* label, int& value, int min, int step, const char* about) {
    int r = row();
    auto set = [this, &value, min](int v) {
      value = std::max(min, v);
      write_config("saved");
    };
    int id = control(
        [this, label, &value, set, min] {
          prompt(std::string(label) + " (seconds)", std::to_string(value), "at least " + std::to_string(min),
                 [set](const std::string& s) {
                   if (s.empty() || !all_digits(s) || s.size() > 6) return std::string("a whole number of seconds");
                   set(std::stoi(s));
                   return std::string();
                 });
        },
        [&value, set, step](int d) { set(value + d * step); }, r);
    L.push_back(hbox({txt(padded(label, 22)), clickable(txt(" − ", Pal::Accent) | bg(Pal::Btn), [set, &value, step] { set(value - step); }),
                      text(" "), field(std::to_string(value), 5, id, controls[static_cast<size_t>(id)].act), text(" "),
                      clickable(txt(" + ", Pal::Accent) | bg(Pal::Btn), [set, &value, step] { set(value + step); }), txt(" s", Pal::Dim),
                      txt("       " + std::string(about), Pal::Dim)}));
  };

  if (set_page == 0) {
    L.push_back(text(""));
    title("General");
    L.push_back(text(""));
    number("Refresh every", cfg.refresh_seconds, 10, 10, "the list reloads on its own; r reloads now");
    L.push_back(text(""));
    number("Watch reviewed PRs", cfg.watch_seconds, 5, 5, "PRs a plugin marks as watched, polled quietly");
    L.push_back(text(""));
    auto toggle_update = [this] {
      cfg.auto_update = !cfg.auto_update;
      write_config("saved");
    };
    int id = control(toggle_update, [toggle_update](int) { toggle_update(); }, row());
    L.push_back(hbox({txt(padded("Update anton", 22)), check(cfg.auto_update, "download new releases", id, toggle_update)}));
    if (!update_note.empty()) L.push_back(hbox({text(std::string(22, ' ')), txt("⬆ " + update_note, Pal::Green)}));
    L.push_back(text(""));
    std::string start = cfg.start_view;
    if (start.empty())
      for (auto& t : tabs)
        if (t.saved) {
          start = t.saved->name;
          break;
        }
    auto pick_start = [this] {
      Menu m;
      m.title = "start on";
      m.w = 34;
      m.y = anchors["set:start"].y_max + 1;
      m.x = anchors["set:start"].x_min;
      m.items = [this] {
        std::vector<MenuItem> items;
        bool first = true;
        for (auto& t : tabs) {
          if (!t.saved) continue;
          MenuItem it{MenuItem::Normal, t.saved->name, first ? "the first tab" : "", [this, name = t.saved->name, first] {
                        cfg.start_view = first ? "" : name;
                        write_config("saved");
                      }};
          it.radio = true;
          it.on = cfg.start_view == t.saved->name || (first && cfg.start_view.empty());
          items.push_back(it);
          first = false;
        }
        return items;
      };
      push_menu(m);
    };
    id = control(pick_start, nullptr, row());
    L.push_back(hbox({txt(padded("Start on", 22)), dropdown(start.empty() ? "(none saved)" : start, id, pick_start, "set:start"),
                      txt("   or pass --query for a one-off view", Pal::Dim)}));
    L.push_back(text(""));
    auto toggle_mouse = [this] {
      cfg.mouse = !cfg.mouse;
      screen.TrackMouse(cfg.mouse);
      screen.WithRestoredIO([] {})();  // reinstalls the terminal with or without mouse reports
      write_config(cfg.mouse ? "mouse on" : "mouse off: the terminal selects text again");
    };
    id = control(toggle_mouse, [toggle_mouse](int) { toggle_mouse(); }, row());
    L.push_back(hbox({txt(padded("Mouse", 22)), check(cfg.mouse, "clicks and wheel", id, toggle_mouse),
                      txt("   Shift-drag still selects text in most terminals", Pal::Dim)}));
    L.push_back(text(""));
    hr();
    title("Plugins");
    L.push_back(txt(cfg.plugins.empty() ? "None configured. A plugin's command and options live in config.json (e opens it)."
                                        : "Status only: a plugin's command and options live in config.json (e opens it).",
                    Pal::Dim));
    for (size_t i = 0; i < cfg.plugins.size(); ++i) {
      L.push_back(text(""));
      const PluginState& st = live[i];
      bool up = plugin_up(i);
      std::string state = one_shot(i) ? (last_code[i] == 0 ? "ran" : "exit " + std::to_string(last_code[i]))
                          : up        ? "running"
                                      : st.health.empty() ? "starting" : st.health;
      std::string what;
      for (auto& a : st.actions) what += (what.empty() ? "" : " · ") + a.key + " " + (a.button.empty() ? a.label : a.button);
      what += (what.empty() ? "" : " · ") + std::to_string(st.prs.size()) + (st.prs.size() == 1 ? " PR" : " PRs");
      std::string cmd;
      for (auto& a : cfg.plugins[i].command) cmd += (cmd.empty() ? "" : " ") + a;
      auto restart = [this, i] {
        if (!plugins) return flash("plugins are off (--no-plugins)", 3, true);
        plugins->restart(i);
        flash("restarting " + cfg.plugins[i].name);
      };
      id = control(restart, nullptr, row());
      int what_w = std::max(10, pane_w - 8 - 14 - 22 - 12), cmd_w = std::min(30, pane_w - 8 - 14 - what_w - 2 - 12);
      L.push_back(hbox({txt(padded(cfg.plugins[i].name, 8), Pal::Fg, true), txt(up ? "● " : "○ ", up ? Pal::Green : Pal::Red),
                        txt(padded(state, 12), Pal::Dim), txt(padded(clipped(what, what_w), what_w + 2)),
                        cmd_w >= 10 ? txt(clipped("$ " + cmd, cmd_w), Pal::Faint) : text(""), filler(),
                        clickable(focused(button("", "Restart"), id), restart), text(" ")}));
      if (!st.health_text.empty()) L.push_back(txt("        " + clipped(st.health_text, pane_w - 10), Pal::Dim));
      if (!st.last_stderr.empty()) L.push_back(txt("        stderr: " + clipped(st.last_stderr, pane_w - 18), Pal::Dim));
    }
  } else if (set_page == 1) {
    set_view = std::min(set_view, tabs.size() - 1);
    L.push_back(text(""));
    title("Views");
    L.push_back(txt("The tabs across the top, in this order; anton opens on the first. Drag ⠿ or use J/K to reorder.", Pal::Dim));
    L.push_back(text(""));
    L.push_back(txt("   NAME              KIND      FILTER", Pal::Dim));
    for (size_t i = 0; i < tabs.size(); ++i) {
      const View& v = tabs[i].saved ? *tabs[i].saved : tabs[i].v;
      int id = control([this, i] { set_view = i; }, nullptr, row());
      controls.back().move = [this, i](int d) {
        move_view(i, d);
        if (i + d < tabs.size()) set_view = i + d;
      };
      Element r = hbox({txt("⠿  ", Pal::Dim), txt(padded(clipped(v.name, 17), 18), Pal::Fg, i == set_view),
                        txt(padded(v.is_list() ? "list" : "search", 10), v.is_list() ? Pal::Cyan : Pal::Dim),
                        v.is_list() ? txt(clipped(v.plugin + " · " + v.list + " · " + v.query, pane_w - 32)) : colored_query(clipped(v.query, pane_w - 32)),
                        txt(tabs[i].saved ? "" : "  (not saved)", Pal::Yellow), filler()});
      if (i == set_view) r = truecolor() ? r | bg(Pal::Sel) : r | inverted;
      r = focused(r, id);
      Hit& h = hit(r);
      h.click = [this, i] { set_view = i; };
      h.drag = 1;
      h.index = static_cast<int>(i);
      h.drop = [this, i](int from) {
        if (from < 0 || static_cast<size_t>(from) >= tabs.size()) return;
        while (static_cast<size_t>(from) != i) {
          int d = static_cast<size_t>(from) < i ? 1 : -1;
          move_view(static_cast<size_t>(from), d);
          from += d;
        }
        set_view = i;
      };
      L.push_back(r);
    }
    auto add = [this] {
      Tab t{View{"New view", "is:open", "", "", {}}, std::nullopt};
      for (int k = 2; std::any_of(tabs.begin(), tabs.end(), [&](const Tab& o) { return o.v.name == t.v.name; }); ++k)
        t.v.name = "New view " + std::to_string(k);
      t.saved = t.v;
      tabs.push_back(t);
      set_view = tabs.size() - 1;
      write_config("added " + t.v.name);
    };
    int id = control(add, nullptr, row());
    L.push_back(hbox({text(" "), clickable(focused(hbox({txt(" + ", Pal::Accent, true), txt("New view ")}) | bg(Pal::Chip), id), add)}));
    L.push_back(text(""));
    hr();
    size_t vi = set_view;
    const View& v = tabs[vi].saved ? *tabs[vi].saved : tabs[vi].v;
    L.push_back(txt(v.name, Pal::Fg, true));
    L.push_back(text(""));
    auto rename = [this, vi] {
      prompt("Rename view", tabs[vi].v.name, "", [this, vi](const std::string& name) { return rename_view(vi, name); });
    };
    id = control(rename, nullptr, row());
    L.push_back(hbox({txt(padded("Name", 11), Pal::Dim), field(v.name, 30, id, rename)}));
    L.push_back(text(""));
    auto kind = [this, vi] {
      bool to_list = !tabs[vi].v.is_list();
      if (to_list && cfg.plugins.empty()) return flash("list views need a plugin; none is configured", 4, true);
      edit_view(vi, [this, to_list](View& w) {
        if (to_list) {
          w.plugin = cfg.plugins[0].name;
          auto lists = plugin_lists(w.plugin);
          w.list = lists.empty() ? "code-review" : lists[0];
          std::string err;
          if (!parse_local(w.query, err)) w.query = "is:open";
        } else {
          w.plugin.clear();
          w.list.clear();
        }
        if (w.sort == Sort::List && !w.is_list()) w.sort.reset();
      });
    };
    id = control(kind, [kind](int) { kind(); }, row());
    L.push_back(hbox({txt(padded("Shows", 11), Pal::Dim),
                      clickable(focused(hbox({txt(v.is_list() ? "○ " : "● ", v.is_list() ? Pal::Dim : Pal::Accent),
                                              txt("a GitHub search", Pal::Fg, !v.is_list())}), id),
                                [this, kind, vi] { if (tabs[vi].v.is_list()) kind(); }),
                      text("    "),
                      clickable(hbox({txt(v.is_list() ? "● " : "○ ", v.is_list() ? Pal::Accent : Pal::Dim),
                                      txt("the PRs a plugin lists", Pal::Fg, v.is_list())}),
                                [this, kind, vi] { if (!tabs[vi].v.is_list()) kind(); })}));
    if (v.is_list()) {
      L.push_back(text(""));
      auto pick_plugin = [this, vi] {
        Menu m;
        m.title = "plugin";
        m.w = 30;
        m.x = anchors["set:plugin"].x_min;
        m.y = anchors["set:plugin"].y_max + 1;
        m.items = [this, vi] {
          std::vector<MenuItem> items;
          for (auto& p : cfg.plugins) {
            MenuItem it{MenuItem::Normal, p.name, "", [this, vi, name = p.name] { edit_view(vi, [name](View& w) { w.plugin = name; }); }};
            it.radio = true;
            it.on = tabs[vi].v.plugin == p.name;
            items.push_back(it);
          }
          return items;
        };
        push_menu(m);
      };
      auto pick_list = [this, vi] {
        Menu m;
        m.title = "list";
        m.w = 36;
        m.x = anchors["set:list"].x_min;
        m.y = anchors["set:list"].y_max + 1;
        m.input = true;
        m.placeholder = "a list name";
        m.items = [this, vi] {
          std::vector<MenuItem> items;
          const std::string typed = menus.empty() ? "" : menus.back().edit.s;
          for (auto& l : plugin_lists(tabs[vi].v.plugin)) {
            if (!fuzzy(l, typed)) continue;
            MenuItem it{MenuItem::Normal, l, "", [this, vi, l] { edit_view(vi, [l](View& w) { w.list = l; }); }};
            it.radio = true;
            it.on = tabs[vi].v.list == l;
            items.push_back(it);
          }
          return items;
        };
        m.submit = [this, vi](const std::string& l) {
          if (l.empty()) return std::string("type a list name");
          edit_view(vi, [l](View& w) { w.list = l; });
          return std::string();
        };
        m.foot = {"the lists the plugin offers right now;", "⏎ takes a name it hasn't listed yet"};
        push_menu(m);
      };
      id = control(pick_plugin, nullptr, row());
      int id2 = control(pick_list, nullptr, row());
      L.push_back(hbox({txt(padded("Plugin", 11), Pal::Dim), dropdown(v.plugin, id, pick_plugin, "set:plugin"), txt("   list ", Pal::Dim),
                        dropdown(v.list, id2, pick_list, "set:list"), txt("   lists the plugin offers right now", Pal::Faint)}));
    }
    L.push_back(text(""));
    auto filter = [this, vi] {
      const View& w = tabs[vi].saved ? *tabs[vi].saved : tabs[vi].v;
      prompt("Filter for " + w.name, w.query,
             w.is_list() ? "list views filter locally" : "GitHub search syntax", [this, vi](const std::string& raw) {
               std::string q = join(tokenize(raw)), err;
               if (tabs[vi].v.is_list() && !parse_local(q, err)) return err;
               edit_view(vi, [q](View& x) { x.query = q; });
               return std::string();
             });
    };
    id = control(filter, nullptr, row());
    L.push_back(hbox({txt(padded("Filter", 11), Pal::Dim), field(v.query, std::max(20, std::min(60, pane_w - 16)), id, filter)}));
    if (v.is_list())
      L.push_back(txt(std::string(12, ' ') + "list views filter locally: is: draft org: repo: author: review: label: reviewed-by:@me recent:<N>h|d",
                      Pal::Faint));
    L.push_back(text(""));
    auto pick_sort = [this, vi] {
      Menu m;
      m.title = "sort";
      m.w = 28;
      m.x = anchors["set:sort"].x_min;
      m.y = anchors["set:sort"].y_max + 1;
      m.items = [this, vi] {
        std::vector<MenuItem> items;
        for (Sort s : {Sort::List, Sort::Updated, Sort::Created, Sort::Size, Sort::Repo}) {
          if (s == Sort::List && !tabs[vi].v.is_list()) continue;
          MenuItem it{MenuItem::Normal, s == Sort::List ? "list order" : sort_name(s), "",
                      [this, vi, s] { edit_view(vi, [s](View& w) { w.sort = s; }); }};
          it.radio = true;
          it.on = (tabs[vi].saved ? *tabs[vi].saved : tabs[vi].v).sort_or_default() == s;
          items.push_back(it);
        }
        return items;
      };
      push_menu(m);
    };
    id = control(pick_sort, nullptr, row());
    L.push_back(hbox({txt(padded("Sort", 11), Pal::Dim), dropdown(v.sort_or_default() == Sort::List ? "list order" : sort_name(v.sort_or_default()), id, pick_sort, "set:sort")}));
    L.push_back(text(""));
    L.push_back(text(""));
    auto dup = [this, vi] {
      duplicate_view(vi);
      set_view = vi + 1;
    };
    auto del = [this, vi] {
      if (tabs.size() == 1) return flash("anton needs one view; this is the last", 3, true);
      confirm("Delete " + tabs[vi].v.name + "?", "Delete " + tabs[vi].v.name, [this, vi] { delete_view(vi); });
    };
    id = control(dup, nullptr, row());
    int id_del = control(del, nullptr, row());
    L.push_back(hbox({text(std::string(11, ' ')), clickable(focused(button("", "Duplicate"), id), dup), text(" "),
                      clickable(focused(button("", "Delete view", Pal::Btn, Pal::Red), id_del), del)}));
  } else {
    // Every built-in, the ones config.json leaves out shown off: turning one
    // on writes the whole list, off ones as {"name", "hidden": true}.
    std::vector<Column> all = cfg.columns;
    for (auto& n : kColumns)
      if (std::none_of(all.begin(), all.end(), [&](const Column& c) { return c.command.empty() && c.name == n; }))
        all.push_back({n, {}, 10, 10, true});
    L.push_back(text(""));
    title("Columns");
    L.push_back(txt("What the PR list shows, left to right. Space toggles; drag ⠿ or J/K to reorder.", Pal::Dim));
    L.push_back(text(""));
    for (size_t i = 0; i < all.size(); ++i) {
      const Column& c = all[i];
      auto toggle = [this, all, i] {
        std::vector<Column> next = all;
        next[i].hidden = !next[i].hidden;
        if (std::all_of(next.begin(), next.end(), [](const Column& x) { return x.hidden; }))
          return flash("the list needs one column", 3, true);
        cfg.columns = next;
        write_config("saved");
      };
      auto move = [this, all, i](int d) {
        long j = static_cast<long>(i) + d;
        if (j < 0 || j >= static_cast<long>(all.size())) return;
        std::vector<Column> next = all;
        std::swap(next[i], next[static_cast<size_t>(j)]);
        cfg.columns = next;
        write_config("saved");
      };
      int id = control(toggle, [toggle](int) { toggle(); }, row());
      controls.back().move = move;
      std::string about = c.command.empty() ? column_about(c)
                                            : "command column · width " + std::to_string(c.width) + " · command in config.json";
      Element r = hbox({txt("⠿  ", Pal::Dim), txt(c.hidden ? "[ ]" : "[x]", c.hidden ? Pal::Dim : Pal::Accent),
                        txt("  " + padded(c.name == "number" ? "#" : clipped(c.name, 9), 10), c.hidden ? Pal::Dim : Pal::Fg),
                        txt(clipped(about, pane_w - 20), Pal::Dim), filler()});
      r = focused(r, id);
      Hit& h = hit(r);
      h.click = toggle;
      h.drag = 2;
      h.index = static_cast<int>(i);
      h.drop = [this, all, i](int from) {
        if (from < 0 || static_cast<size_t>(from) >= all.size()) return;
        std::vector<Column> next = all;
        Column moved = next[static_cast<size_t>(from)];
        next.erase(next.begin() + from);
        next.insert(next.begin() + static_cast<long>(i), moved);
        cfg.columns = next;
        write_config();
      };
      L.push_back(r);
    }
    L.push_back(text(""));
    auto size_prompt = [this](const char* what, long& value, bool green) {
      prompt(std::string("Size: ") + what, std::to_string(value), "lines changed (+ and −)", [this, &value, green](const std::string& s) {
        if (s.empty() || !all_digits(s) || s.size() > 7) return std::string("a whole number of lines");
        long n = std::stol(s);
        if (green ? n > cfg.red_from : n < cfg.green_below) return std::string("green below must not pass red from");
        value = n;
        write_config("saved");
        return std::string();
      });
    };
    auto adjust = [this](long& value, bool green, int d) {
      long n = std::max(0L, value + d * 50L);
      if (green ? n > cfg.red_from : n < cfg.green_below) return;
      value = n;
      write_config("saved");
    };
    int g = control([size_prompt, this] { size_prompt("green below", cfg.green_below, true); },
                    [adjust, this](int d) { adjust(cfg.green_below, true, d); }, row());
    int rd = control([size_prompt, this] { size_prompt("red from", cfg.red_from, false); },
                     [adjust, this](int d) { adjust(cfg.red_from, false, d); }, row());
    auto sample = [&](long total, Pal p) {
      long a = total * 9 / 10;
      return txt("+" + compact(a) + " -" + compact(total - a) + "   ", p);
    };
    L.push_back(hbox({txt(padded("Size colors", 14), Pal::Dim), txt("green below ", Pal::Dim),
                      field(std::to_string(cfg.green_below), 5, g, controls[static_cast<size_t>(g)].act), txt("   red from ", Pal::Dim),
                      field(std::to_string(cfg.red_from), 5, rd, controls[static_cast<size_t>(rd)].act), text("     "),
                      sample(std::max(1L, cfg.green_below / 2), Pal::Green), sample((cfg.green_below + cfg.red_from) / 2, Pal::Yellow),
                      sample(cfg.red_from * 3, Pal::Red)}));
    L.push_back(text(""));
    auto strip = [this] {
      prompt("Title strip (a regex, cut from every title shown)", cfg.title_strip, "empty: off", [this](const std::string& s) {
        try {
          cfg.title_re = std::regex(s);
        } catch (const std::regex_error& e) {
          return "bad regex: " + std::string(e.what());
        }
        cfg.title_strip = s;
        write_config("saved");
        return std::string();
      });
    };
    int id = control(strip, nullptr, row());
    std::string before = "[ABC-123] Fix the login page";
    for (auto& [u, pr] : cache)
      if (!cfg.title_strip.empty() && shown_title(pr.title) != pr.title) {
        before = pr.title;
        break;
      }
    L.push_back(hbox({txt(padded("Title strip", 14), Pal::Dim), field(cfg.title_strip.empty() ? "(off)" : cfg.title_strip, 26, id, strip),
                      text("  "), txt(clipped(before, 28), Pal::Faint), txt("  →  ", Pal::Dim), txt(clipped(shown_title(before), 24))}));
    L.push_back(text(""));
    hr();
    L.push_back(txt("Preview", Pal::Dim, true));
    L.push_back(text(""));
    auto rs = rows(active).value_or(std::vector<Pr>{});
    if (rs.size() > 3) rs.resize(3);
    if (rs.empty()) L.push_back(txt("(the active view has no PRs to show yet)", Pal::Faint));
    else L.push_back(render_table(rs, static_cast<int>(rs.size()), false) | size(WIDTH, EQUAL, pane_w));
  }

  // -- the frame: header, nav, the page scrolled to the focused control -------------
  set_focus = std::clamp(set_focus, 0, std::max(0, static_cast<int>(controls.size()) - 1));
  int h = std::max(1, H - 3), n = static_cast<int>(L.size());
  if (!controls.empty()) {
    int r = controls[static_cast<size_t>(set_focus)].row;
    set_scroll = std::clamp(set_scroll, std::min(r, r - h + 3), r);
  }
  set_scroll = std::clamp(set_scroll, 0, std::max(0, n - h));
  Elements page;
  for (int i = set_scroll; i < std::min(n, set_scroll + h); ++i)  // a row too wide is cut, not squeezed
    page.push_back(hbox({text("  "), L[static_cast<size_t>(i)]}) | xframe);
  Element pane = vbox(std::move(page)) | flex;
  hit(pane).wheel = [this, n, h](int d) { set_scroll = std::clamp(set_scroll + 3 * d, 0, std::max(0, n - h)); };

  Elements nav = {text("")};
  static const char* pages[] = {"General", "Views", "Columns"};
  for (int i = 0; i < 3; ++i) {
    bool on = i == set_page;
    Element e = hbox({txt(on ? "▌  " : "   ", Pal::Accent), txt(pages[i], on ? Pal::Fg : Pal::Dim, on), filler()}) | size(WIDTH, EQUAL, 20);
    if (on) e = truecolor() ? e | bg(Pal::Sel) : e | inverted;
    nav.push_back(clickable(e, [this, i] {
      set_page = i;
      set_focus = set_scroll = 0;
    }));
    nav.push_back(text(""));
  }
  nav.push_back(text(""));
  nav.push_back(txt("   j/k move", Pal::Faint));
  nav.push_back(txt("   Tab next section", Pal::Faint));
  nav.push_back(txt("   ⏎ change", Pal::Faint));

  Element header = hbox({txt(" anton ", Pal::Ink, true) | bg(Pal::Accent), txt("  Settings", Pal::Fg, true), filler(),
                         clickable(button("e", "Open config.json"), [this] { edit_config(); }), text("  "),
                         clickable(button("esc", "Close"), [this] { mode = Mode::Normal; })});
  Element line = now() < notice_until && !notice.empty()
                     ? txt(" " + notice, notice_bad ? Pal::Red : Pal::Cyan)
                     : txt(clipped(" Saved to " + tilde(config_path()) + " as you change things. Plugin commands and command columns stay in the file.", W),
                           Pal::Dim);
  return vbox({header, line, rule(),
               hbox({vbox(std::move(nav)) | size(WIDTH, EQUAL, 20), separator() | color(pal(Pal::Faint)), pane}) | flex});
}

bool Ui::settings_key(const Event& e) {
  auto ch = [&](const char* c) { return e == Event::Character(c); };
  int n = static_cast<int>(controls.size());
  const Control* c = n ? &controls[static_cast<size_t>(std::clamp(set_focus, 0, n - 1))] : nullptr;
  if (e == Event::Escape || ch("q")) {
    mode = Mode::Normal;
  } else if (e == Event::Tab || e == Event::TabReverse) {
    set_page = (set_page + (e == Event::Tab ? 1 : 2)) % 3;
    set_focus = set_scroll = 0;
  } else if (ch("j") || e == Event::ArrowDown) {
    set_focus = std::min(set_focus + 1, std::max(0, n - 1));
  } else if (ch("k") || e == Event::ArrowUp) {
    set_focus = std::max(set_focus - 1, 0);
  } else if (e == Event::Return || ch(" ")) {
    if (c && c->act) c->act();
  } else if (ch("h") || ch("-") || e == Event::ArrowLeft || ch("l") || ch("+") || e == Event::ArrowRight) {
    if (c && c->adj) c->adj(ch("h") || ch("-") || e == Event::ArrowLeft ? -1 : 1);
  } else if (ch("J") || ch("K")) {
    if (c && c->move) {
      int d = ch("J") ? 1 : -1;
      auto move = c->move;  // the controls are rebuilt on the next frame
      move(d);
      set_focus = std::clamp(set_focus + d, 0, std::max(0, n - 1));
    }
  } else if (ch("e")) {
    edit_config();
  }
  return true;
}

// $EDITOR (else vi) on config.json, anton's screen put away meanwhile; then
// the file is read again. A file that doesn't load leaves things as they were.
void Ui::edit_config() {
  std::string path = config_path();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  const char* ed = std::getenv("EDITOR");
  std::vector<std::string> argv = tokenize(ed && *ed ? ed : "vi");  // "code -w" is two words
  argv.push_back(path);
  int code = -1;
  screen.WithRestoredIO([&] { code = run_tty(argv); })();
  if (code != 0) return flash(argv[0] + (code < 0 ? ": couldn't run it" : ": exited " + std::to_string(code)), 6, true);
  Config c;
  if (std::string err = load_config(c); !err.empty()) return flash("config.json not reloaded: " + err, 10, true);
  apply_config(std::move(c));
}

void Ui::apply_config(Config c) {
  bool plugins_changed = c.plugins.size() != cfg.plugins.size();
  for (size_t i = 0; !plugins_changed && i < c.plugins.size(); ++i)
    plugins_changed = c.plugins[i].name != cfg.plugins[i].name || c.plugins[i].command != cfg.plugins[i].command ||
                      c.plugins[i].rerun != cfg.plugins[i].rerun || c.plugins[i].env != cfg.plugins[i].env;
  c.plugins = cfg.plugins;  // the running ones stay until anton restarts
  c.adhoc_query = cfg.adhoc_query;
  bool mouse = c.mouse != cfg.mouse;
  if (cfg.adhoc_query.empty()) {
    std::string name = view().name;
    tabs.clear();
    for (auto& v : c.views) tabs.push_back({v, v});
    active = 0;
    for (size_t i = 0; i < tabs.size(); ++i)
      if (tabs[i].v.name == name) active = i;
  } else {
    offscreen = c.views;
  }
  cfg = std::move(c);
  set_view = std::min(set_view, tabs.size() - 1);
  if (mouse) {
    screen.TrackMouse(cfg.mouse);
    screen.WithRestoredIO([] {})();
  }
  selected = list_top = 0;
  refresh();
  flash(plugins_changed ? "reloaded config.json; plugin changes take effect when anton restarts" : "reloaded config.json", 5);
}

}  // namespace anton
