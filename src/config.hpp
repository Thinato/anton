#pragma once
// ~/.config/anton/config.json -> Config. Every key is optional.

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugins.hpp"

namespace anton {

struct View {
  std::string name, query;
  std::string plugin, list;  // set together: a list view
  bool is_list() const { return !list.empty(); }
};

struct Config {
  int refresh_seconds = 120;
  long green_below = 200, red_from = 1000;
  // Mirrors GitHub's own pull-requests page; the first is the landing view.
  std::vector<View> views = {
      {"Created", "is:open author:@me", "", ""},
      {"Review requests", "is:open review-requested:@me", "", ""},
      {"Assigned", "is:open assignee:@me", "", ""},
      {"Mentioned", "is:open mentions:@me", "", ""},
  };
  std::vector<PluginConfig> plugins;
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
      pc.rerun = p.value("rerun", pc.rerun);
      pc.env = p.value("env", pc.env);
      if (pc.command.empty()) return "plugin \"" + pc.name + "\": empty command";
      if (pc.rerun != "exit" && pc.rerun != "refresh" && pc.rerun != "never")
        return "plugin \"" + pc.name + "\": rerun must be exit, refresh or never";
      c.plugins.push_back(pc);
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
