#include <gtk/gtk.h>

#include <cstdio>
#include <utility>

#include "app/cli.hpp"
#include "app/daemon.hpp"
#include "ui/shell.hpp"

namespace {

// The daemon half of the dispatch: everything else in cli.cpp is a client that
// never touches GTK. Handed over as a function pointer so the fork-and-detach
// logic can live next to the command line it belongs to without dragging the
// toolkit in with it.
int runShell(asuna::ShellOptions opt) {
    // GTK 4.22 defaults to the Vulkan renderer, which costs ~22% of a core to
    // recomposite this full-width surface at 30fps; the GL renderer does the
    // same job for ~5%. Measured on Mesa/radeonsi. Not overwritten, so
    // GSK_RENDERER=... from the environment still wins.
    g_setenv("GSK_RENDERER", "gl", FALSE);
    asuna::daemon::installLogFilter();
    gtk_init();
    asuna::Shell shell(std::move(opt));
    return shell.run();
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffer stdout: as a daemon this is a file, and the C library would
    // otherwise pick full buffering and hold the log hostage until the pet
    // exits - which is exactly when you least want to be reading it.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    return asuna::cli::dispatch(argc, argv, runShell);
}
