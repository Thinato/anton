// anton — a PR review TUI. See specs/.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.hpp"
#include "proc.hpp"
#include "ui.hpp"

static const char* kUsage =
    "usage: anton [--query '<github search>'] [--no-plugins]\n"
    "\n"
    "  --query Q      open a single ad-hoc view instead of the configured ones\n"
    "  --no-plugins   start without plugins\n"
    "\n"
    "config: $XDG_CONFIG_HOME/anton/config.json (default ~/.config/anton/config.json)\n";

int main(int argc, char** argv) {
  // A clipboard tool that exits before reading must not kill anton (and leave
  // the terminal raw). Children get SIGPIPE back to default in spawn().
  std::signal(SIGPIPE, SIG_IGN);
  anton::Config cfg;
  if (std::string err = anton::load_config(cfg); !err.empty()) {
    std::fprintf(stderr, "anton: %s\n", err.c_str());
    return 2;
  }

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (a == "--no-plugins") {
      cfg.plugins.clear();
    } else if (a == "--query" && i + 1 < argc) {
      cfg.views = {{"Query", argv[++i], "", ""}};
    } else if (a.rfind("--query=", 0) == 0) {
      cfg.views = {{"Query", a.substr(8), "", ""}};
    } else {
      std::fprintf(stderr, "anton: unknown argument %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }

  // Fail before the alternate screen, where the message would be lost.
  anton::Result auth = anton::run({"gh", "auth", "status"});
  if (auth.code != 0) {
    std::fprintf(stderr, "anton: gh is not ready (gh auth status failed)\n%s", auth.err.c_str());
    return 1;
  }

  anton::run_ui(std::move(cfg));
}
