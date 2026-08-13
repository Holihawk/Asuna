cmake_minimum_required(VERSION 3.26)
project(asuna CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# GLES is the default backend: GtkGLArea cannot hand out a desktop
# compatibility profile, and the renderer's shaders were already GLES-shaped.
option(LIVE2D_GLES "Build the Live2D renderer against OpenGL ES 3" ON)

# ---------------------------------------------------------------------------
# Vendored Cubism 2.1 runtime (third_party/live2d-v2).
# Only Common/, Glad/ and V2/ are vendored. V3/ is deliberately absent: it is
# the one part of upstream that carries prebuilt proprietary Cubism Core
# binaries, and our model is Cubism 2.1 (.moc) so we never need it.
# ---------------------------------------------------------------------------
set(LIVE2D_ROOT ${CMAKE_CURRENT_LIST_DIR}/third_party/live2d-v2)

add_subdirectory(${LIVE2D_ROOT}/Common ${CMAKE_BINARY_DIR}/live2d/Common)
if(NOT LIVE2D_GLES)
  add_subdirectory(${LIVE2D_ROOT}/Glad ${CMAKE_BINARY_DIR}/live2d/Glad)
  find_package(OpenGL REQUIRED)
endif()
include(${LIVE2D_ROOT}/V2/cmake/V2.cmake)   # defines target V2 / Live2D::V2

# ---------------------------------------------------------------------------
# asuna - the desktop pet
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LAYERSHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
# For the parts of Phase 5 that are GLib but not GTK - the control socket sits
# on the main loop, and its test has no business linking a toolkit.
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

# src/ is laid out by concern: app/ is the process and its interfaces, pet/ is
# the figure and how she moves, character/ is what she does on her own, ui/ is
# everything that touches GTK. json and paths stay at the root because they
# belong to no layer - all four of those use them.
add_executable(asuna
  src/main.cpp
  src/json.cpp
  src/paths.cpp

  src/app/argparse.cpp
  src/app/cli/dispatch.cpp
  src/app/cli/autostart.cpp
  src/app/cli/common.cpp
  src/app/cli/config_commands.cpp
  src/app/cli/extension_commands.cpp
  src/app/cli/lifecycle.cpp
  src/app/cli/options.cpp
  src/app/cli/protocol.cpp
  src/app/cli/subscribe.cpp
  src/app/config/config.cpp
  src/app/config/toml.cpp
  src/app/config_apply.cpp
  src/app/daemon.cpp
  src/app/ipc.cpp
  src/app/state.cpp

  src/pet/pet.cpp
  src/pet/framing.cpp
  src/pet/motion.cpp
  src/pet/outfits.cpp

  src/character/behaviour.cpp
  src/character/dialogue.cpp

  # One Shell class, split by responsibility: construction, runtime state,
  # rendering, layout, input regions, pointer input, speech, commands,
  # extensions, and synthetic-input diagnostics.
  src/ui/shell/construction.cpp
  src/ui/shell/runtime.cpp
  src/ui/shell/render.cpp
  src/ui/shell/layout.cpp
  src/ui/shell/region.cpp
  src/ui/shell/input.cpp
  src/ui/shell/speech.cpp
  src/ui/shell/command.cpp
  src/ui/shell/extension.cpp
  src/ui/shell/debug.cpp
  src/ui/bubble.cpp
  src/ui/menu.cpp
)
target_include_directories(asuna PRIVATE src)
# NOTE: gtk4-layer-shell MUST come before gtk4 on the link line. It works by
# interposing on libwayland-client's global registry, so if GTK (which pulls in
# libwayland-client) appears first in DT_NEEDED, the interposition never
# happens: gtk_layer_init_for_window() then silently does nothing and the
# window comes up as an ordinary xdg-toplevel. Verify with:
#   readelf -d build/asuna | grep NEEDED
target_link_libraries(asuna PRIVATE
  PkgConfig::LAYERSHELL
  PkgConfig::GTK4
  Live2D::V2
)

# ---------------------------------------------------------------------------
# Motion tests: the drag feel, asserted offline. No GL and no compositor, so
# these run anywhere and cover what cannot be checked without a real pointer.
# ---------------------------------------------------------------------------
add_executable(asuna-motion-test tests/motion_test.cpp src/pet/motion.cpp)
target_include_directories(asuna-motion-test PRIVATE src)

# The personality is scheduling on a timescale of minutes, so the same argument
# applies twice over: nobody verifies a 30-minute sleep timer by watching it.
add_executable(asuna-behaviour-test
  tests/behaviour_test.cpp src/character/behaviour.cpp src/character/dialogue.cpp src/json.cpp)
target_include_directories(asuna-behaviour-test PRIVATE src)

# The control protocol. GLib but not GTK, and no compositor: a server, a main
# loop and a client on a second thread cover encode/frame/parse/dispatch/reply.
add_executable(asuna-ipc-test
  tests/ipc_test.cpp src/app/ipc.cpp src/json.cpp src/paths.cpp)
target_include_directories(asuna-ipc-test PRIVATE src)
target_link_libraries(asuna-ipc-test PRIVATE PkgConfig::GLIB)

# Dependency direction: app value types and parser interfaces must not include
# ui/. The UI may consume app/, pet/ and character/; app mapping may consume
# GTK-free character value types. The targets below enforce the important part:
# parser-only and public app-header checks receive only -Isrc, never GTK flags.
#
# Config parsing remains its own leaf. Config::applyTo is intentionally absent:
# it lives in a separate GTK-free TU so this target need not link mapping code.
add_executable(asuna-config-test
  tests/config_test.cpp src/app/config/config.cpp src/app/config/toml.cpp src/paths.cpp)
target_include_directories(asuna-config-test PRIVATE src)

# Config-to-options mapping has its own test and TU. This proves mapping remains
# owned by app/config without making the parser suite depend on it or on GTK.
add_executable(asuna-config-apply-test
  tests/config_apply_test.cpp src/app/config_apply.cpp src/app/config/config.cpp
  src/app/config/toml.cpp src/paths.cpp)
target_include_directories(asuna-config-apply-test PRIVATE src)

# Compile-only boundary guard for public application headers. It deliberately
# receives no GTK/GLib imported targets or include flags; an app -> ui include
# therefore breaks the ordinary build at compile time.
add_library(asuna-app-headers-test OBJECT tests/app_headers_test.cpp)
target_include_directories(asuna-app-headers-test PRIVATE src)

# What the command line is allowed to say. A leaf with no dependencies at all -
# not even paths - so the boundary and overflow cases cost nothing to run.
add_executable(asuna-argparse-test tests/argparse_test.cpp src/app/argparse.cpp)
target_include_directories(asuna-argparse-test PRIVATE src)

# The shared JSON reader and writer, which four things stand on: every outfit's
# index.json, the dialogue file, the control protocol and the state file. Also a
# leaf. Its tests used to ride along in behaviour_test, which was the first
# thing to need a parser.
add_executable(asuna-json-test tests/json_test.cpp src/json.cpp)
target_include_directories(asuna-json-test PRIVATE src)

# Whether a pid file still names the process that wrote it - the check that
# stands between `ext stop` and a stranger who inherited the number. GLib for
# the log filter that shares this file; no GTK, no GL, no socket.
add_executable(asuna-daemon-test tests/daemon_test.cpp src/app/daemon.cpp)
target_include_directories(asuna-daemon-test PRIVATE src)
target_link_libraries(asuna-daemon-test PRIVATE PkgConfig::GLIB)

# What she remembers between runs. No GTK and no GL: the state file is JSON and
# a path, and the suite points XDG_STATE_HOME at a temporary directory before
# the first call, so it cannot see a real state.json let alone write one.
add_executable(asuna-state-test
  tests/state_test.cpp src/app/state.cpp src/json.cpp src/paths.cpp)
target_include_directories(asuna-state-test PRIVATE src)

enable_testing()
add_test(NAME motion COMMAND asuna-motion-test)
add_test(NAME ipc COMMAND asuna-ipc-test)
add_test(NAME config COMMAND asuna-config-test)
add_test(NAME config_apply COMMAND asuna-config-apply-test)
add_test(NAME argparse COMMAND asuna-argparse-test)
add_test(NAME json COMMAND asuna-json-test)
add_test(NAME state COMMAND asuna-state-test)
add_test(NAME daemon COMMAND asuna-daemon-test)
# The wiring rather than the parsers: which bound each flag is actually given.
# Runs the real binary, and every case in it is decided before the control
# socket is touched, so it neither needs her running nor disturbs her if she is.
add_test(NAME cli COMMAND ${CMAKE_CURRENT_LIST_DIR}/tests/cli_test.sh $<TARGET_FILE:asuna>)
# Reads the shipped data/dialogue.zh.json, so it runs from the source tree.
add_test(NAME behaviour COMMAND asuna-behaviour-test
         WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
# The extension helper's failover, offline. python3 is not an added dependency:
# tools/asuna-ext.py is what `asuna ext start` runs, so a tree that cannot run
# this suite cannot run the feature it tests either. No network and no daemon -
# Provider.request is replaced with canned responses.
find_program(PYTHON3 python3)
if(PYTHON3)
  add_test(NAME ext COMMAND ${PYTHON3} ${CMAKE_CURRENT_LIST_DIR}/tests/ext_test.py)
else()
  message(WARNING "python3 not found - skipping the `ext` test suite.")
endif()

# ---------------------------------------------------------------------------
# Phase 0 spike / regression harness: renders offscreen, no compositor.
# ---------------------------------------------------------------------------
find_package(glfw3 QUIET)
if(glfw3_FOUND)
  # Shares the production framing solver and Pet so the harness cannot drift
  # from what ships - --hit exercises the real pointer/canvas transform.
  add_executable(asuna-render-test
    tests/render_test.cpp src/pet/framing.cpp src/pet/pet.cpp src/json.cpp)
  target_include_directories(asuna-render-test PRIVATE src)
  target_link_libraries(asuna-render-test PRIVATE Live2D::V2 glfw)
  if(NOT LIVE2D_GLES)
    target_link_libraries(asuna-render-test PRIVATE OpenGL::GL)
  endif()
else()
  message(WARNING "glfw3 not found - skipping asuna-render-test. "
                  "Install glfw-devel to build the Phase 0 spike.")
endif()
