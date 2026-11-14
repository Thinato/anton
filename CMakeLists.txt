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
  src/markdown.cpp
  src/proc.cpp
  src/github.cpp
  src/plugins.cpp
  src/ui.cpp
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

# Self-checks: assert-based mains over the pure headers. -UNDEBUG because a
# Release build defines NDEBUG, which would compile every assert away.
enable_testing()
foreach(t IN ITEMS model fold llm markdown)
  add_executable(${t}_check tests/${t}_check.cpp)
  target_include_directories(${t}_check PRIVATE src plugins/slkx plugins/llm)
  target_link_libraries(${t}_check PRIVATE nlohmann_json::nlohmann_json)
  target_compile_options(${t}_check PRIVATE -UNDEBUG ${WARN})
  add_test(NAME ${t}_check COMMAND ${t}_check)
endforeach()
# The markdown core is a .cpp over md4c, measured with FTXUI's string_width.
target_sources(markdown_check PRIVATE src/markdown.cpp)
target_link_libraries(markdown_check PRIVATE md4c ftxui::screen)
