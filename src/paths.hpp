#pragma once

#include <string>

namespace asuna {

// Where things live.
//
// Collected here because Phase 5 is the point at which the process stops being
// something you launch from the source tree with a working directory that
// happens to be right. A daemon started by `~/.config/autostart/asuna.desktop`
// runs from the user's home directory, so every path that used to be relative
// to the current directory has to be found some other way.
namespace paths {

// $XDG_RUNTIME_DIR/asuna, created on demand. Falls back to /tmp/asuna-<uid>
// when the runtime dir is unset, which is not a Wayland session's normal state
// but is what a bare ssh login looks like.
std::string runtimeDir();

// $XDG_STATE_HOME/asuna, i.e. ~/.local/state/asuna. Not created: its two
// writers (the state file and the log) both create it themselves, and a path
// getter that makes directories surprises a `--no-persist` run.
std::string stateDir();

// $XDG_CONFIG_HOME/asuna, i.e. ~/.config/asuna. Not created - Phase 6 owns the
// config file, and nothing should be making the directory before there is
// something to put in it.
std::string configDir();

std::string socketPath();   // <runtime>/control.sock
std::string lockPath();     // <runtime>/asuna.lock
std::string logPath();      // <state>/asuna.log

// The extension helper. It runs out of process (see the README, "Extensions"),
// so unlike everything above these are about a program rather than about her.
std::string extPidPath();   // <runtime>/ext.pid
std::string extLogPath();   // <state>/ext.log
// The bundled helper script, or "" if it cannot be found. Searched the same way
// modelsRoot() is, and for the same reason: an autostarted daemon has no
// working directory worth anything.
std::string extHelper();

// The directory containing this executable, or "" if it cannot be determined.
std::string exeDir();

// The models/ directory holding the asuna_NN folders.
//
// Searched rather than assumed: $ASUNA_MODEL_DIR, then ./models (so running out
// of the source tree behaves exactly as it always has), then beside the binary,
// one level up from it, the installed share/asuna/models, and finally
// $XDG_DATA_HOME/asuna/models. Returns "models" if none of them exist, so the
// failure is the same "no such file" it was before rather than a silent
// difference.
std::string modelsRoot();

}  // namespace paths
}  // namespace asuna
