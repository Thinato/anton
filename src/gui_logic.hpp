#pragma once
// anton-gui's pure parts (06-gui.md): the GitHub theme tokens, what a PR's
// state and size look like, temoc results read off marks, the run log's
// events, where marks land on markdown blocks. No ImGui here, so
// tests/gui_check.cpp covers it.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "markdown.hpp"
#include "model.hpp"

namespace anton::gui {

// -- colors ----------------------------------------------------------------------

// "#rrggbb" or "rgba(r, g, b, a)" -> 0xRRGGBBAA; 0 when it's neither.
inline uint32_t parse_color(const std::string& s) {
  unsigned r, g, b;
  float a = 1;
  if (s.size() == 7 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3)
    return r << 24 | g << 16 | b << 8 | 0xffu;
  if (std::sscanf(s.c_str(), "rgba(%u,%u,%u,%f)", &r, &g, &b, &a) == 4 ||
      std::sscanf(s.c_str(), "rgba(%u, %u, %u, %f)", &r, &g, &b, &a) == 4)
    return (r & 0xff) << 24 | (g & 0xff) << 16 | (b & 0xff) << 8 | (static_cast<uint32_t>(std::lround(a * 255)) & 0xff);
  return 0;
}

// The design's tokens (the THEMES objects of the approved canvas): GitHub's
// dark and light defaults. 0xRRGGBBAA.
struct Theme {
  bool dark;
  uint32_t bg, panel, inset, border, border_muted, fg, muted, accent, accent_muted, tab_line, sel, sel_mark,
      success, success_muted, success_soft, danger, danger_muted, attention, attention_muted, done, done_muted,
      neutral, neutral_muted, btn, input_bg, primary, primary_hover, primary_border, empty, scrim, shadow,
      palette_bg, palette_foot, palette_sel, fg2;  // fg2: the canvas's secondary text, a step under fg
};

inline Theme make_theme(bool dark) {
  auto c = parse_color;
  if (dark)
    return {true, c("#0d1117"), c("#151b23"), c("#010409"), c("#3d444d"), c("#262c36"), c("#f0f6fc"),
            c("#9198a1"), c("#4493f8"), c("rgba(56,139,253,0.15)"), c("#f78166"), c("rgba(56,139,253,0.12)"),
            c("#1f6feb"), c("#3fb950"), c("rgba(46,160,67,0.15)"), c("rgba(46,160,67,0.07)"), c("#f85149"),
            c("rgba(248,81,73,0.12)"), c("#d29922"), c("rgba(187,128,9,0.15)"), c("#ab7df8"),
            c("rgba(171,125,248,0.15)"), c("#9198a1"), c("rgba(101,108,118,0.25)"), c("#212830"), c("#0d1117"),
            c("#238636"), c("#29903b"), c("rgba(240,246,252,0.1)"), c("#2a313c"), c("rgba(1,4,9,0.6)"),
            c("rgba(1,4,9,0.85)"), c("#151b23"), c("#0d1117"), c("rgba(56,139,253,0.15)"), c("#d1d7e0")};
  return {false, c("#ffffff"), c("#f6f8fa"), c("#eff2f5"), c("#d1d9e0"), c("#e6eaef"), c("#1f2328"),
          c("#59636e"), c("#0969da"), c("#ddf4ff"), c("#fd8c73"), c("#ddf4ff"), c("#0969da"), c("#1a7f37"),
          c("#dafbe1"), c("#f1fbf3"), c("#d1242f"), c("#ffebe9"), c("#9a6700"), c("#fff8c5"), c("#8250df"),
          c("#fbefff"), c("#59636e"), c("#e6eaef"), c("#f6f8fa"), c("#ffffff"), c("#1f883d"), c("#1c8139"),
          c("rgba(31,35,40,0.15)"), c("#d1d9e0"), c("rgba(31,35,40,0.35)"), c("rgba(31,35,40,0.25)"),
          c("#ffffff"), c("#f6f8fa"), c("#ddf4ff"), c("#25292e")};
}

// "dark", "light" or "system" (the OS appearance, os_dark) -> dark?
inline bool resolve_dark(const std::string& pref, bool os_dark) {
  return pref == "dark" || (pref != "light" && os_dark);
}

// An avatar's letter on the person's color: dark text on light colors
// (WCAG relative luminance over 0.179), white on dark ones.
inline bool dark_text_on(int rgb) {
  auto lin = [](int v) {
    double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  double l = 0.2126 * lin(rgb >> 16 & 0xff) + 0.7152 * lin(rgb >> 8 & 0xff) + 0.0722 * lin(rgb & 0xff);
  return l > 0.179;
}

// -- the PR list -----------------------------------------------------------------

enum class Tone { Accent, Success, Danger, Attention, Done, Neutral };

// GitHub's five-block diffstat: how many blocks are filled grows with the
// change's size, split green/red by additions and deletions.
// 0 empty, 1 green, 2 red.
inline std::array<int, 5> diffstat(long add, long del) {
  long n = std::max(0L, add) + std::max(0L, del);
  int filled = n >= 500 ? 5 : n >= 200 ? 4 : n >= 50 ? 3 : n >= 10 ? 2 : n > 0 ? 1 : 0;
  int green = n ? static_cast<int>(std::lround(static_cast<double>(filled) * std::max(0L, add) / n)) : 0;
  std::array<int, 5> out{};
  for (int i = 0; i < 5; ++i) out[static_cast<size_t>(i)] = i < green ? 1 : i < filled ? 2 : 0;
  return out;
}

struct Pill {
  std::string text;
  Tone tone;
};

// The STATE column as a pill: derive_state's pick, in GitHub's words.
inline Pill state_pill(const Shown& s) {
  switch (s.kind) {
    case Kind::Merged: return {"Merged", Tone::Done};
    case Kind::Done: return {"Closed", Tone::Neutral};
    case Kind::Conflict: return {"Conflicts", Tone::Danger};
    case Kind::Custom: return {s.text, Tone::Accent};
    case Kind::Draft: return {"Draft", Tone::Neutral};
    case Kind::Failing: return {"Checks failing", Tone::Danger};
    case Kind::Changes: return {"Changes requested", Tone::Danger};
    case Kind::Approved: return {"Approved", Tone::Success};
    case Kind::Mine: return {s.text == "you ✓" ? "You approved" : "You asked for changes", Tone::Success};
    case Kind::Review: return {"Review required", Tone::Attention};
  }
  return {s.text, Tone::Neutral};
}

// A reviewer's latest review, as the preview lists it.
inline Pill review_pill(const std::string& state) {
  if (state == "APPROVED") return {"Approved", Tone::Success};
  if (state == "CHANGES_REQUESTED") return {"Changes requested", Tone::Danger};
  if (state == "COMMENTED") return {"Commented", Tone::Neutral};
  if (state == "DISMISSED") return {"Dismissed", Tone::Neutral};
  return {"Pending", Tone::Attention};
}

// Text from plugins without the pictographs (U+1F000 up, the emoji
// presentation selector, joiners) the bundled fonts can't draw: "👀 paz"
// reads "paz". Symbols like ⚙ and ✓ stay.
inline std::string strip_emoji(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    std::string ch = s.substr(i, n);
    bool drop = n == 4 || ch == "\xEF\xB8\x8F" /* U+FE0F */ || ch == "\xE2\x80\x8D" /* U+200D */;
    if (!drop && !(ch == " " && !out.empty() && out.back() == ' ')) out += ch;  // "toggle 👀 reviewing": one space
    i += n;
  }
  size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
  return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

// The PR list's row height for a row density.
inline float row_height(const std::string& density) { return density == "compact" ? 24.f : density == "cozy" ? 28.f : 34.f; }

// -- temoc results, read off marks -------------------------------------------------

enum class Result { Passed, Failed, Error, Snoozed, Skipped, Running, Queued, Partial };

// A mark as one of temoc-ui's results: by its glyph, else by its level.
inline Result result_of(const Mark& m) {
  const std::string& g = m.glyph;
  if (g == "✓") return Result::Passed;
  if (g == "✗") return Result::Failed;
  if (g == "!") return Result::Error;
  if (g == "~") return Result::Snoozed;
  if (g == "-" || g == "–") return Result::Skipped;
  if (g == "⋯") return Result::Running;
  if (g == "◦") return Result::Queued;
  if (g == "·") return Result::Partial;
  if (m.level == "ok") return Result::Passed;
  if (m.level == "error") return Result::Failed;
  if (m.level == "warn") return Result::Snoozed;
  return Result::Skipped;
}

inline const char* result_glyph(Result r) {
  static const char* g[] = {"✓", "✗", "!", "~", "–", "⋯", "◦", "·"};
  return g[static_cast<int>(r)];
}

// How bad a mark is, to pick a line's worst: error, warn, ok, dim.
inline int severity(const Mark& m) {
  return m.level == "error" ? 3 : m.level == "warn" ? 2 : m.level == "ok" ? 1 : 0;
}

inline const Mark* worst(const std::vector<Mark>& ms) {
  const Mark* w = nullptr;
  for (auto& m : ms)
    if (!w || severity(m) > severity(*w)) w = &m;
  return w;
}

inline std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

inline std::string after_first_line(const std::string& s) {
  auto nl = s.find('\n');
  return nl == std::string::npos ? "" : s.substr(nl + 1);
}

// "failed · 1 of 4" -> "failed": the short form the outline shows.
inline std::string short_result(const std::string& text) { return first_line(text).substr(0, first_line(text).find(" · ")); }

// "setting up · 0:23" -> "setting up": a run's line without its clock, so a
// tick isn't news to the run log.
inline std::string unclocked(const std::string& s) {
  const std::string sep = " · ";
  auto dot = s.rfind(sep);
  if (dot == std::string::npos) return s;
  std::string t = s.substr(dot + sep.size());
  auto colon = t.find(':');
  bool clock = colon != std::string::npos && colon > 0 && t.size() - colon == 3 &&
               all_digits(t.substr(0, colon)) && all_digits(t.substr(colon + 1));
  return clock ? s.substr(0, dot) : s;
}

// Every mark (line > 0) under the block line it belongs to: the nearest
// shown source line at or above it (a paragraph's first line, a table row,
// a directive's badge), as the TUI lays them (01-anton.md, Reader). Line 0
// and marks above the first block go under 0.
inline std::map<int, std::vector<Mark>> attach(const std::vector<md::Block>& blocks, const std::vector<Mark>& marks) {
  std::vector<int> srcs;
  for (auto& b : blocks) {
    if (b.kind == md::Block::Table)
      for (auto& r : b.rows) srcs.push_back(r.src);
    else if (b.src > 0)
      srcs.push_back(b.src);
  }
  std::sort(srcs.begin(), srcs.end());
  std::map<int, std::vector<Mark>> out;
  for (auto& m : marks) {
    int at = 0;
    if (m.line > 0) {
      auto it = std::upper_bound(srcs.begin(), srcs.end(), m.line);
      if (it != srcs.begin()) at = *std::prev(it);
    }
    out[at].push_back(m);
  }
  return out;
}

// A test table's name: the nearest heading above it, else its fixture.
inline std::string table_name(const std::vector<md::Block>& blocks, size_t table) {
  std::string fixture;
  for (size_t i = table; i-- > 0;) {
    const md::Block& b = blocks[i];
    if (b.kind == md::Block::Badge && fixture.empty() && b.src == blocks[table].test && !b.spans.empty()) {
      fixture = b.spans[0].text;
      if (fixture.starts_with("🧪 ")) fixture = fixture.substr(5);
    }
    if (b.kind == md::Block::Table && i != table) break;  // a heading over another table isn't this one's
    if (b.kind == md::Block::Heading) {
      std::string h;
      for (auto& s : b.spans) h += s.text;
      return h;
    }
  }
  return fixture;
}

// -- the run log -------------------------------------------------------------------

// One line of the reader's Run log: a plugin's progress as it streams.
struct LogEntry {
  std::time_t at = 0;
  std::string plugin, url, event, text;
  std::string level;  // ok | error | warn | dim, for its color
};

// What changed between two of a plugin's pr lines for one PR, as log
// lines: the run's state (summary), each file's state (line 0), each
// table's and row's result, the command. A clock ticking isn't a change;
// queued rows aren't news until they run.
inline std::vector<LogEntry> log_diff(const PrInfo* before, const PrInfo& after) {
  std::vector<LogEntry> out;
  auto entry = [&](std::string ev, std::string text, std::string level) {
    out.push_back({0, "", "", std::move(ev), std::move(text), std::move(level)});
  };
  if (!before || before->marks_command != after.marks_command)
    if (!after.marks_command.empty()) entry("command", after.marks_command, "dim");
  auto sum = [](const PrInfo* p) { return p && p->marks_summary ? unclocked(first_line(p->marks_summary->text)) : ""; };
  if (after.marks_summary && sum(before) != sum(&after))
    entry("summary", after.marks_summary->glyph + " " + sum(&after), after.marks_summary->level);
  std::map<std::pair<std::string, int>, std::string> old;
  if (before)
    for (auto& m : before->marks) old[{m.file, m.line}] += m.glyph + unclocked(m.text) + '\n';
  std::map<std::pair<std::string, int>, std::string> now_;
  for (auto& m : after.marks) now_[{m.file, m.line}] += m.glyph + unclocked(m.text) + '\n';
  for (auto& m : after.marks) {
    auto key = std::make_pair(m.file, m.line);
    if (old.count(key) && old[key] == now_[key]) continue;
    if (now_[key].empty()) continue;  // logged once per (file, line)
    now_[key].clear();
    Result r = result_of(m);
    if (r == Result::Queued) continue;
    std::string where = m.line == 0 ? m.file : path_tail(m.file, 1) + ":" + std::to_string(m.line);
    std::string text = where + "  " + result_glyph(r);
    if (std::string t = unclocked(first_line(m.text)); !t.empty()) text += " " + t;
    std::string ev = m.line == 0 ? (m.banner ? "file_failed" : "file") : r == Result::Running ? "running" : "result";
    entry(ev, text, m.level);
  }
  return out;
}

}  // namespace anton::gui
