// Assert-based self-check for src/markdown.hpp, through plain text.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include <ftxui/screen/string.hpp>

#include "markdown.hpp"

using namespace anton;

static std::vector<std::string> lines(const std::string& md, int w) {
  std::vector<std::string> out;
  for (auto& l : md::render(md, w)) out.push_back(md::plain(l));
  return out;
}

static std::string joined(const std::string& md, int w) {
  std::string s;
  for (auto& l : lines(md, w)) s += l + "\n";
  return s;
}

static void dump(const std::string& md, int w) { std::fputs(joined(md, w).c_str(), stderr); }

// Every line fits: the renderer's promise to ui.cpp.
static void fits(const std::string& md, int w) {
  for (auto& l : lines(md, w)) assert(ftxui::string_width(l) <= w);
}

int main() {
  // paragraphs wrap by display width; blocks are separated by a blank line
  assert(joined("one two three four five", 10) == "one two\nthree four\nfive\n");
  assert(joined("a\nb\n\nc", 20) == "a b\n\nc\n");
  assert(joined("**bold** and _it_ ~~gone~~ `code`", 40) == "bold and it gone code\n");
  {
    auto l = md::render("**bold** `code`", 40)[0];
    assert(l.spans[0].text == "bold" && l.spans[0].style == md::Bold);
    assert(l.spans.back().text == "code" && l.spans.back().style == md::Code);
  }
  // CJK and emoji are two cells wide: 4 of them fill 8 columns
  assert(joined("日本語の 文章です", 8) == "日本語の\n文章です\n");
  assert(joined("🎉🎉 🎉🎉 🎉🎉", 9) == "🎉🎉 🎉🎉\n🎉🎉\n");
  // a word wider than the line is cut across lines, never past the edge
  assert(joined("abcdefghijklmnop", 10) == "abcdefghij\nklmnop\n");
  fits("日本語日本語日本語日本語日本語日本語", 11);

  // headings keep their #s; levels are on the line
  {
    auto out = md::render("# Title\n\n## Sub\n\ntext", 40);
    assert(md::plain(out[0]) == "# Title" && out[0].heading == 1);
    assert(md::plain(out[2]) == "## Sub" && out[2].heading == 2);
    assert(out[4].heading == 0);
  }

  // lists: bullets, numbers, nesting, hanging indent, task boxes
  assert(joined("- a\n- b\n  - c\n- d", 40) == "• a\n• b\n  ◦ c\n• d\n");
  assert(joined("3. x\n4. y", 40) == "3. x\n4. y\n");
  assert(joined("- one two three four", 12) == "• one two\n  three four\n");
  assert(joined("- [ ] todo\n- [x] done", 40) == "☐ todo\n☑ done\n");
  assert(joined("- a\n\n- b", 40) == "• a\n\n• b\n");  // loose list keeps its gaps

  // block quotes get a bar on every line, wrapped ones too
  assert(joined("> quoted text here", 10) == "│ quoted\n│ text\n│ here\n");

  // code blocks: as written, indented, never wrapped; a long line is cut
  assert(joined("```\nint x = 1;\n  y();\n```", 40) == "  int x = 1;\n    y();\n");
  assert(joined("```\n0123456789abcdef\n```", 10) == "  0123456…\n");
  assert(joined("    indented", 40) == "  indented\n");

  // links: text, then the URL unless the text is the URL; images by alt
  assert(joined("[docs](https://x.io/d)", 60) == "docs https://x.io/d\n");
  assert(joined("<https://x.io>", 60) == "https://x.io\n");
  assert(joined("see https://x.io/a now", 60) == "see https://x.io/a now\n");
  assert(joined("![a cat](cat.png)", 60) == "[image: a cat]\n");
  assert(joined("---", 10) == "──────────\n");

  // HTML: comments vanish, <details>/<summary> is a ▸ line, <br> breaks,
  // <img alt> is an image, other tags go and keep their text
  assert(joined("<!-- fill this in -->\n\ntext", 40) == "text\n");
  assert(joined("a <!-- hidden --> b", 40) == "a b\n");
  assert(joined("<details>\n<summary>More</summary>\n\nhidden\n\n</details>", 40) == "▸ More\n\nhidden\n");
  assert(joined("one<br>two", 40) == "one\ntwo\n");
  assert(joined("<p align=\"center\"><img src=\"x.png\" alt=\"logo\"></p>", 40) == "[image: logo]\n");
  assert(joined("<b>bold</b> &amp; &#x41;", 40) == "bold & A\n");

  // no control characters reach the terminal, even via entities
  for (auto& l : lines("a\x1b[31mred\x07 &#27; \x7f\r\nb", 40))
    for (unsigned char c : l) assert(c >= 0x20 && c != 0x7f);

  // tables: box borders, header separator, alignment
  {
    std::string t =
        "intro\n"
        "\n"
        "| name | n |\n"
        "|:-----|--:|\n"
        "| a    | 1 |\n"
        "| bb   | 22 |\n";
    auto out = md::render(t, 40);
    std::vector<std::string> want = {
        "intro",
        "",
        "┌──────┬────┐",
        "│ name │  n │",
        "├──────┼────┤",
        "│ a    │  1 │",
        "│ bb   │ 22 │",
        "└──────┴────┘",
    };
    assert(out.size() == want.size());
    for (size_t i = 0; i < want.size(); ++i) assert(md::plain(out[i]) == want[i]);
    // each row knows its source line; borders have none
    assert(out[2].src == 0 && out[3].src == 3 && out[5].src == 5 && out[6].src == 6 && out[7].src == 0);
    assert(out[0].src == 1);
  }
  assert(joined("| a | b |\n|:-:|---|\n| x | y |", 40) ==
         "┌───┬───┐\n│ a │ b │\n├───┼───┤\n│ x │ y │\n└───┴───┘\n");
  {
    // too wide: the widest column shrinks and its cells wrap inside it
    std::string t =
        "| id | description |\n"
        "|----|-------------|\n"
        "| 1  | a long description that will not fit |\n"
        "| 2  | short |\n";
    fits(t, 30);
    auto out = md::render(t, 30);
    assert(md::plain(out[0]) == "┌────┬───────────────────────┐");
    assert(md::plain(out[3]) == "│ 1  │ a long description    │");
    assert(md::plain(out[4]) == "│    │ that will not fit     │");
    assert(out[3].src == 3 && out[4].src == 3);
    assert(md::plain(out[5]) == "├────┼───────────────────────┤");  // wrapped rows get separators
    assert(out[6].src == 4);
  }
  // a table with CJK cells lines up by cells, not bytes
  assert(joined("| 名前 | x |\n|---|---|\n| 太郎 | y |", 40) ==
         "┌──────┬───┐\n│ 名前 │ x │\n├──────┼───┤\n│ 太郎 │ y │\n└──────┴───┘\n");
  // temoc's hidden directive above a table renders nothing; the rows keep
  // their source lines
  {
    auto out = md::render("[//]: # \"decisionTable Calc\"\n\n| a | b |\n|---|---|\n| 1 | 2 |", 40);
    assert(out.size() == 5 && md::plain(out[0]).rfind("┌", 0) == 0);
    assert(out[1].src == 3 && out[3].src == 5);
  }

  // paragraphs and list items know where they start
  {
    auto out = md::render("first\n\n- item\n- two", 40);
    assert(out[0].src == 1 && out[2].src == 3 && out[3].src == 4);
  }
  assert(md::render("", 40).empty());
  assert(md::render("<!-- only a template comment -->", 40).empty());
  (void)dump;
  std::puts("markdown_check: ok");
}
