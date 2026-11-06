#pragma once
// Self-update from GitHub releases (specs/01-anton.md, "Updates").

#include <array>
#include <cstdio>
#include <optional>
#include <string>

#ifndef ANTON_VERSION
#define ANTON_VERSION "dev"  // local builds; release builds pass the tag
#endif

namespace anton {

inline constexpr const char* kVersion = ANTON_VERSION;

// "v0.1.12-beta" -> {0, 1, 12}. The suffix doesn't order: N keeps counting
// across a -beta -> stable switch (.github/workflows/release.yml).
inline std::optional<std::array<int, 3>> parse_version(const std::string& s) {
  int a, b, c;
  if (std::sscanf(s.c_str(), "v%d.%d.%d", &a, &b, &c) != 3) return std::nullopt;
  return std::array{a, b, c};
}

inline bool newer(const std::string& latest, const std::string& current) {
  auto l = parse_version(latest), c = parse_version(current);
  return l && c && *l > *c;
}

// Asks GitHub for the latest release. If it is newer than this build: with
// install, swaps it in next to the running binary (the running process keeps
// its old file); without, only says so. Returns a line for the footer, or ""
// when there's nothing to say: up to date, offline, a dev build, or a
// platform without prebuilt binaries. Blocking: run it off the UI thread.
std::string self_update(bool install);

}  // namespace anton
