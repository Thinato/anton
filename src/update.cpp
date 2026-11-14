#include "update.hpp"

#include <filesystem>

#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "proc.hpp"

namespace anton {

namespace fs = std::filesystem;

static const char* kRepo = "Thinato/anton";

// The release asset built for this binary's platform (release.yml).
static const char* target() {
#if defined(__APPLE__) && defined(__aarch64__)
  return "macos-arm64";
#elif defined(__linux__) && defined(__x86_64__)
  return "linux-x86_64";
#else
  return nullptr;
#endif
}

static fs::path exe_path() {
  std::error_code ec;
#ifdef __APPLE__
  char buf[4096];
  uint32_t n = sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return {};
  fs::path p = fs::canonical(buf, ec);
#else
  fs::path p = fs::canonical("/proc/self/exe", ec);
#endif
  return ec ? fs::path() : p;
}

std::string self_update(bool install) {
  if (!parse_version(kVersion) || !target()) return "";
  Result r = run({"gh", "api", std::string("repos/") + kRepo + "/releases/latest", "--jq", ".tag_name"});
  std::string tag = r.out.substr(0, r.out.find_first_of("\r\n"));
  if (r.code != 0 || !newer(tag, kVersion)) return "";
  if (!install) return "anton " + tag + " is out";

  fs::path exe = exe_path(), dir = exe.parent_path();
  if (exe.empty() || access(dir.c_str(), W_OK) != 0)
    return "anton " + tag + " is out (can't write where anton is installed)";
  // Unpacked in the same directory, so the rename below is atomic.
  // ponytail: quitting mid-download leaves this dir behind; it's tiny.
  std::string tmpl = (dir / ".anton-update-XXXXXX").string();
  if (!mkdtemp(tmpl.data())) return "anton " + tag + " is out (can't write where anton is installed)";
  fs::path tmp = tmpl;
  std::string tarball = std::string("anton-") + target() + ".tar.gz", err;
  if (run({"gh", "release", "download", tag, "--repo", kRepo, "--pattern", tarball, "--dir", tmp.string()})
          .code != 0)
    err = "download failed";
  else if (run({"tar", "-xzf", (tmp / tarball).string(), "-C", tmp.string()}).code != 0)
    err = "unpack failed";
  else
    // anton under whatever name it runs as; the plugins only where they
    // already are, so a dir that never had one doesn't grow it.
    for (auto [from, to] : {std::pair{tmp / "anton", exe}, std::pair{tmp / "anton-slkx", dir / "anton-slkx"},
                            std::pair{tmp / "anton-llm", dir / "anton-llm"},
                            std::pair{tmp / "anton-temoc", dir / "anton-temoc"}}) {
      if (!fs::exists(from) || (to != exe && !fs::exists(to))) continue;
      std::error_code ec;
      fs::rename(from, to, ec);
      if (ec) {
        err = "can't replace " + to.string() + ": " + ec.message();
        break;
      }
    }
  std::error_code ec;
  fs::remove_all(tmp, ec);
  return err.empty() ? "updated to " + tag + ", restart anton to use it" : "update to " + tag + " failed: " + err;
}

}  // namespace anton
