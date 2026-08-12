#pragma once

#include "ui/shell.hpp"

namespace asuna {

// The command line: `asuna start`, `asuna say …`, `asuna exit`, and the bare
// `asuna --flags` form that runs her in the foreground the way every phase
// before this one did.
//
// Everything here except the two forms that run the pet is a client: it opens
// the control socket, sends one line, prints what comes back, and exits. That
// is deliberately the whole of it - the CLI knows the *names* of things and
// nothing about what they do, so a verb cannot behave differently depending on
// whether it arrived from the menu or the terminal.
namespace cli {

// Runs the pet in this process. Supplied by main.cpp so the fork-and-detach
// dance in here does not have to know about GTK.
using RunShell = int (*)(ShellOptions);

// Exit statuses. Distinguished because scripts ask different questions:
// `asuna status` failing because she is not running is not the same as failing
// because the socket is broken.
enum Status {
    kOk = 0,
    kError = 1,
    kUsage = 2,
    kNotRunning = 3,
    kAlreadyRunning = 4,
};

int dispatch(int argc, char** argv, RunShell runShell);

}  // namespace cli
}  // namespace asuna
