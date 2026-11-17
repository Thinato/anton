#pragma once
// Writing config.json back (01-anton.md, "Writing config.json"): the keys
// anton manages are set from a Config; every other key and value, and the
// order of keys, stay as the file has them. merged_config is pure;
// save_config does the I/O.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "config.hpp"

namespace anton {

// The file's text with c's managed keys in it. "" and err set when the file
// isn't something anton can rewrite without losing what's there.
inline std::string merged_config(const std::string& existing, const Config& c, std::string& err) {
  using oj = nlohmann::ordered_json;
  oj j = oj::object();
  if (existing.find_first_not_of(" \t\r\n") != std::string::npos) {
    j = oj::parse(existing, nullptr, false);
    if (j.is_discarded()) {
      bool commented = !oj::parse(existing, nullptr, false, true).is_discarded();
      err = commented ? "it has comments, which rewriting would drop; edit it by hand (e)"
                      : "it isn't valid JSON; fix it by hand (e)";
      return "";
    }
    if (!j.is_object()) {
      err = "it isn't a JSON object; fix it by hand (e)";
      return "";
    }
  }
  const Config d;  // defaults: a key at its default isn't added, only updated if there
  auto put = [&](const char* key, const oj& v, bool is_default) {
    if (j.contains(key) || !is_default) j[key] = v;
  };
  put("refresh_seconds", c.refresh_seconds, c.refresh_seconds == d.refresh_seconds);
  put("watch_seconds", c.watch_seconds, c.watch_seconds == d.watch_seconds);
  put("auto_update", c.auto_update, c.auto_update == d.auto_update);
  put("mouse", c.mouse, c.mouse == d.mouse);
  if (c.start_view.empty()) j.erase("start_view");
  else j["start_view"] = c.start_view;
  put("title_strip", c.title_strip, c.title_strip.empty());
  if (j.contains("size") && j["size"].is_object()) {
    j["size"]["green_below"] = c.green_below;
    j["size"]["red_from"] = c.red_from;
  } else {
    put("size", {{"green_below", c.green_below}, {"red_from", c.red_from}},
        c.green_below == d.green_below && c.red_from == d.red_from);
  }

  // anton-gui's settings, by the same rule one level down: a key is set
  // when the file has it or it's off its default.
  {
    const GuiPrefs& g = c.gui;
    const GuiPrefs dg;
    oj gui = j.contains("gui") && j["gui"].is_object() ? j["gui"] : oj::object();
    auto sub = [](oj& o, const char* key, const oj& v, bool is_default) {
      if (o.contains(key) || !is_default) o[key] = v;
    };
    sub(gui, "theme", g.theme, g.theme == dg.theme);
    sub(gui, "font_size", g.font_size, g.font_size == dg.font_size);
    sub(gui, "code_font", g.code_font, g.code_font == dg.code_font);
    sub(gui, "density", g.density, g.density == dg.density);
    sub(gui, "author_style", g.author_style, g.author_style == dg.author_style);
    sub(gui, "state_style", g.state_style, g.state_style == dg.state_style);
    oj cols = oj::array();
    for (auto& c : g.columns) cols.push_back(c.shown ? oj(c.name) : oj{{"name", c.name}, {"hidden", true}});
    sub(gui, "columns", cols, g.columns == dg.columns);
    oj panes = gui.contains("panes") && gui["panes"].is_object() ? gui["panes"] : oj::object();
    sub(panes, "views", g.views, g.views == dg.views);
    sub(panes, "preview", g.preview, g.preview == dg.preview);
    sub(panes, "results", g.results, g.results == dg.results);
    sub(panes, "run_log", g.run_log, g.run_log == dg.run_log);
    sub(panes, "status_bar", g.status_bar, g.status_bar == dg.status_bar);
    if (!panes.empty()) gui["panes"] = panes;
    if (!gui.empty()) j["gui"] = gui;
  }

  // An entry already in the file under the same name keeps the keys anton
  // doesn't know (a command column's command, above all).
  auto old_entry = [&](const char* list, const std::string& name) {
    if (j.contains(list) && j[list].is_array())
      for (auto& o : j[list])
        if (o.is_object() && o.value("name", "") == name) return o;
    return oj::object();
  };
  if (j.contains("views") || c.views != d.views) {
    oj views = oj::array();
    for (auto& v : c.views) {
      oj o = old_entry("views", v.name);  // a new one gets name, plugin, list, query
      o["name"] = v.name;
      if (v.is_list()) o["plugin"] = v.plugin, o["list"] = v.list;
      else o.erase("plugin"), o.erase("list");
      o["query"] = v.query;
      if (v.sort && *v.sort != (v.is_list() ? Sort::List : Sort::Updated))
        o["sort"] = sort_name(*v.sort);
      else
        o.erase("sort");
      views.push_back(o);
    }
    j["views"] = views;
  }
  if (j.contains("columns") || c.columns != d.columns) {
    oj cols = oj::array();
    for (auto& col : c.columns) {
      if (col.command.empty() && !col.hidden) {
        cols.push_back(col.name);
        continue;
      }
      oj o = old_entry("columns", col.name);
      if (o.empty()) {
        o["name"] = col.name;
        if (!col.command.empty()) o["command"] = col.command, o["width"] = col.width;
      }
      if (col.hidden) o["hidden"] = true;
      else o.erase("hidden");
      cols.push_back(o);
    }
    j["columns"] = cols;
  }
  return j.dump(2) + "\n";
}

// Merges c into config_path(), written to a temp file and renamed over it,
// so a crash never leaves half a file. Returns "" or why it didn't.
inline std::string save_config(const Config& c) {
  namespace fs = std::filesystem;
  std::string path = config_path(), existing;
  if (std::ifstream f(path); f) {
    std::stringstream ss;
    ss << f.rdbuf();
    existing = ss.str();
  }
  std::string err, text = merged_config(existing, c, err);
  if (!err.empty()) return "config.json not saved: " + err;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::string tmp = path + ".tmp" + std::to_string(::getpid());
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << text;
    out.flush();
    if (!out) return "not saved: can't write " + tmp + ": " + std::strerror(errno);
  }
  if (fs::exists(path, ec))  // it may hold tokens in a plugin's env: keep its mode
    fs::permissions(tmp, fs::status(path, ec).permissions(), ec);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::string e = std::strerror(errno);
    std::remove(tmp.c_str());
    return "not saved: can't replace " + path + ": " + e;
  }
  return "";
}

}  // namespace anton
