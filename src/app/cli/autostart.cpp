#include "app/cli/internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace asuna {
namespace cli {

// --- autostart --------------------------------------------------------------

std::string autostartPath() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    const fs::path base = (xdg && *xdg) ? fs::path(xdg) : fs::path(home ? home : ".") / ".config";
    return (base / "autostart" / "asuna.desktop").string();
}

std::string exePath() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("asuna") : exe.string();
}

void printNiriHint() {
    printf("\nFor niri, put this in ~/.config/niri/config.kdl instead - the XDG\n"
           "autostart file above is ignored by compositors that do not run an\n"
           "autostart service:\n\n"
           "  spawn-at-startup \"%s\" \"start\" \"--foreground\"\n",
           exePath().c_str());
}

int cmdAutostart(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "status";
    const std::string path = autostartPath();
    std::error_code ec;

    if (verb == "status") {
        printf("asuna: autostart is %s (%s)\n", fs::exists(path, ec) ? "enabled" : "disabled",
               path.c_str());
        printNiriHint();
        return kOk;
    }
    if (verb == "disable") {
        if (!fs::remove(path, ec)) return complain("autostart was not enabled", kOk);
        printf("asuna: autostart disabled (%s removed)\n", path.c_str());
        return kOk;
    }
    if (verb != "enable") return complain("autostart takes enable, disable or status", kUsage);

    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return complain("cannot write " + path);
    // `start` rather than `start --foreground`: an XDG autostart runner launches
    // this and forgets it, so detaching leaves it nothing to supervise.
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=Asuna\n"
      << "Comment=Live2D desktop pet\n"
      << "Exec=" << exePath() << " start\n"
      << "Terminal=false\n"
      << "X-GNOME-Autostart-enabled=true\n";
    if (!f) return complain("cannot write " + path);
    printf("asuna: autostart enabled (%s)\n", path.c_str());
    printNiriHint();
    return kOk;
}

}  // namespace cli
}  // namespace asuna
