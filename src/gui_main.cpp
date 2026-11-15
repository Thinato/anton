// anton-gui: flags, the window, the loop, and --screenshot (06-gui.md).
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "gui.hpp"
#include "update.hpp"

extern const unsigned char font_Inter_Regular[], font_Inter_Medium[], font_Inter_SemiBold[], font_JetBrainsMono_Regular[];
extern const unsigned font_Inter_Regular_size, font_Inter_Medium_size, font_Inter_SemiBold_size, font_JetBrainsMono_Regular_size;

namespace {

const char* kUsage =
    "usage: anton-gui [--query '<github search>'] [--no-plugins] [--version]\n"
    "                 [--screenshot out.png --screen list|reader|settings|palette\n"
    "                  [--theme dark|light] [--pr N] [--file F] [--size WxH] [--scale S]\n"
    "                  [--palette-query Q] [--settings-page general|appearance|views|plugins]\n"
    "                  [--press KEY [--wait SECONDS]] [--keys 'j j <Enter> J r']]\n"
    "\n"
    "  --query Q       open a single ad-hoc view instead of the configured ones\n"
    "  --no-plugins    start without plugins\n"
    "  --version       print the version\n"
    "  --screenshot    render a screen offscreen once its data is in, write it as a PNG, exit\n"
    "  --press KEY     with --screenshot: press a plugin's key first (T: run the specs), shoot\n"
    "                  --wait seconds later (4)\n"
    "  --keys K        with --screenshot: type keys first, space-separated: a character, or\n"
    "                  <Enter> <Esc> <Tab> <Up> <Down> <Ctrl-K> <Ctrl-,>\n"
    "\n"
    "config: $XDG_CONFIG_HOME/anton/config.json (default ~/.config/anton/config.json)\n"
    "layout: $XDG_CONFIG_HOME/anton/gui-layout.ini\n";

struct Shot {
  std::string out, screen = "list", theme, file, palette_query, settings_page = "appearance", press;
  std::vector<std::string> keys;  // --keys, one a few frames apart
  int pr = 0, w = 1440, h = 900;
  double wait = 4;
  float scale = 1;
};

// The few GL calls --screenshot makes besides ImGui's: a framebuffer to
// render into and read back. Loaded through GLFW, as ImGui's loader does.
using GLuint = unsigned int;
using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
#ifndef APIENTRY
#define APIENTRY
#endif
struct Gl {
  void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
  void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
  void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
  void(APIENTRY* GenTextures)(GLsizei, GLuint*);
  void(APIENTRY* BindTexture)(GLenum, GLuint);
  void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
  void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
  void(APIENTRY* Viewport)(GLint, GLint, GLsizei, GLsizei);
  void(APIENTRY* ClearColor)(float, float, float, float);
  void(APIENTRY* Clear)(unsigned);
  void(APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
  void(APIENTRY* PixelStorei)(GLenum, GLint);
  bool load() {
    auto get = [](const char* n) { return glfwGetProcAddress(n); };
    GenFramebuffers = reinterpret_cast<decltype(GenFramebuffers)>(get("glGenFramebuffers"));
    BindFramebuffer = reinterpret_cast<decltype(BindFramebuffer)>(get("glBindFramebuffer"));
    FramebufferTexture2D = reinterpret_cast<decltype(FramebufferTexture2D)>(get("glFramebufferTexture2D"));
    CheckFramebufferStatus = reinterpret_cast<decltype(CheckFramebufferStatus)>(get("glCheckFramebufferStatus"));
    GenTextures = reinterpret_cast<decltype(GenTextures)>(get("glGenTextures"));
    BindTexture = reinterpret_cast<decltype(BindTexture)>(get("glBindTexture"));
    TexImage2D = reinterpret_cast<decltype(TexImage2D)>(get("glTexImage2D"));
    TexParameteri = reinterpret_cast<decltype(TexParameteri)>(get("glTexParameteri"));
    Viewport = reinterpret_cast<decltype(Viewport)>(get("glViewport"));
    ClearColor = reinterpret_cast<decltype(ClearColor)>(get("glClearColor"));
    Clear = reinterpret_cast<decltype(Clear)>(get("glClear"));
    ReadPixels = reinterpret_cast<decltype(ReadPixels)>(get("glReadPixels"));
    PixelStorei = reinterpret_cast<decltype(PixelStorei)>(get("glPixelStorei"));
    return GenFramebuffers && BindFramebuffer && FramebufferTexture2D && CheckFramebufferStatus && GenTextures &&
           BindTexture && TexImage2D && TexParameteri && Viewport && ClearColor && Clear && ReadPixels && PixelStorei;
  }
};
constexpr GLenum GL_FRAMEBUFFER_ = 0x8D40, GL_COLOR_ATTACHMENT0_ = 0x8CE0, GL_TEXTURE_2D_ = 0x0DE1, GL_RGBA_ = 0x1908,
                 GL_RGBA8_ = 0x8058, GL_UNSIGNED_BYTE_ = 0x1401, GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5,
                 GL_TEXTURE_MIN_FILTER_ = 0x2801, GL_LINEAR_ = 0x2601, GL_PACK_ALIGNMENT_ = 0x0D05;
constexpr unsigned GL_COLOR_BUFFER_BIT_ = 0x4000;

ImFont* add_font(const unsigned char* data, unsigned size, bool merge = false) {
  ImFontConfig c;
  c.FontDataOwnedByAtlas = false;
  c.MergeMode = merge;
  return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), 13.f, &c);
}

ImFont* add_file(const char* path, bool merge) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return nullptr;
  ImFontConfig c;
  c.MergeMode = merge;
  return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, 13.f, &c);
}

// Inter for text with JetBrains Mono under it for the symbols Inter lacks
// (✓ ✗ ⋯ ◦ ▶), then the system's symbol font for the rest (⚙).
ImFont* ui_font(const unsigned char* data, unsigned size) {
  ImFont* f = add_font(data, size);
  add_font(font_JetBrainsMono_Regular, font_JetBrainsMono_Regular_size, true);
  for (const char* p : {"/System/Library/Fonts/Apple Symbols.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"})
    if (add_file(p, true)) break;
  return f;
}

ImFont* system_mono() {
  for (const char* p : {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc",
                        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf"})
    if (ImFont* f = add_file(p, false)) {
      add_font(font_JetBrainsMono_Regular, font_JetBrainsMono_Regular_size, true);
      return f;
    }
  return nullptr;
}

bool parse_args(int argc, char** argv, anton::Config& cfg, Shot& shot, int& exit_code) {
  auto value = [&](int& i, const char* flag) -> std::string {
    if (i + 1 >= argc) {
      std::fprintf(stderr, "anton-gui: %s needs a value\n%s", flag, kUsage);
      exit_code = 2;
      return "";
    }
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      std::fputs(kUsage, stdout);
      exit_code = 0;
      return false;
    } else if (a == "--version") {
      std::puts(anton::kVersion);
      exit_code = 0;
      return false;
    } else if (a == "--no-plugins") {
      cfg.plugins.clear();
    } else if (a == "--query") {
      cfg.adhoc_query = value(i, "--query");
    } else if (a == "--screenshot") {
      shot.out = value(i, "--screenshot");
    } else if (a == "--screen") {
      shot.screen = value(i, "--screen");
    } else if (a == "--theme") {
      shot.theme = value(i, "--theme");
    } else if (a == "--pr") {
      shot.pr = std::atoi(value(i, "--pr").c_str());
    } else if (a == "--file") {
      shot.file = value(i, "--file");
    } else if (a == "--palette-query") {
      shot.palette_query = value(i, "--palette-query");
    } else if (a == "--press") {
      shot.press = value(i, "--press");
    } else if (a == "--keys") {
      std::string v = value(i, "--keys");
      for (size_t at = 0; at < v.size();) {
        size_t sp = v.find(' ', at);
        if (std::string k = v.substr(at, sp == std::string::npos ? std::string::npos : sp - at); !k.empty()) shot.keys.push_back(k);
        if (sp == std::string::npos) break;
        at = sp + 1;
      }
    } else if (a == "--wait") {
      shot.wait = std::atof(value(i, "--wait").c_str());
    } else if (a == "--settings-page") {
      shot.settings_page = value(i, "--settings-page");
    } else if (a == "--size") {
      std::string v = value(i, "--size");
      if (std::sscanf(v.c_str(), "%dx%d", &shot.w, &shot.h) != 2 || shot.w < 320 || shot.h < 240) {
        std::fprintf(stderr, "anton-gui: --size takes WxH, like 1440x900\n");
        exit_code = 2;
        return false;
      }
    } else if (a == "--scale") {
      shot.scale = static_cast<float>(std::atof(value(i, "--scale").c_str()));
      if (shot.scale < 1 || shot.scale > 3) shot.scale = 1;
    } else {
      std::fprintf(stderr, "anton-gui: unknown argument %s\n%s", a.c_str(), kUsage);
      exit_code = 2;
      return false;
    }
    if (exit_code >= 0) return false;
  }
  static const char* screens[] = {"list", "reader", "settings", "palette"};
  if (std::none_of(std::begin(screens), std::end(screens), [&](const char* s) { return shot.screen == s; })) {
    std::fprintf(stderr, "anton-gui: --screen is list, reader, settings or palette\n");
    exit_code = 2;
    return false;
  }
  if (!shot.theme.empty() && shot.theme != "dark" && shot.theme != "light") {
    std::fprintf(stderr, "anton-gui: --theme is dark or light\n");
    exit_code = 2;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace anton;
  using namespace anton::gui;
  std::signal(SIGPIPE, SIG_IGN);
  Config cfg;
  if (std::string err = load_config(cfg); !err.empty()) {
    std::fprintf(stderr, "anton-gui: %s\n", err.c_str());
    return 2;
  }
  Shot shot;
  int code = -1;
  if (!parse_args(argc, argv, cfg, shot, code)) return code < 0 ? 0 : code;
  bool shooting = !shot.out.empty();
  if (shooting && !shot.theme.empty()) cfg.gui.theme = shot.theme;

  glfwSetErrorCallback([](int e, const char* d) { std::fprintf(stderr, "anton-gui: glfw error %d: %s\n", e, d); });
  if (!glfwInit()) {
    std::fprintf(stderr, "anton-gui: can't open a window (no display?)\n");
    return 1;
  }
#ifdef __APPLE__
  const char* glsl = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, 1);
#else
  const char* glsl = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
  if (shooting) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  int ww = shooting ? shot.w : 1440, wh = shooting ? shot.h : 900;
  if (!shooting)
    if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
      int x, y, w, h;
      glfwGetMonitorWorkarea(m, &x, &y, &w, &h);
      if (w > 0 && h > 0) ww = std::min(ww, w - 40), wh = std::min(wh, h - 60);
    }
  GLFWwindow* window = glfwCreateWindow(ww, wh, "anton", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "anton-gui: can't create an OpenGL 3 window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  static std::string ini = gui_layout_path();
  if (shooting) {
    io.IniFilename = nullptr;  // a screenshot is the default layout, and leaves none behind
  } else {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(ini).parent_path(), ec);
    io.IniFilename = ini.c_str();
  }
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl);

  auto gui = std::make_unique<Gui>(std::move(cfg));
  Gui* g = gui.get();
  g->window = window;
  g->screenshot = shooting;
  g->f.regular = ui_font(font_Inter_Regular, font_Inter_Regular_size);
  g->f.medium = ui_font(font_Inter_Medium, font_Inter_Medium_size);
  g->f.semibold = ui_font(font_Inter_SemiBold, font_Inter_SemiBold_size);
  ImFont* jetbrains = add_font(font_JetBrainsMono_Regular, font_JetBrainsMono_Regular_size);
  ImFont* sysmono = system_mono();
  g->f.mono = jetbrains;
  g->apply_theme();

  for (int sig : {SIGTERM, SIGHUP, SIGINT}) {
    struct sigaction sa {};
    sa.sa_handler = [](int s) {
      Plugins::kill_all_from_signal();
      ::signal(s, SIG_DFL);
      ::raise(s);
    };
    sigaction(sig, &sa, nullptr);
  }
  g->start();

  Gl gl{};
  bool have_gl = gl.load();
  GLuint fbo = 0, tex = 0;
  int fw = static_cast<int>(shot.w * shot.scale), fh = static_cast<int>(shot.h * shot.scale);
  if (shooting) {
    if (!have_gl) {
      std::fprintf(stderr, "anton-gui: --screenshot needs OpenGL framebuffers\n");
      return 1;
    }
    gl.GenTextures(1, &tex);
    gl.BindTexture(GL_TEXTURE_2D_, tex);
    gl.TexImage2D(GL_TEXTURE_2D_, 0, static_cast<GLint>(GL_RGBA8_), fw, fh, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, nullptr);
    gl.TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, static_cast<GLint>(GL_LINEAR_));
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER_, fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D_, tex, 0);
    if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_) != GL_FRAMEBUFFER_COMPLETE_) {
      std::fprintf(stderr, "anton-gui: can't make an offscreen framebuffer\n");
      return 1;
    }
  }

  auto t0 = std::chrono::steady_clock::now();
  auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  int hot = 3;          // frames to draw at full rate after something happened
  bool staged = false;  // --screenshot: the screen it asked for is set up
  bool pressed = false;  // ...and --press is done
  double ready_at = 0;
  int exit_status = 0;
  while (!glfwWindowShouldClose(window)) {
    if (shooting) {
      glfwPollEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    } else if (hot > 0 || io.WantTextInput || ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      glfwWaitEventsTimeout(hot > 0 ? 0.0 : 0.5);
      --hot;
    } else {
      auto before = std::chrono::steady_clock::now();
      glfwWaitEventsTimeout(1.0);
      if (std::chrono::steady_clock::now() - before < std::chrono::milliseconds(950)) hot = 3;
    }
    g->drain();
    g->f.mono = g->cfg.gui.code_font == "System monospace" && sysmono ? sysmono : jetbrains;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    if (shooting) {
      io.DisplaySize = ImVec2(static_cast<float>(shot.w), static_cast<float>(shot.h));
      io.DisplayFramebufferScale = ImVec2(shot.scale, shot.scale);
      io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    ImGui::NewFrame();
    g->frame();
    ImGui::Render();

    if (shooting) {
      // set the screen up once the list has its rows, then wait for its data
      if (!staged && !g->cur_rows.empty() && !g->loading() && secs() > 0.8) {
        staged = true;
        std::optional<Pr> pick;
        if (!g->cur_rows.empty()) pick = g->cur_rows[0];
        if (shot.pr)
          for (size_t i = 0; i < g->tabs.size(); ++i) {
            auto rs = g->rows(i);
            if (!rs) continue;
            auto it = std::find_if(rs->begin(), rs->end(), [&](const Pr& p) { return p.number == shot.pr; });
            if (it == rs->end()) continue;
            pick = *it;
            g->switch_view(i);
            g->sel_url = it->url;
            g->scroll_to_sel = true;
            break;
          }
        if (shot.screen == "reader" || (shot.screen == "palette" && !shot.file.empty())) {
          if (pick) g->open_reader(*pick);
        } else if (shot.screen == "settings") {
          g->settings_open = true;
          const char* pages[] = {"general", "appearance", "views", "plugins"};
          for (int i = 0; i < 4; ++i)
            if (shot.settings_page == pages[i]) g->settings_page = i;
        }
        if (shot.screen == "palette") {
          g->palette_open = true;
          g->palette_q = shot.palette_query;
        }
      }
      bool ready = staged;
      if (ready && g->screen == Screen::Reader) {
        const auto& b = g->bodies[g->reader_url];
        const Core::Files* fs = g->reader_files();
        ready = !b.loading && b.updated != -1 && fs && !fs->loading;
        if (ready && !shot.file.empty()) {
          auto ds = g->docs();
          bool found = false;
          for (int pass = 0; pass < 3 && !found; ++pass)  // the path, then the tab's name, then a path's tail
            for (size_t i = 0; i < ds.size() && !found; ++i)
              if (pass == 0 ? ds[i].path == shot.file : pass == 1 ? ds[i].name == shot.file : ds[i].path.ends_with("/" + shot.file))
                g->reader_tab = i, found = true;
        }
      }
      if (ready && g->screen == Screen::List && !g->cur_rows.empty())
        if (const Pr* p = g->selected_pr(); p && p->url == g->sel_url)
          ready = !g->bodies[p->url].loading && g->bodies[p->url].updated != -1;
      if (ready && !ready_at) ready_at = secs();
      bool timed_out = secs() > 30 + shot.wait;
      double settle = 1.5;
      if (!shot.press.empty() && ready_at) {  // a key, then its effect a while later
        settle += shot.wait;
        if (!pressed && secs() - ready_at > 1.5) {
          pressed = true;
          const Core::FileTab* ft = g->screen == Screen::Reader ? g->reader_tab_file() : nullptr;
          std::string url = g->screen == Screen::Reader ? g->reader_url : g->sel_url;
          g->plugin_action(shot.press, url, ft && ft->spec ? ft->path : "");
        }
      }
      if (ready_at && secs() - ready_at > 1.0 && !shot.keys.empty()) {  // type --keys, one every few frames
        static int frame = 0;
        if (++frame % 4 == 0) {
          std::string key = shot.keys.front();
          shot.keys.erase(shot.keys.begin());
          auto tap = [&](ImGuiKey k, bool ctrl = false) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(k, true);
            io.AddKeyEvent(k, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
          };
          if (key == "<Enter>") tap(ImGuiKey_Enter);
          else if (key == "<Esc>") tap(ImGuiKey_Escape);
          else if (key == "<Tab>") tap(ImGuiKey_Tab);
          else if (key == "<Up>") tap(ImGuiKey_UpArrow);
          else if (key == "<Down>") tap(ImGuiKey_DownArrow);
          else if (key == "<Ctrl-K>") tap(ImGuiKey_K, true);
          else if (key == "<Ctrl-,>") tap(ImGuiKey_Comma, true);
          else for (char c : key) io.AddInputCharacter(static_cast<unsigned>(static_cast<unsigned char>(c)));
        }
        ready_at = secs() - 1.0;  // the settling starts after the last key
      }
      if ((ready_at && secs() - ready_at > settle && shot.keys.empty()) || timed_out) {
        if (timed_out) std::fprintf(stderr, "anton-gui: --screenshot: data didn't arrive in 30s; capturing what's there\n");
        gl.Viewport(0, 0, fw, fh);
        gl.ClearColor(0, 0, 0, 1);
        gl.Clear(GL_COLOR_BUFFER_BIT_);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        std::vector<unsigned char> px(static_cast<size_t>(fw) * fh * 4);
        gl.PixelStorei(GL_PACK_ALIGNMENT_, 1);
        gl.ReadPixels(0, 0, fw, fh, GL_RGBA_, GL_UNSIGNED_BYTE_, px.data());
        std::vector<unsigned char> flipped(px.size());
        for (int y = 0; y < fh; ++y)
          std::memcpy(&flipped[static_cast<size_t>(y) * fw * 4], &px[static_cast<size_t>(fh - 1 - y) * fw * 4], static_cast<size_t>(fw) * 4);
        for (size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
        if (!stbi_write_png(shot.out.c_str(), fw, fh, 4, flipped.data(), fw * 4)) {
          std::fprintf(stderr, "anton-gui: can't write %s\n", shot.out.c_str());
          exit_status = 1;
        } else {
          std::printf("%s\n", shot.out.c_str());
        }
        break;
      }
      gl.Viewport(0, 0, fw, fh);
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      continue;
    }

    int dw, dh;
    glfwGetFramebufferSize(window, &dw, &dh);
    if (have_gl) {
      gl.Viewport(0, 0, dw, dh);
      ImVec4 bg = g->vec(g->t.border);
      gl.ClearColor(bg.x, bg.y, bg.z, 1);
      gl.Clear(GL_COLOR_BUFFER_BIT_);
    }
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  if (g->plugins) g->plugins->stop();
  if (!shooting) ImGui::SaveIniSettingsToDisk(ini.c_str());
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  std::fflush(stdout);
  std::_Exit(exit_status);  // worker threads may still be blocked on a child process
}
