#pragma once
// Subprocesses without a shell: posix_spawnp + pipes. Shared by anton (gh,
// plugins, pbcopy) and anton-slkx (slkx).

#include <sys/types.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace anton {

struct Spawned {
  pid_t pid = -1;
  int out = -1, err = -1;  // read ends; -1 if spawn failed
  int in = -1;             // write end of stdin, with keep_stdin
  std::string error;       // strerror when pid == -1
};

// Starts argv, stdin from `input` if non-empty else /dev/null, stdout/stderr
// piped. extra_env is added to the inherited environment. With new_group the
// child leads its own process group, so kill_group also reaches whatever it
// spawns (without new_group) itself. With keep_stdin, stdin is a pipe whose
// write end is returned in `in` (the caller closes it); `input` is ignored.
Spawned spawn(const std::vector<std::string>& argv,
              const std::map<std::string, std::string>& extra_env = {},
              const std::string& input = "", bool new_group = true, bool keep_stdin = false);

// Reads both pipes until EOF, calling the callbacks once per line (without
// '\n'), then reaps the child. Returns its exit code (128+signal if killed).
// An unterminated line longer than max_line is discarded rather than buffered
// forever (plugins); run() needs no limit -- slkx prints 1 MiB+ JSON lines.
int stream(Spawned& s, const std::function<void(const std::string&)>& on_out,
           const std::function<void(const std::string&)>& on_err,
           size_t max_line = std::string::npos);

struct Result {
  int code = -1;
  std::string out, err;
};

// spawn + stream, collecting output. code -1 and err set if it never started.
Result run(const std::vector<std::string>& argv, const std::string& input = "",
           bool new_group = true);

void kill_group(pid_t pid);

}  // namespace anton
