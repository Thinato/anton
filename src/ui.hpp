#pragma once

#include "config.hpp"

namespace anton {

// Runs the TUI until the user quits. Never returns normally: it stops the
// plugins and _Exits, since detached worker threads may still be blocked on
// a child process.
[[noreturn]] void run_ui(Config cfg);

}  // namespace anton
