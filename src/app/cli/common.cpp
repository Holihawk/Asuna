#include "app/cli/internal.hpp"

#include <time.h>

#include <cstdio>
#include <string>

namespace asuna {
namespace cli {

bool gJson = false;   // --json: print the reply line instead of a summary

int nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int complain(const std::string& message, int status) {
    fprintf(stderr, "asuna: %s\n", message.c_str());
    return status;
}

// The first line of something multi-line, with an ellipsis if there was more.
// Her persona is a paragraph and `asuna ext config` is a summary; printing the
// whole thing there would bury everything else.
std::string firstLine(const std::string& text) {
    const size_t eol = text.find('\n');
    if (eol == std::string::npos) return text;
    return text.substr(0, eol) + " …";
}

void usage() {
    printf(
        "asuna - Live2D desktop pet\n"
        "\n"
        "usage: asuna <command> [arguments]\n"
        "       asuna [options]            run her here, in this terminal\n"
        "\n"
        "lifecycle\n"
        "  start [options]      launch in the background; --foreground to stay\n"
        "  exit                 ask her to leave (aliases: stop, quit)\n"
        "  restart [options]    exit, then start\n"
        "  status [--json]      running? where, how big, which outfit, how much RAM\n"
        "  ping                 is the control socket answering\n"
        "\n"
        "on screen\n"
        "  hide | show | toggle put her away without exiting, and bring her back\n"
        "  move <px|left|centre|right>\n"
        "  scale <n>                      clamped to 0.5-2.5 and to the screen\n"
        "  layer <top|bottom|overlay|background>\n"
        "  output [list|<name>]           which monitor she lives on\n"
        "  menu [open|close|toggle]       for a compositor keybind\n"
        "\n"
        "her\n"
        "  say <text> [--for S] a line in the speech bubble\n"
        "  motion [name|--list]\n"
        "  expression [name|--list]\n"
        "  model [list]         the outfit registry\n"
        "  model use <id>       change outfit, e.g. `asuna model use 31`\n"
        "\n"
        "extensions (off by default; see `asuna config init`, section [ext])\n"
        "  chat [text]          talk to her - opens a prompt if given no text\n"
        "  ext <start|stop|restart|status|config|cancel>\n"
        "                       stop/restart take --force, which signals a pid\n"
        "                       file too old to prove what it names\n"
        "  ext test             can each configured provider be reached\n"
        "  subscribe            print her events as they happen, until ^C\n"
        "\n"
        "session\n"
        "  autostart <enable|disable|status>\n"
        "  config [path|show|init|check|edit|reload]\n"
        "\n"
        "options (for `start` and the bare form; they beat the config file)\n"
        "  --model ID|PATH  outfit id (02, 31, ...) or a model index.json path\n"
        "                   (default: last used, else 02)\n"
        "  --layer NAME     top | bottom | overlay | background (default: last used)\n"
        "  --output NAME    monitor connector, e.g. eDP-1 (default: first)\n"
        "  --framing NAME   auto | bust | full (default auto, from index.json)\n"
        "  --anchor NAME    right | left | centre (default right)\n"
        "  --height PX      strip height for a bust framing (default 460)\n"
        "  --max-height PX  ceiling for full-body outfits (default 760)\n"
        "  --margin PX      gap from the anchored edge (default 24)\n"
        "  --bottom PX      gap above the bottom screen edge (default 0, flush)\n"
        "  --language NAME  picks data/dialogue.<name>.json (default zh)\n"
        "  --pad PX         transparent margin around her (default 8)\n"
        "  --band PX        strip reserved above her for speech (default 92)\n"
        "  --side F         width drawn past each side of her box, as a fraction\n"
        "                   of it, so outstretched arms are not clipped (0.30)\n"
        "  --gaze-halo PX   how far past her the cursor is still followed (160,\n"
        "                   0 disables); only claimed once she has been touched\n"
        "  --no-greet       skip the launch greeting\n"
        "  --scale F        size multiplier, 0.5-2.5 (default: last used, else 1)\n"
        "  --x PX           explicit horizontal position, overrides --anchor\n"
        "  --fps N          render cap, 0 = uncapped (default 30). Above half\n"
        "                   the display's refresh rate it cannot be paced evenly,\n"
        "                   so it is treated as uncapped\n"
        "  --hidden         start put away; --show starts her visible\n"
        "  --no-persist     do not read or write state.json\n"
        "  --no-control     no lock and no control socket, so a second copy can\n"
        "                   run beside a live one (debugging only)\n"
        "  --foreground     `start` only: stay in this terminal\n"
        "  -h, --help       this message\n"
        "\n"
        "Drag her along the bottom edge with the left button; scroll over her to\n"
        "resize; click a part of her for a reaction; right-click for the menu.\n"
        "\n"
        "Which setting wins: a flag above, then what you last did by hand\n"
        "(position, size, outfit, layer, output - in state.json), then the config\n"
        "file, then the built-in default. `asuna config init` writes a commented\n"
        "file with every setting in it, including how talkative she is.\n");
}

}  // namespace cli
}  // namespace asuna
