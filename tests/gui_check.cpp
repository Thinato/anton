// Assert-based self-check for anton-gui's pure parts: src/gui_logic.hpp,
// the palette's fuzzy match (filters.hpp), the "gui" keys of config.json
// (config.hpp, config_write.hpp) and where the layout lives.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "config_write.hpp"
#include "filters.hpp"
#include "gui_logic.hpp"

using namespace anton;
using namespace anton::gui;

int main() {
  // colors: hex and rgba(), as the canvas writes them
  assert(parse_color("#0d1117") == 0x0d1117ffu);
  assert(parse_color("rgba(56,139,253,0.15)") == 0x388bfd26u);
  assert(parse_color("rgba(1, 4, 9, 0.6)") == 0x01040999u);
  assert(parse_color("red") == 0 && parse_color("#12345") == 0);

  // the themes: the canvas's exact tokens
  {
    Theme d = make_theme(true), l = make_theme(false);
    assert(d.dark && !l.dark);
    assert(d.bg == 0x0d1117ffu && d.panel == 0x151b23ffu && d.inset == 0x010409ffu && d.border == 0x3d444dffu);
    assert(d.fg == 0xf0f6fcffu && d.muted == 0x9198a1ffu && d.accent == 0x4493f8ffu);
    assert(d.tab_line == 0xf78166ffu && l.tab_line == 0xfd8c73ffu);
    assert(d.success == 0x3fb950ffu && d.danger == 0xf85149ffu && d.attention == 0xd29922ffu && d.done == 0xab7df8ffu);
    assert(d.primary == 0x238636ffu && l.primary == 0x1f883dffu);
    assert(l.bg == 0xffffffffu && l.panel == 0xf6f8faffu && l.border == 0xd1d9e0ffu && l.fg == 0x1f2328ffu);
    assert(l.accent_muted == 0xddf4ffffu && l.danger_muted == 0xffebe9ffu && l.success_soft == 0xf1fbf3ffu);
    assert(d.success_soft == parse_color("rgba(46,160,67,0.07)"));
  }
  assert(resolve_dark("dark", false) && !resolve_dark("light", true));
  assert(resolve_dark("system", true) && !resolve_dark("system", false));

  // avatars: dark letters on light colors
  assert(dark_text_on(0xa2a5d8) && dark_text_on(0xc98652) && !dark_text_on(0x75444c) && !dark_text_on(0x644c6a));

  // the diffstat: GitHub's five blocks
  assert((diffstat(0, 0) == std::array<int, 5>{0, 0, 0, 0, 0}));
  assert((diffstat(385, 0) == std::array<int, 5>{1, 1, 1, 1, 0}));
  assert((diffstat(0, 4) == std::array<int, 5>{2, 0, 0, 0, 0}));
  assert((diffstat(433, 106) == std::array<int, 5>{1, 1, 1, 1, 2}));
  assert((diffstat(12, 1) == std::array<int, 5>{1, 1, 0, 0, 0}));

  // the state pill: derive_state's pick, GitHub's words and tones
  {
    Pr p;
    assert(state_pill(derive_state(p, std::nullopt)).text == "Review required");
    p.review_decision = "APPROVED";
    assert(state_pill(derive_state(p, std::nullopt)).tone == Tone::Success);
    assert(state_pill(derive_state(p, Status{"⚙", "queued"})).text == "⚙ queued");
    p.mergeable = "CONFLICTING";
    assert(state_pill(derive_state(p, Status{"⚙", "queued"})).text == "Conflicts");
    p.state = "MERGED";
    assert(state_pill(derive_state(p, std::nullopt)).tone == Tone::Done);
  }
  assert(review_pill("CHANGES_REQUESTED").tone == Tone::Danger && review_pill("APPROVED").text == "Approved");

  // emoji the fonts can't draw go; symbols stay
  assert(strip_emoji("👀 paz") == "paz" && strip_emoji("⚙ queued") == "⚙ queued" && strip_emoji("✓ 14") == "✓ 14");
  assert(strip_emoji("❤️ x") == "❤ x" && strip_emoji("🧪") == "" && strip_emoji("toggle 👀 reviewing") == "toggle reviewing");
  assert(row_height("compact") < row_height("comfortable"));

  // results off marks: glyph first, level otherwise
  assert(result_of({"f", 3, "error", "!", ""}) == Result::Error);
  assert(result_of({"f", 3, "dim", "◦", ""}) == Result::Queued);
  assert(result_of({"f", 3, "warn", "", ""}) == Result::Snoozed);
  assert(result_of({"f", 3, "dim", "-", ""}) == Result::Skipped);
  assert(short_result("failed · 1 of 4\nmake got exception") == "failed");
  assert(unclocked("setting up · 0:23") == "setting up");
  assert(unclocked("running Steps · 3 of 8 · 1:02") == "running Steps · 3 of 8");
  assert(unclocked("failed · 1 of 4") == "failed · 1 of 4");
  {
    std::vector<Mark> ms = {{"f", 0, "ok", "✓", ""}, {"f", 3, "error", "✗", ""}, {"f", 3, "warn", "~", ""}};
    assert(worst(ms)->glyph == "✗" && worst({}) == nullptr);
  }

  // marks land on the nearest block line at or above them
  {
    std::string t = "# Calc\n\nSome text\nmore text\n\n[//]: # \"decisionTable Calc\"\n\n| a | b? |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n";
    auto blocks = md::blocks(t);
    std::vector<Mark> ms = {{"f", 0, "error", "✗", "failed · 1 of 2"}, {"f", 4, "warn", "~", "on the paragraph"},
                            {"f", 6, "error", "✗", "failed · 1 of 2"}, {"f", 10, "ok", "✓", ""},
                            {"f", 11, "error", "✗", "b expected 4, got 5"}, {"f", 99, "dim", "·", "past the end"}};
    auto at = attach(blocks, ms);
    assert(at[0].size() == 1 && at[3].size() == 1 && at[3][0].text == "on the paragraph");
    assert(at[6].size() == 1 && at[10].size() == 1 && at[11].size() == 2);
    size_t table = 0;
    for (size_t i = 0; i < blocks.size(); ++i)
      if (blocks[i].kind == md::Block::Table) table = i;
    assert(table_name(blocks, table) == "Calc");
    auto bare = md::blocks("[//]: # \"decisionTable StepsFixture\"\n\n| a |\n|---|\n| 1 |\n");
    assert(table_name(bare, 1) == "StepsFixture");
  }

  // the run log: what changed, without the clock's ticks or queued rows
  {
    PrInfo a;
    a.marks_command = "temoc -- specs/a.md";
    a.marks_summary = Mark{"", 0, "dim", "⋯", "setting up · 0:01"};
    a.marks = {{"specs/a.md", 0, "dim", "⋯", "setting up · 0:01"}, {"specs/a.md", 9, "dim", "◦", ""}};
    auto first = log_diff(nullptr, a);
    assert(first.size() == 3 && first[0].event == "command" && first[1].event == "summary" && first[2].event == "file");
    assert(first[1].text == "⋯ setting up" && first[2].text == "specs/a.md  ⋯ setting up");
    PrInfo b = a;
    b.marks_summary->text = "setting up · 0:02";
    b.marks[0].text = "setting up · 0:02";
    assert(log_diff(&a, b).empty());  // a tick
    PrInfo c = b;
    c.marks[1] = {"specs/a.md", 9, "error", "✗", "sum expected 5, got 4"};
    auto row = log_diff(&b, c);
    assert(row.size() == 1 && row[0].event == "result" && row[0].text == "a.md:9  ✗ sum expected 5, got 4");
    assert(row[0].level == "error");
  }

  // the palettes' fuzzy match: whole words highlight as a run
  {
    auto m = fuzzy_find("Run temoc specs", "temoc");
    assert(m && (*m == std::vector<size_t>{4, 5, 6, 7, 8}));
    auto s = fuzzy_find("brick-so/core", "bcore");
    assert(s && (*s == std::vector<size_t>{0, 3, 7, 11, 12}));
    assert(!fuzzy_find("core", "x") && fuzzy_find("Core", "")->empty());
    assert(fuzzy_find("Open in READER", "reader"));
  }

  // the "gui" keys: parsed, checked, written back only off their defaults
  {
    Config c;
    assert(parse_config(R"({"gui": {"theme": "light", "font_size": 40, "density": "compact",
                                    "panes": {"preview": false}}})", c).empty());
    assert(c.gui.theme == "light" && c.gui.font_size == 18 && c.gui.density == "compact" && !c.gui.preview && c.gui.views);
    // a bad gui value doesn't stop the TUI: it's reported, and the gui keys default
    Config bad;
    assert(parse_config(R"({"gui": {"theme": "blue", "density": "compact"}, "refresh_seconds": 60})", bad).empty());
    assert(bad.gui_error == "gui.theme: dark, light or system" && bad.gui == GuiPrefs{} && bad.refresh_seconds == 60);
    Config bad2;
    assert(parse_config(R"({"gui": {"font_size": "14"}})", bad2).empty() && bad2.gui_error.starts_with("gui: "));
    Config bad3;
    assert(parse_config(R"({"gui": {"code_font": "Comic"}})", bad3).empty() && !bad3.gui_error.empty());

    std::string err;
    Config d;
    assert(merged_config("", d, err) == "{}\n");  // defaults add nothing
    d.gui.theme = "dark";
    d.gui.run_log = false;
    std::string out = merged_config("{\"plugins\": [], \"gui\": {\"mine\": 1}}", d, err);
    assert(err.empty());
    assert(out == "{\n  \"plugins\": [],\n  \"gui\": {\n    \"mine\": 1,\n    \"theme\": \"dark\",\n"
                  "    \"panes\": {\n      \"run_log\": false\n    }\n  }\n}\n");
    Config back;
    assert(parse_config(out, back).empty() && back.gui == d.gui);  // the round trip
    d.gui.theme = "system";  // back to the default: kept, since the file has it now
    assert(merged_config(out, d, err).find("\"theme\": \"system\"") != std::string::npos);
  }

  // the layout lives in the config dir, never the cwd
  setenv("XDG_CONFIG_HOME", "/tmp/xdg-check", 1);
  assert(gui_layout_path() == "/tmp/xdg-check/anton/gui-layout.ini");
  assert(config_path() == "/tmp/xdg-check/anton/config.json");
  unsetenv("XDG_CONFIG_HOME");
  setenv("HOME", "/home/someone", 1);
  assert(gui_layout_path() == "/home/someone/.config/anton/gui-layout.ini");

  std::puts("gui_check: ok");
}
