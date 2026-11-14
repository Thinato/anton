#pragma once
// ~/.config/anton/config.json -> Config. Every key is optional.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
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
  bool is_list() const { return !list.empty(); }
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
};

inline const std::vector<std::string> kColumns = {"repo", "number", "size", "state", "age", "author", "tags", "title"};

struct Config {
  int refresh_seconds = 120;
  int watch_seconds = 15;  // PRs a plugin marks "watch": true
  bool auto_update = true;  // false: only tell when a release is out
  long green_below = 200, red_from = 1000;
  // "title_strip": a regex cut out of every title shown, e.g. a ticket prefix.
  std::string title_strip;
  std::regex title_re;
  // Mirrors GitHub's own pull-requests page; the first is the landing view.
  std::vector<View> views = {
      {"Created", "is:open author:@me", "", ""},
      {"Review requests", "is:open review-requested:@me", "", ""},
      {"Assigned", "is:open assignee:@me", "", ""},
      {"Mentioned", "is:open mentions:@me", "", ""},
  };
  std::vector<Column> columns = [] {
    std::vector<Column> v;
    for (auto& n : kColumns) v.push_back({n, {}});
    return v;
  }();
  std::vector<PluginConfig> plugins;
  std::map<std::string, Person> people;  // lowercased GitHub login -> person
};

inline std::string config_path() {
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return std::string(x) + "/anton/config.json";
  const char* home = std::getenv("HOME");
  return std::string(home ? home : ".") + "/.config/anton/config.json";
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
    if (j.contains("size")) {
      c.green_below = j["size"].value("green_below", c.green_below);
      c.red_from = j["size"].value("red_from", c.red_from);
    }
    if (j.contains("views")) {
      c.views.clear();
      for (auto& v : j.at("views")) {
        View view{v.at("name").get<std::string>(), v.value("query", std::string("is:open")),
                  v.value("plugin", std::string()), v.value("list", std::string())};
        if (view.plugin.empty() != view.list.empty())
          return "view \"" + view.name + "\": plugin and list go together";
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
        if (v.is_string()) {
          col.name = v.get<std::string>();
          if (std::find(kColumns.begin(), kColumns.end(), col.name) == kColumns.end())
            return "columns: no column \"" + col.name + "\" (built in: " + join(kColumns) +
                   "; or {\"name\": ..., \"command\": [...]})";
        } else {
          col.name = v.at("name").get<std::string>();
          col.command = v.at("command").get<std::vector<std::string>>();
          for (auto& a : col.command) a = expand_home(a);
          col.width = std::clamp(v.value("width", col.width), 1, 80);
          col.timeout = std::clamp(v.value("timeout", col.timeout), 1, 300);
          if (col.command.empty()) return "column \"" + col.name + "\": empty command";
        }
        c.columns.push_back(col);
      }
      if (c.columns.empty()) return "columns: needs at least one column";
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
