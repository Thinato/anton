#pragma once
// ~/.config/anton/config.json -> Config. Every key is optional.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model.hpp"  // lower
#include "plugins.hpp"

namespace anton {

struct View {
  std::string name, query;
  std::string plugin, list;  // set together: a list view
  std::optional<Sort> sort;  // config "sort"; unset: list for list views, else updated
  bool notify = false;       // list views: a PR newly in it is told once (01-anton.md, "Notify")
  bool is_list() const { return !list.empty(); }
  Sort sort_or_default() const { return sort.value_or(is_list() ? Sort::List : Sort::Updated); }
  bool operator==(const View&) const = default;
};

// A GitHub user as you want to see them: the name your Slack plugin shows for
// them (so the author column and Slack tags line up), and a color.
struct Person {
  std::string name;  // "" = keep the login
  int rgb = -1;      // 0xRRGGBB from "hex"; used on truecolor terminals
  int ansi = -1;     // 0-255 from "ansi"; used on older ones (and if no hex)
};

// "red", "bright_red", "gray", or 0-255. -1 if none of those.
inline int parse_ansi(const nlohmann::json& v) {
  if (v.is_number_integer()) {
    int n = v.get<int>();
    return n >= 0 && n <= 255 ? n : -1;
  }
  if (!v.is_string()) return -1;
  static const char* base[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
  std::string s = lower(v.get<std::string>());
  if (s == "gray" || s == "grey") return 8;
  bool bright = s.rfind("bright_", 0) == 0;
  if (bright) s = s.substr(7);
  for (int i = 0; i < 8; ++i)
    if (s == base[i]) return i + (bright ? 8 : 0);
  return -1;
}

// "#rrggbb" -> 0xRRGGBB, -1 if malformed.
inline int parse_hex(const std::string& s) {
  if (s.size() != 7 || s[0] != '#') return -1;
  for (size_t i = 1; i < 7; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return -1;
  return static_cast<int>(std::strtol(s.c_str() + 1, nullptr, 16));
}

// "$HOME", "${HOME}" and a leading "~" in a plugin command, so a config can
// be shared: there's no shell to expand them (02-plugins.md).
inline std::string expand_home(std::string s) {
  const char* h = std::getenv("HOME");
  if (!h || !*h) return s;
  if (s == "~" || s.rfind("~/", 0) == 0) s.replace(0, 1, h);
  for (const char* var : {"${HOME}", "$HOME"})
    for (size_t at = s.find(var); at != std::string::npos; at = s.find(var, at + std::strlen(h)))
      s.replace(at, std::strlen(var), h);
  return s;
}

// A table column: a built-in, or a command whose output fills the cell
// (01-anton.md, "Columns").
struct Column {
  std::string name;                  // built-in name, or the command column's header
  std::vector<std::string> command;  // non-empty: a command column
  int width = 10;                    // command columns; built-ins size themselves
  int timeout = 10;                  // seconds
  bool hidden = false;               // off, but kept in the file to turn back on
  bool operator==(const Column&) const = default;
};

inline const std::vector<std::string> kColumns = {"repo", "number", "size", "state", "age", "author", "tags", "title"};

// The GUI's PR list columns, in their default order. The title is always
// shown, last; labels and tags go after it.
inline const std::vector<std::string> kGuiColumns = {"number", "repo", "size", "blocks", "state",
                                                     "age",    "author", "labels", "tags"};

struct GuiColumn {
  std::string name;
  bool shown = true;
  bool operator==(const GuiColumn&) const = default;
};

// anton-gui's own settings ("gui" in config.json, 06-gui.md). The TUI
// ignores them; config_write keeps them either way.
struct GuiPrefs {
  std::string theme = "system";              // dark | light | system (the OS appearance)
  int font_size = 12;                        // text sizes, 9-20 px: the interface (everything scales with it),
  int reading_size = 13, code_size = 11;     // descriptions and specs, mono numbers/paths/logs
  std::string code_font = "JetBrains Mono";  // or "System monospace"
  std::string density = "compact";           // compact | cozy | roomy: the PR list's rows, 24 | 28 | 34
  std::vector<GuiColumn> columns = [] {      // every kGuiColumns name once, in the order shown
    std::vector<GuiColumn> v;
    for (auto& n : kGuiColumns) v.push_back({n, n != "blocks"});
    return v;
  }();
  std::string author_style = "avatar";  // avatar | color: a circle, or the name in a colour
  std::string state_style = "dot";      // dot | text | pill
  std::string reader_placement = "beside_preview";  // beside_preview | last | window: where Open in reader puts it
  bool tooltips = true;                 // hover tips, the actions' long labels among them
  bool views = true, preview = true, results = true, run_log = true, status_bar = true;  // panes shown
  bool shows(const std::string& column) const {
    for (auto& c : columns)
      if (c.name == column) return c.shown;
    return false;
  }
  bool operator==(const GuiPrefs&) const = default;
};

struct Config {
  int refresh_seconds = 120;
  int watch_seconds = 15;  // PRs a plugin marks "watch": true
  bool auto_update = true;  // false: only tell when a release is out
  bool mouse = true;        // false: no mouse tracking, the terminal selects text
  std::string start_view;   // the landing view's name; "" = the first
  long green_below = 200, red_from = 1000;
  // "title_strip": a regex cut out of every title shown, e.g. a ticket prefix.
  std::string title_strip;
  std::regex title_re;
  // Mirrors GitHub's own pull-requests page; the first is the landing view.
  std::vector<View> views = {
      {"Created", "is:open author:@me", "", "", {}},
      {"Review requests", "is:open review-requested:@me", "", "", {}},
      {"Assigned", "is:open assignee:@me", "", "", {}},
      {"Mentioned", "is:open mentions:@me", "", "", {}},
  };
  std::vector<Column> columns = [] {
    std::vector<Column> v;
    for (auto& n : kColumns) v.push_back({n, {}});
    return v;
  }();
  std::string adhoc_query;  // --query: one unsaved view in place of `views` on screen
  std::vector<PluginConfig> plugins;
  std::map<std::string, Person> people;  // lowercased GitHub login -> person
  GuiPrefs gui;
  std::string gui_error;  // a bad "gui" value: anton-gui says so, the TUI ignores it
};

// $XDG_CONFIG_HOME/anton, default ~/.config/anton: config.json, the
// plugins' files, anton-gui's layout.
inline std::string config_dir() {
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/anton";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.config/anton";
}

inline std::string config_path() { return config_dir() + "/config.json"; }

// Where anton-gui keeps its panes' layout: per user, never the cwd.
inline std::string gui_layout_path() { return config_dir() + "/gui-layout.ini"; }

// The "gui" object; "" or its first problem.
inline std::string parse_gui(const nlohmann::json& g, GuiPrefs& p) {
  try {
    if (!g.is_object()) return "gui: want an object";
    p.theme = g.value("theme", p.theme);
    if (p.theme != "dark" && p.theme != "light" && p.theme != "system") return "gui.theme: dark, light or system";
    p.font_size = std::clamp(g.value("font_size", p.font_size), 9, 20);
    p.reading_size = std::clamp(g.value("reading_size", p.reading_size), 9, 20);
    p.code_size = std::clamp(g.value("code_size", p.code_size), 9, 20);
    p.tooltips = g.value("tooltips", p.tooltips);
    p.code_font = g.value("code_font", p.code_font);
    if (p.code_font != "JetBrains Mono" && p.code_font != "System monospace")
      return "gui.code_font: \"JetBrains Mono\" or \"System monospace\"";
    p.density = g.value("density", p.density);
    if (p.density == "comfortable") p.density = "roomy";  // the old name for the tallest rows
    if (p.density != "compact" && p.density != "cozy" && p.density != "roomy")
      return "gui.density: compact, cozy or roomy";
    p.author_style = g.value("author_style", p.author_style);
    if (p.author_style != "avatar" && p.author_style != "color") return "gui.author_style: avatar or color";
    p.state_style = g.value("state_style", p.state_style);
    if (p.state_style != "dot" && p.state_style != "text" && p.state_style != "pill")
      return "gui.state_style: dot, text or pill";
    p.reader_placement = g.value("reader_placement", p.reader_placement);
    if (p.reader_placement != "beside_preview" && p.reader_placement != "last" && p.reader_placement != "window")
      return "gui.reader_placement: beside_preview, last or window";
    if (g.contains("columns")) {  // "repo" shown, {"name": "blocks", "hidden": true} not; the rest after, hidden
      if (!g.at("columns").is_array()) return "gui.columns: want a list";
      std::vector<GuiColumn> cols;
      for (auto& v : g.at("columns")) {
        GuiColumn c{v.is_string() ? v.get<std::string>() : v.value("name", std::string()), true};
        if (v.is_object()) c.shown = !v.value("hidden", false);
        // the title always shows; a column from a newer anton is skipped, so
        // the rest of the gui keys still load (and aren't written over)
        if (std::find(kGuiColumns.begin(), kGuiColumns.end(), c.name) == kGuiColumns.end()) continue;
        if (std::none_of(cols.begin(), cols.end(), [&](const GuiColumn& x) { return x.name == c.name; })) cols.push_back(c);
      }
      for (auto& n : kGuiColumns)
        if (std::none_of(cols.begin(), cols.end(), [&](const GuiColumn& x) { return x.name == n; })) cols.push_back({n, false});
      p.columns = cols;
    }
    if (g.contains("panes")) {
      const nlohmann::json& ps = g.at("panes");
      p.views = ps.value("views", p.views);
      p.preview = ps.value("preview", p.preview);
      p.results = ps.value("results", p.results);
      p.run_log = ps.value("run_log", p.run_log);
      p.status_bar = ps.value("status_bar", p.status_bar);
    }
  } catch (const nlohmann::json::exception& e) {
    return std::string("gui: ") + e.what();
  }
  return "";
}

// Parses `text`; returns "" or the first problem found.
inline std::string parse_config(const std::string& text, Config& c) {
  using nlohmann::json;
  json j = json::parse(text, nullptr, false, true);  // allow comments
  if (j.is_discarded() || !j.is_object()) return "not a JSON object";
  try {
    c.refresh_seconds = std::max(10, j.value("refresh_seconds", c.refresh_seconds));
    c.watch_seconds = std::max(5, j.value("watch_seconds", c.watch_seconds));
    c.auto_update = j.value("auto_update", c.auto_update);
    c.mouse = j.value("mouse", c.mouse);
    c.start_view = j.value("start_view", c.start_view);
    c.title_strip = j.value("title_strip", c.title_strip);
    if (!c.title_strip.empty()) {
      try {
        c.title_re = std::regex(c.title_strip);
      } catch (const std::regex_error& e) {
        return "title_strip: bad regex: " + std::string(e.what());
      }
    }
    json people = j.value("people", json::object());  // a named copy: items() keeps a reference
    for (auto& [login, v] : people.items()) {
      Person p;
      p.name = v.value("name", std::string());
      if (v.contains("hex") && (p.rgb = parse_hex(v.value("hex", std::string()))) < 0)
        return "people." + login + ".hex: want \"#rrggbb\"";
      if (v.contains("ansi") && (p.ansi = parse_ansi(v["ansi"])) < 0)
        return "people." + login + ".ansi: want 0-255 or a name like \"red\", \"bright_red\"";
      c.people[lower(login)] = p;
    }
    // anton-gui's keys: a bad one is anton-gui's to report (gui_error); it
    // must not keep the terminal UI from starting. They go back to defaults.
    if (j.contains("gui")) c.gui_error = parse_gui(j.at("gui"), c.gui);
    if (!c.gui_error.empty()) c.gui = GuiPrefs{};
    if (j.contains("size")) {
      c.green_below = j["size"].value("green_below", c.green_below);
      c.red_from = j["size"].value("red_from", c.red_from);
    }
    if (j.contains("views")) {
      c.views.clear();
      for (auto& v : j.at("views")) {
        View view{v.at("name").get<std::string>(), v.value("query", std::string("is:open")),
                  v.value("plugin", std::string()), v.value("list", std::string()), {}, v.value("notify", false)};
        if (view.plugin.empty() != view.list.empty())
          return "view \"" + view.name + "\": plugin and list go together";
        if (v.contains("sort")) {
          view.sort = parse_sort(v.at("sort").get<std::string>());
          if (!view.sort || (*view.sort == Sort::List && !view.is_list()))
            return "view \"" + view.name + "\": sort is updated, created, size or repo (or list, in list views)";
        }
        c.views.push_back(view);
      }
      if (c.views.empty()) return "views: needs at least one view";
    }
    for (auto& p : j.value("plugins", json::array())) {
      PluginConfig pc;
      pc.name = p.at("name").get<std::string>();
      pc.command = p.at("command").get<std::vector<std::string>>();
      for (auto& a : pc.command) a = expand_home(a);
      pc.rerun = p.value("rerun", pc.rerun);
      pc.env = p.value("env", pc.env);
      if (pc.command.empty()) return "plugin \"" + pc.name + "\": empty command";
      if (pc.rerun != "exit" && pc.rerun != "refresh" && pc.rerun != "never")
        return "plugin \"" + pc.name + "\": rerun must be exit, refresh or never";
      c.plugins.push_back(pc);
    }
    if (j.contains("columns")) {
      c.columns.clear();
      for (auto& v : j.at("columns")) {
        Column col;
        if (v.is_string() || !v.contains("command")) {  // a built-in: "repo", or {"name": "repo", "hidden": true}
          col.name = v.is_string() ? v.get<std::string>() : v.at("name").get<std::string>();
          col.hidden = v.is_object() && v.value("hidden", false);
          if (std::find(kColumns.begin(), kColumns.end(), col.name) == kColumns.end())
            return "columns: no column \"" + col.name + "\" (built in: " + join(kColumns) +
                   "; or {\"name\": ..., \"command\": [...]})";
        } else {
          col.name = v.at("name").get<std::string>();
          col.command = v.at("command").get<std::vector<std::string>>();
          for (auto& a : col.command) a = expand_home(a);
          col.width = std::clamp(v.value("width", col.width), 1, 80);
          col.timeout = std::clamp(v.value("timeout", col.timeout), 1, 300);
          col.hidden = v.value("hidden", false);
          if (col.command.empty()) return "column \"" + col.name + "\": empty command";
        }
        c.columns.push_back(col);
      }
      if (std::all_of(c.columns.begin(), c.columns.end(), [](const Column& col) { return col.hidden; }))
        return "columns: needs at least one column that isn't hidden";
    }
    if (c.plugins.size() > Plugins::kMaxPlugins)
      return "at most " + std::to_string(Plugins::kMaxPlugins) + " plugins";
    for (auto& v : c.views) {
      if (!v.is_list()) continue;
      bool known = false;
      for (auto& p : c.plugins) known = known || p.name == v.plugin;
      if (!known) return "view \"" + v.name + "\": no plugin named \"" + v.plugin + "\"";
    }
  } catch (const json::exception& e) {
    return e.what();
  }
  return "";
}

// Missing file = defaults. Returns "" or "<path>: <problem>".
inline std::string load_config(Config& c) {
  std::string path = config_path();
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  std::string err = parse_config(ss.str(), c);
  return err.empty() ? "" : path + ": " + err;
}

}  // namespace anton
