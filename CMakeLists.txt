cmake_minimum_required(VERSION 3.20)
project(anton VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ANTON_SLKX_PLUGIN "Build the anton-slkx Slack plugin" ON)
option(ANTON_LLM_PLUGIN "Build the anton-llm review plugin" ON)
option(ANTON_TEMOC_PLUGIN "Build the anton-temoc acceptance-spec plugin" ON)
option(ANTON_GUI "Build anton-gui, the desktop UI (Dear ImGui, GLFW, OpenGL 3)" ON)

include(FetchContent)

find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v7.0.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ftxui)

# md4c is one C file. Built here as a static library rather than through its
# own CMakeLists, which defaults to a shared library and adds install rules
# for it (SOURCE_SUBDIR points at nothing, so only the sources are fetched).
FetchContent_Declare(md4c
  URL https://github.com/mity/md4c/archive/refs/tags/v0.6.0.tar.gz
  URL_HASH SHA256=4d151298125a81da3b2efa2e0eed8bdb7a9318569804e4fa4d7a2375ab83ef70
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(md4c)
add_library(md4c STATIC ${md4c_SOURCE_DIR}/src/md4c.c)
target_include_directories(md4c SYSTEM PUBLIC ${md4c_SOURCE_DIR}/src)

if(MSVC)
  set(WARN /W4)
else()
  set(WARN -Wall -Wextra -Wpedantic)
endif()

add_executable(anton
  src/main.cpp
  src/core.cpp
  src/markdown.cpp
  src/proc.cpp
  src/github.cpp
  src/plugins.cpp
  src/ui.cpp
  src/ui_reader.cpp
  src/ui_settings.cpp
  src/update.cpp
)
target_include_directories(anton PRIVATE src)
target_link_libraries(anton PRIVATE nlohmann_json::nlohmann_json
  ftxui::component md4c)
target_compile_options(anton PRIVATE ${WARN})
# Release builds pass the tag (release.yml); "dev" never self-updates.
set(ANTON_VERSION "dev" CACHE STRING "Version baked into anton")
target_compile_definitions(anton PRIVATE ANTON_VERSION="${ANTON_VERSION}")
install(TARGETS anton RUNTIME DESTINATION bin)

if(ANTON_SLKX_PLUGIN)
  add_executable(anton-slkx plugins/slkx/main.cpp src/proc.cpp)
  target_include_directories(anton-slkx PRIVATE src plugins/slkx)
  target_link_libraries(anton-slkx PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(anton-slkx PRIVATE ${WARN})
  install(TARGETS anton-slkx RUNTIME DESTINATION bin)
endif()

if(ANTON_LLM_PLUGIN)
  add_executable(anton-llm plugins/llm/main.cpp src/proc.cpp)
  target_include_directories(anton-llm PRIVATE src plugins/llm)
  target_link_libraries(anton-llm PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(anton-llm PRIVATE ${WARN})
  install(TARGETS anton-llm RUNTIME DESTINATION bin)
endif()

if(ANTON_TEMOC_PLUGIN)
  add_executable(anton-temoc plugins/temoc/main.cpp src/proc.cpp)
  target_include_directories(anton-temoc PRIVATE src plugins/temoc)
  target_link_libraries(anton-temoc PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(anton-temoc PRIVATE ${WARN})
  install(TARGETS anton-temoc RUNTIME DESTINATION bin)
endif()

if(ANTON_GUI)
  # GLFW opens the window; it needs the X11 headers on Linux (X11 only:
  # Wayland sessions run it through XWayland). OpenGL is loaded at run time
  # (ImGui's loader, glfwGetProcAddress), so no GL library is linked there.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11)
    if(NOT X11_FOUND OR NOT X11_Xrandr_FOUND OR NOT X11_Xinerama_FOUND OR NOT X11_Xcursor_FOUND OR NOT X11_Xi_FOUND)
      message(FATAL_ERROR "anton-gui needs the X11 development headers (Debian/Ubuntu: "
        "sudo apt install xorg-dev libgl-dev). To build only the terminal UI, configure with -DANTON_GUI=OFF.")
    endif()
  elseif(NOT APPLE)
    message(FATAL_ERROR "anton-gui builds on macOS and Linux. Configure with -DANTON_GUI=OFF.")
  endif()

  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(glfw)

  # Dear ImGui's docking branch, built here from its sources (it ships no
  # CMakeLists), with the GLFW and OpenGL 3 backends.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_compile_definitions(imgui PUBLIC GLFW_INCLUDE_NONE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  target_link_libraries(imgui PUBLIC glfw)
  if(APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
  else()
    target_link_libraries(imgui PUBLIC ${CMAKE_DL_LIBS})
  endif()

  # The PNG writer for --screenshot (public domain, one header).
  FetchContent_Declare(stb_image_write
    URL https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c/stb_image_write.h
    URL_HASH SHA256=a8e7f77f0ccfdc9bac09a71268b131931ff81aafca82c393aeb6aaba10d9e7ca
    DOWNLOAD_NO_EXTRACT TRUE)
  FetchContent_MakeAvailable(stb_image_write)

  # Inter and JetBrains Mono (SIL OFL 1.1, assets/fonts/*-OFL.txt), compiled in.
  add_executable(anton_embed cmake/embed.cpp)
  set(fonts Inter-Regular Inter-Medium Inter-SemiBold JetBrainsMono-Regular)
  set(embed_args)
  set(font_files)
  foreach(f IN LISTS fonts)
    string(REPLACE "-" "_" sym "font_${f}")
    list(APPEND embed_args ${sym} ${CMAKE_SOURCE_DIR}/assets/fonts/${f}.ttf)
    list(APPEND font_files ${CMAKE_SOURCE_DIR}/assets/fonts/${f}.ttf)
  endforeach()
  add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/gui_fonts.cpp
    COMMAND anton_embed ${CMAKE_BINARY_DIR}/gui_fonts.cpp ${embed_args}
    DEPENDS anton_embed ${font_files}
    COMMENT "Embedding anton-gui's fonts")

  add_executable(anton-gui
    src/gui_main.cpp src/gui.cpp src/gui_list.cpp src/gui_reader.cpp src/gui_settings.cpp
    src/core.cpp src/markdown.cpp src/proc.cpp src/github.cpp src/plugins.cpp src/update.cpp
    ${CMAKE_BINARY_DIR}/gui_fonts.cpp)
  if(APPLE)  # the native menu bar: Objective-C++, compiled as C++ so GLFW's languages stay as they are
    target_sources(anton-gui PRIVATE src/gui_menu_mac.mm)
    set_source_files_properties(src/gui_menu_mac.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
    target_link_libraries(anton-gui PRIVATE "-framework Cocoa")
  endif()
  target_include_directories(anton-gui PRIVATE src)
  target_include_directories(anton-gui SYSTEM PRIVATE ${stb_image_write_SOURCE_DIR})
  target_link_libraries(anton-gui PRIVATE nlohmann_json::nlohmann_json imgui md4c ftxui::screen)
  target_compile_options(anton-gui PRIVATE ${WARN})
  target_compile_definitions(anton-gui PRIVATE ANTON_VERSION="${ANTON_VERSION}" IMGUI_DEFINE_MATH_OPERATORS)
  install(TARGETS anton-gui RUNTIME DESTINATION bin)
endif()

# Self-checks: assert-based mains over the pure headers. -UNDEBUG because a
# Release build defines NDEBUG, which would compile every assert away.
enable_testing()
foreach(t IN ITEMS model filters fold llm markdown temoc gui)
  add_executable(${t}_check tests/${t}_check.cpp)
  target_include_directories(${t}_check PRIVATE src plugins/slkx plugins/llm plugins/temoc)
  target_link_libraries(${t}_check PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(${t}_check PRIVATE -UNDEBUG ${WARN})
  add_test(NAME ${t}_check COMMAND ${t}_check)
endforeach()
# The markdown core is a .cpp over md4c, measured with FTXUI's string_width.
target_sources(markdown_check PRIVATE src/markdown.cpp)
target_link_libraries(markdown_check PRIVATE md4c ftxui::screen)
# gui_check: anton-gui's pure parts, which lay out markdown blocks too.
target_sources(gui_check PRIVATE src/markdown.cpp)
target_link_libraries(gui_check PRIVATE md4c ftxui::screen)
# core_check: the state both UIs share, driven without a UI or a worker.
add_executable(core_check tests/core_check.cpp
  src/core.cpp src/markdown.cpp src/proc.cpp src/github.cpp src/plugins.cpp src/update.cpp)
target_include_directories(core_check PRIVATE src)
target_link_libraries(core_check PRIVATE nlohmann_json::nlohmann_json md4c ftxui::screen)
target_compile_options(core_check PRIVATE -UNDEBUG ${WARN})
add_test(NAME core_check COMMAND core_check)
