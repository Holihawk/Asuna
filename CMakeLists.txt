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
  src/app/cli.cpp
  src/app/config.cpp
  src/app/daemon.cpp
  src/app/ipc.cpp
  src/app/state.cpp

  src/pet/pet.cpp
  src/pet/framing.cpp
  src/pet/motion.cpp
  src/pet/outfits.cpp

  src/character/behaviour.cpp
  src/character/dialogue.cpp

  # One Shell class, split by what each part of it is for: the window and its
  # lifecycle, the pixels, the pointer, what she says, the control socket, what
  # an out-of-process extension can see, and the synthetic-input diagnostics.
  src/ui/shell.cpp
  src/ui/shell_render.cpp
  src/ui/shell_input.cpp
  src/ui/shell_speech.cpp
  src/ui/shell_command.cpp
  src/ui/shell_ext.cpp
  src/ui/shell_debug.cpp
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

# The config file. Config::applyTo lives in ui/shell.cpp precisely so this can be
# a parser test rather than a GTK one.
add_executable(asuna-config-test tests/config_test.cpp src/app/config.cpp src/paths.cpp)
target_include_directories(asuna-config-test PRIVATE src)

# What the command line is allowed to say. A leaf with no dependencies at all -
# not even paths - so the boundary and overflow cases cost nothing to run.
add_executable(asuna-argparse-test tests/argparse_test.cpp src/app/argparse.cpp)
target_include_directories(asuna-argparse-test PRIVATE src)

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
add_test(NAME argparse COMMAND asuna-argparse-test)
add_test(NAME state COMMAND asuna-state-test)
# The wiring rather than the parsers: which bound each flag is actually given.
# Runs the real binary, and every case in it is decided before the control
# socket is touched, so it neither needs her running nor disturbs her if she is.
add_test(NAME cli COMMAND ${CMAKE_CURRENT_LIST_DIR}/tests/cli_test.sh $<TARGET_FILE:asuna>)
# Reads the shipped data/dialogue.zh.json, so it runs from the source tree.
add_test(NAME behaviour COMMAND asuna-behaviour-test
         WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})

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
