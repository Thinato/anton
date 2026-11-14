#pragma once
// Subprocesses without a shell: posix_spawnp + pipes. Shared by anton (gh,
// plugins, pbcopy), anton-slkx (slkx) and anton-llm (agents).

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
// A non-empty cwd is the child's working directory.
Spawned spawn(const std::vector<std::string>& argv,
              const std::map<std::string, std::string>& extra_env = {},
              const std::string& input = "", bool new_group = true, bool keep_stdin = false,
              const std::string& cwd = "");

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
  bool timed_out = false;
};

// spawn + stream, collecting output. code -1 and err set if it never started.
// A timeout (seconds, with new_group) SIGTERMs the child's group when it's up.
Result run(const std::vector<std::string>& argv, const std::string& input = "",
           bool new_group = true, int timeout = 0);

void kill_group(pid_t pid);

}  // namespace anton
