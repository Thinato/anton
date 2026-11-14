#pragma once
// Markdown -> display lines, glow-style, for the reader (a PR's description
// and its markdown files) and the note overlay. Pure: text in, styled lines
// out, no terminal and no FTXUI elements; ui.cpp turns the lines into
// elements. Widths are FTXUI's string_width, the same ui.cpp lays out with,
// so a line fits what it draws. Parsing is md4c's (GitHub dialect: tables,
// task lists, strikethrough, autolinks); tests/markdown_check.cpp covers it
// through plain().

#include <string>
#include <vector>

#include "model.hpp"

namespace anton::md {

// Ok, Error and Warn color a mark (green, red, yellow); Output is a temoc
// table's expected-value column.
enum Style : unsigned {
  Bold = 1, Italic = 2, Code = 4, Strike = 8, Link = 16, Dim = 32,
  Ok = 64, Error = 128, Warn = 256, Output = 512
};

struct Span {
  std::string text;
  unsigned style = 0;  // Style bits
};

// What a line is, for laying marks onto it: a temoc directive's badge, a
// line of a table (the bottom border ends it), or anything else.
enum class Part { Text, Badge, Table, TableEnd };

struct Line {
  std::vector<Span> spans;
  // 1-based line of the markdown this came from, 0 for none (table borders,
  // blank lines). Every display line of a table row carries the row's line;
  // a wrapped paragraph or list item carries its first line; a badge, its
  // directive's. Marks (temoc results, a plugin's file:line) attach to rows
  // through it.
  int src = 0;
  int heading = 0;  // 1-6 on a heading's lines
  Part part = Part::Text;
};

// A temoc directive: a link reference definition md4c renders as nothing,
// `[//]: # "decisionTable Fixture#method -- snooze until 2026-12-01"` or
// `[//]: # (import Some.Path)`, at most three spaces in, outside fenced code.
struct Directive {
  int line = 0;         // 1-based
  bool import = false;  // `import X`; otherwise `decisionTable X`
  std::string target;   // the fixture (with any #method), or the import path
  std::string snooze;   // "2026-12-01", "" when not snoozed
};

std::vector<Directive> directives(const std::string& markdown);

// temoc snoozes through the date (UTC), so it's expired the day after.
// today is "YYYY-MM-DD"; "" counts every snooze as active.
inline bool expired(const Directive& d, const std::string& today) {
  return !d.snooze.empty() && !today.empty() && today > d.snooze;
}

// Lines at most `width` cells wide (wider only where a single glyph can't
// fit). Text goes through printable() first: nothing from the markdown can
// reach the terminal as a control character. Directives show as a badge
// line before their table (`🧪 Fixture · snoozed until …`, `import X` dim),
// and a table after a decisionTable directive styles its output columns
// (`sum?`) and dims its `# comment` ones.
std::vector<Line> render(const std::string& markdown, int width, const std::string& today = "");

// Marks on one file, from every plugin, laid onto its rendered lines (which
// were rendered kGutter cells narrower than width). Every line gets a
// gutter holding the glyph of the worst mark on its source line, on the
// first display line of it. A mark's line is the nearest rendered source
// line at or above it. Texts: line 0's (and any above the first rendered
// line) at the top; a badge's after the badge; an error's or a warning's
// on a table row under the table, as `✗ line 13 · text`, and under the line
// elsewhere. ok and dim marks are only their glyph.
constexpr int kGutter = 3;
std::vector<Line> annotate(const std::vector<Line>& lines, const std::vector<Mark>& marks, int width);

// A tab's summary of a file's marks, counted by glyph: ✓ 12, ✗ 2. Only
// rows: marks on line 0 and on directives sum up the rows themselves.
struct Count {
  std::string glyph;
  unsigned style = 0;  // Ok, Error, Warn or Dim
  int n = 0;
};
std::vector<Count> tally(const std::vector<Mark>& marks, const std::vector<Directive>& dirs);

// The line's text without styles, trailing spaces trimmed. For the checks.
std::string plain(const Line& l);

}  // namespace anton::md
