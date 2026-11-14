#pragma once
// Markdown -> display lines, glow-style, for the reader (a PR's description)
// and the note overlay. Pure: text in, styled lines out, no terminal and no
// FTXUI elements; ui.cpp turns the lines into elements. Widths are FTXUI's
// string_width, the same ui.cpp lays out with, so a line fits what it draws.
// Parsing is md4c's (GitHub dialect: tables, task lists, strikethrough,
// autolinks); tests/markdown_check.cpp covers it through plain().

#include <string>
#include <vector>

namespace anton::md {

enum Style : unsigned { Bold = 1, Italic = 2, Code = 4, Strike = 8, Link = 16, Dim = 32 };

struct Span {
  std::string text;
  unsigned style = 0;  // Style bits
};

struct Line {
  std::vector<Span> spans;
  // 1-based line of the markdown this came from, 0 for none (table borders,
  // blank lines). Every display line of a table row carries the row's line;
  // a wrapped paragraph or list item carries its first line. Later marks
  // (temoc results, a plugin's file:line) attach to rows through it.
  int src = 0;
  int heading = 0;  // 1-6 on a heading's lines
};

// Lines at most `width` cells wide (wider only where a single glyph can't
// fit). Text goes through printable() first: nothing from the markdown can
// reach the terminal as a control character.
std::vector<Line> render(const std::string& markdown, int width);

// The line's text without styles, trailing spaces trimmed. For the checks.
std::string plain(const Line& l);

}  // namespace anton::md
