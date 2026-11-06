#include "proc.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

extern char** environ;

namespace anton {

// Close-on-exec from birth, so a spawn on another thread can't inherit our
// pipe and hold its EOF hostage. macOS has no pipe2; there,
// POSIX_SPAWN_CLOEXEC_DEFAULT (below) gives the same guarantee for our spawns.
static int make_pipe(int fds[2]) {
#ifdef __linux__
  return pipe2(fds, O_CLOEXEC);
#else
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return 0;
#endif
}

Spawned spawn(const std::vector<std::string>& argv,
              const std::map<std::string, std::string>& extra_env, const std::string& input,
              bool new_group) {
  Spawned s;
  int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
  if (make_pipe(out) != 0 || make_pipe(err) != 0 || (!input.empty() && make_pipe(in) != 0)) {
    s.error = std::strerror(errno);
    for (int fd : {out[0], out[1], err[0], err[1], in[0], in[1]})
      if (fd != -1) close(fd);
    return s;
  }

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (input.empty())
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  else
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_adddup2(&fa, err[1], 2);
  for (int fd : {out[0], out[1], err[0], err[1], in[0], in[1]})
    if (fd > 2) posix_spawn_file_actions_addclose(&fa, fd);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short flags = POSIX_SPAWN_SETSIGDEF | (new_group ? POSIX_SPAWN_SETPGROUP : 0);
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // only 0/1/2 from the file actions survive
#endif
  posix_spawnattr_setflags(&attr, flags);
  posix_spawnattr_setpgroup(&attr, 0);
  sigset_t def;
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);
  posix_spawnattr_setsigdefault(&attr, &def);

  std::vector<std::string> env_store;
  for (char** e = environ; *e; ++e) {
    std::string kv = *e;
    if (!extra_env.count(kv.substr(0, kv.find('=')))) env_store.push_back(kv);
  }
  for (auto& [k, v] : extra_env) env_store.push_back(k + "=" + v);
  std::vector<char*> envp, args;
  for (auto& kv : env_store) envp.push_back(kv.data());
  envp.push_back(nullptr);
  std::vector<std::string> argv_copy = argv;
  for (auto& a : argv_copy) args.push_back(a.data());
  args.push_back(nullptr);

  int rc = posix_spawnp(&s.pid, args[0], &fa, &attr, args.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(out[1]);
  close(err[1]);
  if (in[0] != -1) close(in[0]);

  if (rc != 0) {
    s.pid = -1;
    s.error = argv.empty() ? "empty command" : argv[0] + ": " + std::strerror(rc);
    close(out[0]);
    close(err[0]);
    if (in[1] != -1) close(in[1]);
    return s;
  }
  s.out = out[0];
  s.err = err[0];
  if (in[1] != -1) {
    // Small inputs (a URL for pbcopy) fit the pipe buffer; a writer thread
    // keeps a large one from deadlocking against the reader.
    std::thread([fd = in[1], input] {
      size_t off = 0;
      while (off < input.size()) {
        ssize_t n = write(fd, input.data() + off, input.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
      }
      close(fd);
    }).detach();
  }
  return s;
}

int stream(Spawned& s, const std::function<void(const std::string&)>& on_out,
           const std::function<void(const std::string&)>& on_err, size_t max_line) {
  if (s.pid == -1) return -1;
  std::string buf[2];
  pollfd fds[2] = {{s.out, POLLIN, 0}, {s.err, POLLIN, 0}};
  int open_fds = 2;
  char chunk[65536];
  while (open_fds > 0) {
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; ++i) {
      if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      ssize_t n = read(fds[i].fd, chunk, sizeof chunk);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) continue;
        close(fds[i].fd);
        fds[i].fd = -1;
        --open_fds;
        if (!buf[i].empty()) (i == 0 ? on_out : on_err)(buf[i]);
        buf[i].clear();
        continue;
      }
      buf[i].append(chunk, static_cast<size_t>(n));
      size_t start = 0, nl;
      while ((nl = buf[i].find('\n', start)) != std::string::npos) {
        (i == 0 ? on_out : on_err)(buf[i].substr(start, nl - start));
        start = nl + 1;
      }
      buf[i].erase(0, start);
      if (buf[i].size() > max_line) buf[i].clear();  // drop a runaway unterminated line
    }
  }
  s.out = s.err = -1;
  int status = 0;
  while (waitpid(s.pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

Result run(const std::vector<std::string>& argv, const std::string& input, bool new_group) {
  Result r;
  Spawned s = spawn(argv, {}, input, new_group);
  if (s.pid == -1) {
    r.err = s.error;
    return r;
  }
  r.code = stream(
      s, [&](const std::string& l) { r.out += l + "\n"; },
      [&](const std::string& l) { r.err += l + "\n"; });
  return r;
}

void kill_group(pid_t pid) {
  if (pid > 0) ::kill(-pid, SIGTERM);
}

}  // namespace anton
