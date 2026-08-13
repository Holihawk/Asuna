#include "app/cli.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/argparse.hpp"
#include "app/config.hpp"
#include "app/daemon.hpp"
#include "app/ipc.hpp"
#include "pet/outfits.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;

namespace asuna {
namespace cli {
namespace {

// How long `start` waits for the socket to answer, and how long `exit` waits
// for the process to actually be gone before escalating.
constexpr int kStartTimeoutMs = 10000;
constexpr int kTermTimeoutMs = 5000;
constexpr int kKillTimeoutMs = 2000;

// How the middle process of `ext start`'s double fork reports back. Its exit
// status is all it has: it is the one process that knows the helper's pid, and
// everything it does happens on the other side of a fork from the caller.
constexpr int kStagedOk = 0;
constexpr int kStagedNoFork = 1;
constexpr int kStagedNoPidFile = 2;
constexpr int kStagedCleanupFailed = 3;
constexpr int kStagedNoGate = 4;

bool gJson = false;   // --json: print the reply line instead of a summary

int nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int complain(const std::string& message, int status = kError) {
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

// --- talking to the daemon --------------------------------------------------

// Sends one command. Prints whatever went wrong, so callers only ever handle
// success; `status` comes back as the exit code to use.
bool send(const std::string& cmd, const std::string& args, Json* data, int* status,
          bool quietWhenAbsent = false) {
    std::string reply, error;
    if (!ipc::call(paths::socketPath(), ipc::request(cmd, args), &reply, &error)) {
        // "not running" from the socket is only half an answer: a daemon whose
        // socket is gone but whose lock is held is wedged, not absent, and
        // saying "not running" would send the user looking in the wrong place.
        if (error == "not running" && daemon::holderPid(paths::lockPath()) > 0) {
            error = "the control socket is not answering, but a daemon is running"
                    " (`asuna exit` will signal it)";
        } else if (error == "not running") {
            *status = kNotRunning;
            // `asuna status` says it better, on stdout, where the answer to a
            // question belongs.
            if (quietWhenAbsent) return false;
        }
        fprintf(stderr, "asuna: %s\n", error.c_str());
        return false;
    }
    if (gJson) printf("%s\n", reply.c_str());
    std::string parseError;
    const Json parsed = Json::parse(reply, &parseError);
    if (!parsed.isObject())
        return !complain("unreadable reply: " + (parseError.empty() ? reply : parseError));
    // Handed back before the verdict, because a refusal can carry detail too -
    // `config reload` sends the list of problems with it, and making the caller
    // ask again for a state the daemon has already computed would be asking a
    // question it has already answered.
    if (data) *data = parsed["data"];
    if (!parsed["ok"].asBool()) {
        const std::string why = parsed["error"].asString();
        fprintf(stderr, "asuna: %s\n", why.empty() ? "the command failed" : why.c_str());
        return false;
    }
    return true;
}

// The common shape: send it, say nothing if it worked. Most verbs are this.
int simple(const std::string& cmd, const std::string& args = "") {
    int status = kError;
    Json data;
    return send(cmd, args, &data, &status) ? kOk : status;
}

// --- options ----------------------------------------------------------------

// Reads the config file into `opt` before a single flag is looked at, which is
// the whole of how "a flag beats the config file" is implemented: the flags are
// parsed straight over the top of it.
//
// A broken config refuses to start rather than starting wrong. Everything else
// here treats a missing piece as "use the default", but a config file is
// something the user wrote on purpose and is watching for the effect of, and
// twenty minutes of wondering why line 12 does nothing is worse than an error.
// The other half of the same argument, for the settings that parse and are in
// range and still do nothing because another one overrides them. Said, and then
// carried on from - see the note on Config::warnings for why these are not
// problems. Printed under the path so it reads the same as a complaint does.
void sayWarnings(const Config& cfg) {
    if (cfg.warnings.empty()) return;
    fprintf(stderr, "asuna: %s\n", cfg.source.empty() ? Config::path().c_str()
                                                      : cfg.source.c_str());
    for (const std::string& w : cfg.warnings) fprintf(stderr, "  %s\n", w.c_str());
}

int applyConfig(ShellOptions* opt) {
    Config cfg;
    cfg.load(Config::path());
    if (!cfg.problems.empty()) {
        fprintf(stderr, "asuna: %s\n", Config::path().c_str());
        for (const std::string& p : cfg.problems) fprintf(stderr, "  %s\n", p.c_str());
        return kUsage;
    }
    sayWarnings(cfg);
    cfg.applyTo(opt);
    return kOk;
}

// Fills `opt` from argv[from..]. Returns kOk, or kUsage having said why.
//
// The numeric flags are read through argparse rather than atoi/atof, and the
// ranges are the ones config.cpp already enforces on the same settings: a
// `strip.height` of 40 is refused in the config file, so `--height 40` has no
// business being accepted here. The difference is what happens next - the
// config file reports the problem and carries on with the default, because the
// rest of the file is still worth reading, whereas a flag is the only thing the
// user just asked for and getting it wrong should stop the launch.
int parseOptions(int argc, char** argv, int from, ShellOptions* opt, bool* foreground) {
    for (int i = from; i < argc; ++i) {
        const std::string a = argv[i];
        bool missing = false;
        std::string problem;
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                missing = true;
                return "";
            }
            return argv[++i];
        };
        // Both readers leave `opt` alone and set `problem` when the text is not
        // a value, so a rejected flag cannot half-apply. `missing` is checked
        // after the chain, and wins: "--height needs a value" beats complaining
        // that the empty string is not a number.
        auto whole = [&](int lo, int hi, int fallback) -> int {
            const std::string v = next();
            int n = fallback;
            if (!missing) argparse::integerIn(a, v, lo, hi, &n, &problem);
            return n;
        };
        auto fraction = [&](double lo, double hi, double fallback) -> double {
            const std::string v = next();
            double n = fallback;
            if (!missing) argparse::realIn(a, v, lo, hi, &n, &problem);
            return n;
        };
        // A floor and no ceiling. Distinct from fraction(lo, kNoMax, ...),
        // which is a two-sided range that happens to end at INT_MAX.
        auto atLeast = [&](double lo, double fallback) -> double {
            const std::string v = next();
            double n = fallback;
            if (!missing) argparse::realAtLeast(a, v, lo, &n, &problem);
            return n;
        };
        if (a == "--model") opt->model = resolveModelArg(next());
        else if (a == "--layer") opt->layer = next();
        else if (a == "--output") opt->output = next();
        else if (a == "--framing") opt->framing = next();
        else if (a == "--anchor") opt->anchor = next();
        else if (a == "--height") opt->stripHeight = whole(80, argparse::kNoMax, opt->stripHeight);
        else if (a == "--max-height") opt->maxHeight = whole(80, argparse::kNoMax, opt->maxHeight);
        else if (a == "--margin") opt->margin = whole(0, argparse::kNoMax, opt->margin);
        else if (a == "--bottom") opt->bottomMargin = whole(0, argparse::kNoMax, opt->bottomMargin);
        else if (a == "--language") opt->language = next();
        else if (a == "--pad") opt->pad = whole(0, argparse::kNoMax, opt->pad);
        else if (a == "--band") opt->bubbleBand = whole(0, argparse::kNoMax, opt->bubbleBand);
        else if (a == "--side")
            opt->sideBleed = static_cast<float>(fraction(0.0, 1.0, opt->sideBleed));
        else if (a == "--gaze-halo") opt->gazeHalo = whole(0, argparse::kNoMax, opt->gazeHalo);
        else if (a == "--no-greet") opt->greet = false;
        else if (a == "--scale") opt->scale = static_cast<float>(fraction(0.5, 2.5, opt->scale));
        else if (a == "--x") opt->x = atLeast(0.0, opt->x);
        else if (a == "--fps") opt->fps = whole(0, argparse::kNoMax, opt->fps);
        else if (a == "--hidden") opt->hidden = 1;
        else if (a == "--show") opt->hidden = 0;
        else if (a == "--no-persist") opt->persist = false;
        else if (a == "--no-control") opt->control = false;
        else if (a == "--foreground" && foreground) *foreground = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return -1;   // handled, exit 0
        } else {
            fprintf(stderr, "asuna: unknown argument '%s'\n", a.c_str());
            return kUsage;
        }
        if (missing) {
            fprintf(stderr, "asuna: %s needs a value\n", a.c_str());
            return kUsage;
        }
        if (!problem.empty()) {
            fprintf(stderr, "asuna: %s\n", problem.c_str());
            return kUsage;
        }
    }
    return kOk;
}

// --- start / exit -----------------------------------------------------------

// Blocks until nobody holds the lock, or the deadline passes. Returns whether
// it went away.
bool waitForExit(int timeoutMs) {
    const int deadline = nowMs() + timeoutMs;
    while (nowMs() < deadline) {
        if (daemon::holderPid(paths::lockPath()) <= 0) return true;
        usleep(50 * 1000);
    }
    return daemon::holderPid(paths::lockPath()) <= 0;
}

// A fast, clear refusal, and the one exit status a script can act on. Not the
// real gate - the daemon takes the lock itself, which is the only check that
// cannot lose a race - but it means the usual case does not go through a fork
// and a ten-second wait to find out something visible from here.
int refuseIfRunning(const ShellOptions& opt) {
    if (!opt.control) return kOk;
    const pid_t holder = daemon::holderPid(paths::lockPath());
    if (holder <= 0) return kOk;
    fprintf(stderr, "asuna: already running (pid %d)\n", holder);
    return kAlreadyRunning;
}

int cmdStart(int argc, char** argv, int from, RunShell runShell) {
    ShellOptions opt;
    bool foreground = false;
    if (const int bad = applyConfig(&opt); bad != kOk) return bad;
    const int parsed = parseOptions(argc, argv, from, &opt, &foreground);
    if (parsed != kOk) return parsed < 0 ? kOk : parsed;

    if (const int busy = refuseIfRunning(opt); busy != kOk) return busy;
    if (foreground) return runShell(std::move(opt));

    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0) return complain(std::string("pipe: ") + strerror(errno));

    const pid_t child = fork();
    if (child < 0) return complain(std::string("fork: ") + strerror(errno));

    if (child == 0) {
        close(ready[0]);
        // Twice, in the usual way: setsid() makes the middle process a session
        // leader with no controlling terminal, and forking again means the
        // process that actually survives is not a session leader, so it can
        // never acquire one by opening a tty.
        setsid();
        const pid_t grandchild = fork();
        if (grandchild < 0) _exit(1);
        if (grandchild > 0) _exit(0);

        std::string error;
        if (!daemon::redirectOutput(paths::logPath(), &error)) {
            const std::string line = "err: " + error + "\n";
            [[maybe_unused]] const ssize_t n = write(ready[1], line.data(), line.size());
            _exit(1);
        }
        opt.readyFd = ready[1];
        _exit(runShell(std::move(opt)));
    }

    close(ready[1]);
    // The middle process exits immediately; reaping it here is what keeps a
    // zombie from being handed to init.
    int ignored = 0;
    waitpid(child, &ignored, 0);

    std::string answer;
    int remaining = kStartTimeoutMs;
    while (remaining > 0 && answer.find('\n') == std::string::npos) {
        pollfd p = {ready[0], POLLIN, 0};
        const int before = nowMs();
        const int r = poll(&p, 1, remaining);
        remaining -= nowMs() - before;
        if (r <= 0) break;
        char buf[256];
        const ssize_t n = read(ready[0], buf, sizeof(buf));
        if (n <= 0) break;   // EOF: it died before it could tell us anything
        answer.append(buf, static_cast<size_t>(n));
    }
    close(ready[0]);

    if (answer.compare(0, 4, "err:") == 0) {
        const size_t eol = answer.find('\n');
        return complain(answer.substr(5, eol == std::string::npos ? eol : eol - 5));
    }
    if (answer.empty()) {
        fprintf(stderr, "asuna: it did not come up. The last of %s:\n\n",
                paths::logPath().c_str());
        const std::string tail = daemon::tailLog(paths::logPath(), 20);
        fputs(tail.empty() ? "  (the log is empty)\n" : tail.c_str(), stderr);
        return kError;
    }

    // The pipe already proves the socket is listening; the ping proves it
    // answers, and is where the pid comes from.
    Json data;
    int status = kError;
    if (!send("ping", "", &data, &status)) return status;
    printf("asuna: started (pid %d), logging to %s\n",
           static_cast<int>(data["pid"].asNumber()), paths::logPath().c_str());
    return kOk;
}

int cmdExit() {
    int status = kError;
    Json data;
    if (send("exit", "", &data, &status)) {
        // She takes a moment over the goodbye, and `asuna exit && asuna start`
        // has to work, so wait for the lock to come free rather than for the
        // reply. Not an error if it outlasts the wait - it is on its way out.
        waitForExit(kTermTimeoutMs);
        return kOk;
    }

    const pid_t pid = daemon::holderPid(paths::lockPath());
    if (pid <= 0) return kNotRunning;   // send() has already said "not running"

    // The socket did not take it, but something is holding the lock. "Exit"
    // has to mean exit.
    fprintf(stderr, "asuna: signalling pid %d\n", pid);
    kill(pid, SIGTERM);
    if (waitForExit(kTermTimeoutMs)) return kOk;
    fprintf(stderr, "asuna: it ignored SIGTERM; killing pid %d\n", pid);
    kill(pid, SIGKILL);
    if (waitForExit(kKillTimeoutMs)) return kOk;
    return complain("it is still there after SIGKILL");
}

// --- status -----------------------------------------------------------------

std::string humanTime(long seconds) {
    char buf[64];
    if (seconds < 60) snprintf(buf, sizeof(buf), "%lds", seconds);
    else if (seconds < 3600) snprintf(buf, sizeof(buf), "%ldm %02lds", seconds / 60, seconds % 60);
    else snprintf(buf, sizeof(buf), "%ldh %02ldm", seconds / 3600, (seconds / 60) % 60);
    return buf;
}

int cmdStatus() {
    int status = kError;
    Json d;
    if (!send("status", "", &d, &status, /*quietWhenAbsent=*/true)) {
        if (status == kNotRunning) printf("asuna: not running\n");
        return status;
    }
    if (gJson) return kOk;   // send() already printed the reply

    const bool hidden = d["hidden"].asBool();
    const bool ready = d["ready"].asBool();
    printf("asuna: running (pid %d), up %s%s\n", static_cast<int>(d["pid"].asNumber()),
           humanTime(static_cast<long>(d["uptime_s"].asNumber())).c_str(),
           hidden ? ", hidden" : (ready ? "" : ", still starting"));
    if (ready) {
        printf("  outfit    %s   %s\n", d["model"].asString().c_str(),
               d["model_path"].asString().c_str());
        printf("  placed    x=%.0f  scale %.2f  layer %s  output %s\n", d["x"].asNumber(),
               d["scale"].asNumber(), d["layer"].asString().c_str(),
               d["output"].asString().empty() ? "(default)" : d["output"].asString().c_str());
        printf("  strip     %.0fx%.0f  fps cap %.0f\n", d["strip_width"].asNumber(),
               d["strip_height"].asNumber(), d["fps"].asNumber());
    } else {
        // Nothing is loaded yet, so the position and the strip are zeroes that
        // mean "not measured", not "measured and zero". Saying so beats
        // printing them and letting the reader decide something is broken.
        printf("  %s\n", hidden ? "no model loaded - it goes up when she does"
                                : "no model loaded yet");
    }
    printf("  memory    %.1f MB resident\n", d["rss_kb"].asNumber() / 1024.0);
    const std::string config = d["config"].asString();
    printf("  config    %s\n", config.empty() ? "none - all defaults" : config.c_str());
    if (d["asleep"].asBool()) printf("  she is asleep\n");
    return kOk;
}

// --- the outfit registry ----------------------------------------------------

int cmdModel(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "list";
    if (verb == "use") {
        if (from + 1 >= argc) return complain("model use needs an outfit id", kUsage);
        return simple("model", ipc::Out().str("use", argv[from + 1]).done());
    }
    if (verb != "list") return complain("model takes `list` or `use <id>`", kUsage);

    int status = kError;
    Json d;
    if (!send("model", ipc::Out().boolean("list", true).done(), &d, &status)) return status;
    if (gJson) return kOk;

    const std::string current = d["current"].asString();
    const Json& models = d["models"];
    const Json& fullBody = d["full_body"];
    std::vector<std::string> full;
    for (size_t i = 0; i < fullBody.size(); ++i) full.push_back(fullBody[i].asString());

    printf("asuna: %zu outfits in %s\n", models.size(), d["root"].asString().c_str());
    for (size_t i = 0; i < models.size(); ++i) {
        const std::string id = models[i].asString();
        // A dot on the one she is wearing: 42 near-identical two-digit ids are
        // otherwise a wall, which is the same reason the menu marks it. A `+`
        // marks the full-body costumes, which are the ones that will change the
        // height of the strip when you put one on.
        const bool isFull = std::find(full.begin(), full.end(), id) != full.end();
        printf("%s%s%s%-2s", i % 12 == 0 ? "  " : " ", id == current ? "\xc2\xb7" : " ",
               id.c_str(), isFull ? "+" : "");
        if (i % 12 == 11 || i + 1 == models.size()) printf("\n");
    }
    if (!full.empty())
        printf("  \xc2\xb7 wearing now   + full body (%zu of them; taller strip)\n", full.size());
    return kOk;
}

int cmdNamed(const std::string& cmd, int argc, char** argv, int from) {
    const bool list = from < argc && (strcmp(argv[from], "--list") == 0 ||
                                      strcmp(argv[from], "list") == 0);
    if (from >= argc || list) {
        int status = kError;
        Json d;
        if (!send(cmd, ipc::Out().boolean("list", true).done(), &d, &status)) return status;
        if (gJson) return kOk;
        const Json& names = d[cmd == "motion" ? "motions" : "expressions"];
        for (size_t i = 0; i < names.size(); ++i) printf("%s\n", names[i].asString().c_str());
        return kOk;
    }
    return simple(cmd, ipc::Out().str("name", argv[from]).done());
}

// --- monitors ---------------------------------------------------------------

int cmdOutput(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "list";
    if (verb != "list") return simple("output", ipc::Out().str("name", verb).done());

    int status = kError;
    Json d;
    if (!send("output", ipc::Out().boolean("list", true).done(), &d, &status)) return status;
    if (gJson) return kOk;

    const std::string current = d["current"].asString();
    const std::string preferred = d["preferred"].asString();
    const Json& outputs = d["outputs"];
    for (size_t i = 0; i < outputs.size(); ++i) {
        const std::string line = outputs[i].asString();
        const std::string name = line.substr(0, line.find(' '));
        printf(" %s %s\n", name == current ? "\xc2\xb7" : " ", line.c_str());
    }
    // The distinction only shows up when a cable has been pulled, and that is
    // exactly when it is worth seeing: she is on this screen, but she is still
    // waiting for that one.
    if (preferred.empty())
        printf("asuna: no output preference - she takes the first one\n");
    else if (preferred != current)
        printf("asuna: she wants %s, which is not plugged in\n", preferred.c_str());
    else
        printf("asuna: pinned to %s\n", preferred.c_str());
    return kOk;
}

// --- the config file --------------------------------------------------------

int cmdConfig(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "path";
    const std::string path = Config::path();
    std::error_code ec;

    if (verb == "path") {
        printf("%s%s\n", path.c_str(), fs::exists(path, ec) ? "" : "   (does not exist)");
        return kOk;
    }
    if (verb == "init") {
        const bool force = from + 1 < argc && strcmp(argv[from + 1], "--force") == 0;
        if (fs::exists(path, ec) && !force)
            return complain(path + " already exists (--force overwrites it)");
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        if (!f) return complain("could not write " + path);
        f << Config::defaultText();
        if (!f) return complain("could not write " + path);
        f.close();
        // 0600, because this file can now legitimately hold an API key -
        // `[ext.provider.<name>] api_key`. It did not need protecting when the
        // worst it held was how often she blinks.
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        printf("asuna: wrote %s - every setting at its default, so it changes nothing\n",
               path.c_str());
        return kOk;
    }
    if (verb == "show") {
        std::ifstream f(path);
        if (!f) {
            printf("# no config file at %s; these are the defaults\n%s", path.c_str(),
                   Config::defaultText().c_str());
            return kOk;
        }
        printf("%s", std::string(std::istreambuf_iterator<char>(f), {}).c_str());
        return kOk;
    }
    if (verb == "check") {
        Config cfg;
        cfg.load(path);

        // Everything this verb has to say that is not fatal, worked out once
        // and then printed in whichever form was asked for. Two branches each
        // computing their own diagnostics is exactly how the JSON form came to
        // be missing the one below about file permissions, and how the two
        // forms came to disagree about a security-relevant fact.
        //
        // A superset of Config::warnings, which is only ever about the file's
        // *contents*. This is `config check`'s whole answer, and the mode of
        // the file is part of it.
        std::vector<std::string> warnings = cfg.warnings;
        bool literalKey = false;
        for (const auto& p : cfg.ext.providers) literalKey |= !p.apiKey.empty();
        if (literalKey && !cfg.source.empty()) {
            // Worth saying whether or not the file also has a problem in it -
            // and it used to be skipped entirely when it did. A key in a
            // world-readable file is a key anyone with an account on the
            // machine can read, and a typo on line 12 does not make that
            // wait its turn.
            const fs::perms mode = fs::status(path, ec).permissions();
            if ((mode & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none)
                warnings.push_back("it holds an API key and is readable by others -"
                                   " `chmod 600 " + path + "`");
        }

        // The only verdict here that is not the daemon's, so the only one that
        // has to build its own reply line. Same envelope as every other --json
        // answer, because `asuna --json <anything> | jq .ok` should mean one
        // thing: `ok` is false exactly when the exit status is non-zero, and
        // `warnings` is always present, empty or not, so a reader never has to
        // tell "no warnings" from "this build had none to give".
        if (gJson) {
            ipc::Out d;
            d.str("path", path);
            d.boolean("exists", !cfg.source.empty());
            d.raw("problems", ipc::Out::array(cfg.problems));
            d.raw("warnings", ipc::Out::array(warnings));
            const std::string body = d.done();
            if (cfg.problems.empty()) {
                printf("%s\n", ipc::ok(body).c_str());
                return kOk;
            }
            printf("%s\n", ipc::fail(std::to_string(cfg.problems.size()) +
                                     " problem(s) in " + path, body).c_str());
            return kError;
        }
        if (cfg.source.empty()) {
            printf("asuna: no config file at %s - all defaults, which is valid\n",
                   path.c_str());
            return kOk;
        }
        if (cfg.problems.empty()) {
            // "fine" only when it is. A file whose max_height does nothing is
            // not broken - it starts, and it behaves the way the warning says -
            // but telling somebody it is fine is how they stop looking.
            if (warnings.empty()) {
                printf("asuna: %s is fine\n", path.c_str());
            } else {
                printf("asuna: %s parses, with %zu thing(s) worth knowing\n",
                       path.c_str(), warnings.size());
                for (const std::string& w : warnings) printf("  %s\n", w.c_str());
            }
            return kOk;
        }
        fprintf(stderr, "asuna: %s\n", path.c_str());
        for (const std::string& p : cfg.problems) fprintf(stderr, "  %s\n", p.c_str());
        for (const std::string& w : warnings) fprintf(stderr, "  %s\n", w.c_str());
        return kError;
    }
    if (verb == "edit") {
        const char* editor = getenv("EDITOR");
        if (!editor || !*editor) editor = getenv("VISUAL");
        if (!editor || !*editor) return complain("$EDITOR is not set");
        if (!fs::exists(path, ec)) {
            fs::create_directories(fs::path(path).parent_path(), ec);
            std::ofstream f(path, std::ios::trunc);
            if (f) f << Config::defaultText();
        }
        // execlp, not system(): no shell means no quoting question about a path
        // with a space in it, and the editor inherits this terminal as it is.
        execlp(editor, editor, path.c_str(), nullptr);
        return complain(std::string("could not run ") + editor + ": " + strerror(errno));
    }
    if (verb == "reload") {
        int status = kError;
        Json d;
        if (!send("config", "", &d, &status)) {
            // The daemon sent the problem list along with the refusal, so the
            // reason is already here rather than a `config check` away. The
            // warnings come with it for the same reason - both were known at
            // the same moment, and one edit should be able to fix both.
            const Json& problems = d["problems"];
            for (size_t i = 0; i < problems.size(); ++i)
                fprintf(stderr, "  %s\n", problems[i].asString().c_str());
            const Json& warnings = d["warnings"];
            for (size_t i = 0; i < warnings.size(); ++i)
                fprintf(stderr, "  %s\n", warnings[i].asString().c_str());
            return status;
        }
        if (!gJson) {
            const std::string where = d["path"].asString();
            printf("asuna: reloaded %s\n", where.c_str());
            printf("       outfit, size, position, layer and output are not touched -\n"
                   "       those are state.json's, and it outranks the config file\n");
            const std::string note = d["note"].asString();
            if (!note.empty()) printf("       note: %s\n", note.c_str());
            // It reloaded, and some of what was reloaded does nothing. On
            // stderr rather than with the success above, because it is the one
            // part of this output somebody might want to grep for.
            const Json& warnings = d["warnings"];
            for (size_t i = 0; i < warnings.size(); ++i)
                fprintf(stderr, "  %s\n", warnings[i].asString().c_str());
        }
        return kOk;
    }
    return complain("config takes path, show, init, check, edit or reload", kUsage);
}

// --- extensions -------------------------------------------------------------
//
// The helper is a separate program that subscribes to her events and drives her
// through the same socket every verb here uses. All this end does is start it,
// stop it and say whether it is there - it is deliberately not supervised, not
// restarted and not talked to: if it dies, she carries on exactly as she does
// with it switched off, which is the whole point of it being out of process.

// What to run for the helper: `[ext] command` if the config names one, and the
// bundled asuna-ext.py if it does not. Both `ext start` and `ext test` need
// exactly this, and used to carry their own copy of the tokenizer - two copies
// of a splitter is two places for a quoting rule to be almost the same.
int extArgv(const Config& cfg, std::vector<std::string>* argv) {
    if (cfg.ext.command.empty()) {
        const std::string script = paths::extHelper();
        if (script.empty())
            return complain("cannot find asuna-ext.py - set `command` under [ext], or run "
                            "install.sh so it sits beside the binary");
        *argv = {"python3", script};
        return kOk;
    }
    std::string problem;
    if (!argparse::splitCommand(cfg.ext.command, argv, &problem))
        return complain("[ext] command: " + problem, kUsage);
    if (argv->empty()) return complain("[ext] command is empty");
    return kOk;
}

// The helper's pid, if the pid file still names the helper.
//
// Unlike the daemon's lock this really is a pid file, because the helper is
// somebody else's program and cannot be made to hold a lock for us. What makes
// the file trustworthy is the start time recorded beside the pid: a reused pid
// has a different one, so `ext stop` cannot signal a stranger that happened to
// inherit the number. See daemon::readPidFile.
//
// A recycled or stale file is said out loud rather than passed off as "not
// running", because those are different things to whoever is reading: one means
// the helper stopped, the other means it stopped *and* something else is now
// wearing its number.
daemon::Identity extIdentity(bool quiet = false) {
    const daemon::Identity id = daemon::readPidFile(paths::extPidPath());
    if (id.state == daemon::Owner::kRecycled && !quiet)
        fprintf(stderr, "asuna: %s names pid %d, which is now a different process -"
                        " the helper is gone\n",
                paths::extPidPath().c_str(), static_cast<int>(id.pid));
    return id;
}

// The pid to *report*, which is a lower bar than the pid to signal: an old pid
// file that cannot prove which process it names still answers "something is
// there", and saying "not running" about a helper that plainly is would be
// worse than saying so with a caveat. Nothing here signals - see cmdExtStop.
pid_t extPid(bool quiet = false) {
    const daemon::Identity id = extIdentity(quiet);
    return id.present() ? id.pid : 0;
}

int cmdExtStart(const Config& cfg) {
    if (!cfg.ext.enabled)
        return complain("extensions are off - set `enabled = true` under [ext] in " +
                        Config::path() + " (`asuna config edit`)");
    if (!ipc::alive(paths::socketPath()))
        return complain("she is not running - `asuna start` first", kNotRunning);
    if (const pid_t running = extPid())
        return complain("the helper is already running (pid " + std::to_string(running) + ")",
                        kAlreadyRunning);

    std::vector<std::string> argv;
    if (const int bad = extArgv(cfg, &argv); bad != kOk) return bad;

    const pid_t child = fork();
    if (child < 0) return complain(std::string("fork: ") + strerror(errno));
    if (child == 0) {
        // The same double fork `start` does, and for the same reason: nothing
        // in this terminal should own the helper, and the terminal closing must
        // not take it with it.
        setsid();
        int gate[2];
        if (pipe2(gate, O_CLOEXEC) != 0) _exit(kStagedNoGate);
        const pid_t grandchild = fork();
        if (grandchild < 0) {
            close(gate[0]);
            close(gate[1]);
            _exit(kStagedNoFork);
        }
        if (grandchild > 0) {
            close(gate[0]);
            // The middle process is the one that knows the surviving pid. It
            // also records when that pid started, which is what stops the file
            // from naming a stranger later; the write goes through a temporary
            // and a rename, so `ext status` running at this instant sees either
            // the old file or the new one and never half a line.
            //
            // daemon::writePidFile finishes before _exit, which does not run
            // destructors and therefore does not flush a stream - a plain
            // ofstream here once left a pid file of exactly zero bytes and an
            // `ext status` that could see the helper on the socket but not in
            // the process table.
            //
            // Nothing else can write that file: the original parent is on the
            // other side of a fork and never learns this pid. The grandchild
            // waits behind `gate` and cannot exec the configured program until
            // the identity is durable. Closing the gate without a byte makes it
            // exit, so failure is transactional without trying to kill an
            // external program that may already have changed its credentials.
            if (!daemon::writePidFile(paths::extPidPath(), grandchild)) {
                close(gate[1]);
                int stopped = 0;
                while (waitpid(grandchild, &stopped, 0) < 0) {
                    if (errno == EINTR) continue;
                    if (errno != ECHILD) _exit(kStagedCleanupFailed);
                    break;
                }
                _exit(kStagedNoPidFile);
            }
            // A child that failed before reading the gate closes its end. Keep
            // EPIPE as an ordinary error so the pid file is removed below;
            // default SIGPIPE would terminate this middle process first and
            // leave a durable identity for a helper that never started.
            signal(SIGPIPE, SIG_IGN);
            const char go = 1;
            ssize_t sent = 0;
            do {
                sent = write(gate[1], &go, sizeof(go));
            } while (sent < 0 && errno == EINTR);
            close(gate[1]);
            if (sent != sizeof(go)) {
                std::error_code ec;
                fs::remove(paths::extPidPath(), ec);
                int stopped = 0;
                while (waitpid(grandchild, &stopped, 0) < 0 && errno == EINTR) {}
                _exit(kStagedCleanupFailed);
            }
            _exit(kStagedOk);
        }
        close(gate[1]);
        char go = 0;
        ssize_t received = 0;
        do {
            received = read(gate[0], &go, sizeof(go));
        } while (received < 0 && errno == EINTR);
        close(gate[0]);
        if (received != sizeof(go) || go != 1) _exit(1);
        std::string error;
        if (!daemon::redirectOutput(paths::extLogPath(), &error)) _exit(1);
        std::vector<char*> args;
        for (std::string& a : argv) args.push_back(a.data());
        args.push_back(nullptr);
        execvp(args[0], args.data());
        fprintf(stderr, "asuna: could not run %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    int staged = 0;
    while (waitpid(child, &staged, 0) < 0) {
        if (errno == EINTR) continue;
        return complain(std::string("could not wait for helper startup: ") + strerror(errno));
    }
    // The middle process's exit status is the only thing it can send back, and
    // it used to be discarded. A helper the pid file could not name would then
    // be reported as started, and only stop working later - at the point where
    // somebody wanted to stop it.
    switch (WIFEXITED(staged) ? WEXITSTATUS(staged) : -1) {
        case kStagedOk:
            break;
        case kStagedNoFork:
            return complain("could not fork the helper");
        case kStagedNoPidFile:
            return complain("could not write " + paths::extPidPath() +
                            " - the helper has been stopped again rather than left running"
                            " under a pid nothing could identify");
        case kStagedCleanupFailed:
            return complain("could not release the helper startup gate or confirm cleanup");
        case kStagedNoGate:
            return complain("could not create the helper startup gate");
        default:
            return complain("the helper could not be started");
    }

    // Started is not connected. Wait for it to actually subscribe, because
    // "it launched and then died on an import error" and "it is running" look
    // identical from here otherwise, and the difference is in its log.
    const int deadline = nowMs() + 5000;
    while (nowMs() < deadline) {
        Json d;
        int status = kError;
        if (send("ext", ipc::Out().boolean("status", true).done(), &d, &status) &&
            d["subscribers"].asNumber() > 0) {
            printf("asuna: helper running (pid %d), logging to %s\n", extPid(),
                   paths::extLogPath().c_str());
            if (!cfg.ext.visionEnabled)
                printf("       vision is off; `ext.vision_enabled` in the config turns it on\n");
            return kOk;
        }
        usleep(100 * 1000);
    }
    fprintf(stderr, "asuna: the helper did not connect. The last of %s:\n\n",
            paths::extLogPath().c_str());
    const std::string tail = daemon::tailLog(paths::extLogPath(), 20);
    fputs(tail.empty() ? "  (the log is empty)\n" : tail.c_str(), stderr);
    return kError;
}

// The target could not be taken hold of safely, and not because it has gone:
// the kernel would not open a handle, or /proc could not be read to confirm the
// identity - out of descriptors, out of memory. Try again is the honest advice,
// so the pid file is left exactly as it was: removing it would throw away the
// only record of what to stop, and signalling the bare pid instead would spend a
// safety property on a temporary shortage, which is when it is needed most.
int cannotOpen(const daemon::Signal& target, pid_t pid) {
    return complain("could not get a safe hold on pid " + std::to_string(pid) + ": " +
                    strerror(target.error()) + " - nothing has been signalled, and " +
                    paths::extPidPath() + " has been left as it is");
}

// A signal that did not arrive at a process that is still there. Whatever the
// reason - permission, resources, a syscall that is not available - the helper
// is still running, so the file that names it stays: reporting a stop that did
// not happen *and* deleting the only way to try again is the worst of both.
int cannotSignal(pid_t pid, int err) {
    return complain("could not signal pid " + std::to_string(pid) + ": " + strerror(err) +
                    " - it could not be confirmed stopped, so " + paths::extPidPath() +
                    " has been left as it is");
}

// The signal was delivered, but the later liveness check itself failed. Keep
// this separate from cannotSignal(): saying the signal failed here sends the
// operator looking at the wrong syscall and hides the state we actually know.
int cannotConfirmStopped(pid_t pid, int err) {
    return complain("could not confirm whether pid " + std::to_string(pid) + " stopped: " +
                    strerror(err) + " - " + paths::extPidPath() +
                    " has been left as it is");
}

int stillAlive(pid_t pid) {
    return complain("pid " + std::to_string(pid) + " is still present after SIGKILL - " +
                    paths::extPidPath() + " has been left as it is");
}

int cmdExtStop(bool force = false) {
    // Loud here of all places: this is the verb that sends the signal, so a pid
    // file naming somebody else is the exact thing the caller needs told before
    // being reassured that nothing was running.
    const daemon::Identity id = extIdentity();
    std::error_code ec;
    if (!id.present()) {
        fs::remove(paths::extPidPath(), ec);
        return complain("the helper is not running", kOk);
    }

    // A handle to the process, not to the number, and *every* signal below goes
    // through it. The first one used to go out on the strength of a check made
    // before it, which is a window: the helper can exit and its pid be reissued
    // between deciding and killing. Opening once and signalling through the
    // handle removes that, including for the SIGKILL five seconds later.
    daemon::Signal target;
    if (!target.open(id)) {
        // *Why* it refused decides what may happen next, and only one of the
        // three refusals leads anywhere near an unproven signal. Treating them
        // alike is how a normal stop could still reach a stranger: a proven pid
        // file whose helper exited in the moment between reading it and opening
        // it fell through to the forced path, which then pinned - precisely and
        // safely - whoever had inherited the number.
        if (target.refusal() == daemon::Signal::Refusal::kUnavailable)
            return cannotOpen(target, id.pid);

        if (id.state != daemon::Owner::kUnverified) {
            // The file proved itself when it was read and cannot now: the helper
            // left in between. It is already stopped, so the file goes - but
            // nothing is signalled, forced or otherwise, because that pid has
            // been *shown* to have moved on and --force cannot unshow it.
            fs::remove(paths::extPidPath(), ec);
            if (daemon::startTime(id.pid) != 0)
                fprintf(stderr,
                        "asuna: the helper left while it was being stopped, and pid %d is now"
                        " a\n       different process - nothing has been signalled\n",
                        static_cast<int>(id.pid));
            return complain("the helper is not running", kOk);
        }
        if (!force) {
            // Something is running under that number, and nothing here can show
            // it is the helper: the file predates start times. `status` may be
            // generous about that; this verb may not, because being wrong here
            // means signalling a stranger. The pid is named so that checking it
            // by hand is one command away.
            fprintf(stderr,
                    "asuna: %s was written by an older version and does not record which\n"
                    "       process it means. Something is running as pid %d, but it cannot be\n"
                    "       shown to be her helper, so nothing has been signalled.\n"
                    "       Check it with `ps -p %d -o args=`, then either `asuna ext stop\n"
                    "       --force`, or delete that file if it is not her helper.\n",
                    paths::extPidPath().c_str(), static_cast<int>(id.pid),
                    static_cast<int>(id.pid));
            return kError;
        }
        // --force, and only ever on the one identity it is for: a file from a
        // build that recorded no start time, on a pid that is still live. The
        // user has said they checked.
        if (!target.openUnproven(id.pid)) {
            if (target.refusal() == daemon::Signal::Refusal::kUnavailable)
                return cannotOpen(target, id.pid);
            fs::remove(paths::extPidPath(), ec);
            return complain("the helper is not running", kOk);
        }
        fprintf(stderr, "asuna: --force: signalling pid %d without having proved it is"
                        " her helper\n", static_cast<int>(id.pid));
    }

    // Both results are read. They used to be discarded, so a send that failed
    // still printed "helper stopped" and still deleted the pid file - the one
    // outcome where the helper is left running and the only record of it is
    // thrown away. A send that fails because the target has already gone is a
    // different matter and really is a stop, which is what `probe()` separates.
    const int pid = static_cast<int>(id.pid);
    int err = 0;
    if (!target.send(SIGTERM, &err)) {
        int probeError = 0;
        if (target.probe(&probeError) != daemon::Signal::Presence::kGone)
            return cannotSignal(id.pid, err ? err : probeError);
    }
    const int deadline = nowMs() + kTermTimeoutMs;
    daemon::Signal::Presence present = daemon::Signal::Presence::kAlive;
    while (nowMs() < deadline) {
        present = target.probe(&err);
        if (present != daemon::Signal::Presence::kAlive) break;
        usleep(50 * 1000);
    }
    if (present == daemon::Signal::Presence::kUnavailable)
        return cannotConfirmStopped(id.pid, err);
    if (present == daemon::Signal::Presence::kAlive) {
        fprintf(stderr, "asuna: it ignored SIGTERM; killing pid %d\n", pid);
        if (!target.send(SIGKILL, &err)) {
            int probeError = 0;
            if (target.probe(&probeError) != daemon::Signal::Presence::kGone)
                return cannotSignal(id.pid, err ? err : probeError);
        }

        const int killDeadline = nowMs() + kKillTimeoutMs;
        while (nowMs() < killDeadline) {
            present = target.probe(&err);
            if (present != daemon::Signal::Presence::kAlive) break;
            usleep(50 * 1000);
        }
        if (present == daemon::Signal::Presence::kUnavailable)
            return cannotConfirmStopped(id.pid, err);
        if (present == daemon::Signal::Presence::kAlive) return stillAlive(id.pid);
    }
    fs::remove(paths::extPidPath(), ec);
    printf("asuna: helper stopped\n");
    return kOk;
}

// Runs the helper in the foreground with --test, inheriting this terminal.
//
// It has to be the helper rather than anything here: probing a provider means
// making a request with its key, and the key is deliberately somewhere the
// daemon cannot see - in the environment of the process that does the asking.
// Which is this shell's environment, so `OPENAI_API_KEY=… asuna ext test`
// tests exactly what `OPENAI_API_KEY=… asuna ext start` would then use.
int cmdExtTest(const Config& cfg) {
    if (!ipc::alive(paths::socketPath()))
        return complain("she is not running - `asuna start` first", kNotRunning);

    std::vector<std::string> argv;
    if (const int bad = extArgv(cfg, &argv); bad != kOk) return bad;
    argv.push_back("--test");

    const pid_t child = fork();
    if (child < 0) return complain(std::string("fork: ") + strerror(errno));
    if (child == 0) {
        std::vector<char*> args;
        for (std::string& a : argv) args.push_back(a.data());
        args.push_back(nullptr);
        execvp(args[0], args.data());
        fprintf(stderr, "asuna: could not run %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : kError;
}

int cmdExt(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "status";

    // The verb first, so `ext bogus --force` is answered with the list of verbs
    // rather than with a complaint about the option. (`--json` never reaches
    // here: dispatch takes it out of argv, because it says how to print rather
    // than what to do.)
    const char* const known[] = {"start", "stop", "restart", "test",
                                 "status", "config", "cancel"};
    if (std::none_of(std::begin(known), std::end(known),
                     [&verb](const char* v) { return verb == v; }))
        return complain("ext takes start, stop, restart, test, status,"
                        " config or cancel", kUsage);

    // Then an exact grammar: `ext stop [--force]`, `ext restart [--force]`, and
    // the bare verb everywhere else. --force is the one option here that can end
    // a process the CLI was unable to identify, and scanning the whole argument
    // list for it accepted things that read as if they did something and did
    // not - `ext status --force`, `ext stop garbage`, both silently fine.
    //
    // --force only ever means "signal a pid whose identity the pid file cannot
    // prove". It skips no other check, and never applies to a file that has been
    // shown to name something else.
    bool force = false;
    for (int i = from + 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg != "--force")
            return complain("ext " + verb + " does not take '" + arg + "'", kUsage);
        if (verb != "stop" && verb != "restart")
            return complain("--force is only for `ext stop` and `ext restart`", kUsage);
        if (force) return complain("--force is given twice", kUsage);
        force = true;
    }

    if (verb == "start" || verb == "restart" || verb == "test") {
        Config cfg;
        cfg.load(Config::path());
        if (!cfg.problems.empty()) {
            fprintf(stderr, "asuna: %s\n", Config::path().c_str());
            for (const std::string& p : cfg.problems) fprintf(stderr, "  %s\n", p.c_str());
            return kUsage;
        }
        if (verb == "test") return cmdExtTest(cfg);
        // Quiet: cmdExtStart says the same thing a line later if the pid file
        // turns out to name a stranger, and hearing it twice from one command
        // reads like it happened twice.
        //
        // A stop that refuses ends the restart. Carrying on would run into
        // cmdExtStart's own "already running" a moment later, having replaced
        // the one message that explains what to do with one that does not.
        if (verb == "restart" && extPid(true) > 0) {
            if (const int stopped = cmdExtStop(force); stopped != kOk) return stopped;
        }
        return cmdExtStart(cfg);
    }
    if (verb == "stop") return cmdExtStop(force);
    if (verb == "cancel") return simple("ext", ipc::Out().boolean("cancel", true).done());
    if (verb == "config") {
        int status = kError;
        Json d;
        // No `secrets`, so a key written into the config file is not in this
        // reply at all - which is what makes `--json` safe to paste.
        if (!send("ext", ipc::Out().boolean("config", true).done(), &d, &status)) return status;
        if (gJson) return kOk;
        printf("  enabled        %s\n", d["enabled"].asBool() ? "yes" : "no");
        const Json& providers = d["providers"];
        if (providers.size() == 0)
            printf("  providers      none - add an [ext.provider.<name>] section\n");
        for (size_t i = 0; i < providers.size(); ++i) {
            const Json& p = providers[i];
            printf("  %-14s %s\n", i ? "" : "providers",
                   (p["name"].asString() + "  " + p["model"].asString()).c_str());
            printf("                 %s\n", p["base_url"].asString().c_str());
            // Three states worth telling apart, because "it does not work" has
            // a different answer in each: the key is in the config file, the
            // key is in a variable that is set here, or it is named and missing.
            const std::string var = p["api_key_env"].asString();
            if (p["api_key_here"].asBool())
                printf("                 key: in the config file\n");
            else if (var.empty())
                printf("                 key: none (a local model, presumably)\n");
            else
                printf("                 key: $%s, %s in this shell\n", var.c_str(),
                       getenv(var.c_str()) ? "set" : "NOT SET");
        }
        const Json& idle = d["idle_providers"];
        for (size_t i = 0; i < idle.size(); ++i)
            printf("  %-14s %s is defined but not in `providers`, so it is unused\n",
                   i ? "" : "unused", idle[i].asString().c_str());
        printf("  prompt         %s\n", d["prompt_command"].asString().empty()
                                            ? "her own window (asuna-prompt.py)"
                                            : d["prompt_command"].asString().c_str());
        printf("  history        %d turns, forgotten when the chat closes\n",
               static_cast<int>(d["history_turns"].asNumber()));
        printf("  generation     temperature %.2f, at most %d tokens\n",
               d["temperature"].asNumber(), static_cast<int>(d["max_tokens"].asNumber()));
        printf("  vision         %s\n", d["vision_enabled"].asBool()
                                            ? "on, on the same providers"
                                            : "off - she cannot look at the screen");
        if (d["vision_enabled"].asBool()) {
            printf("    every        %.0f-%.0f s, after %.1f s of notice\n",
                   d["vision_min"].asNumber(), d["vision_max"].asNumber(),
                   d["vision_notice"].asNumber());
            const Json& deny = d["vision_deny"];
            if (deny.size() == 0) printf("    never        (nothing on the deny list)\n");
            for (size_t i = 0; i < deny.size(); ++i)
                printf("    %-12s %s\n", i ? "" : "never", deny[i].asString().c_str());
        }
        const std::string source = d["config"].asString();
        printf("  from           %s\n", source.empty() ? "no config file - all defaults"
                                                       : source.c_str());
        printf("  persona        %s\n", firstLine(d["persona"].asString()).c_str());
        return kOk;
    }
    // `status`, by elimination: every other verb returned above, and anything
    // that is not one of them was refused before any of this ran.
    const pid_t pid = extPid();
    int status = kError;
    Json d;
    if (!send("ext", ipc::Out().boolean("status", true).done(), &d, &status, true)) {
        if (status == kNotRunning) printf("asuna: not running\n");
        return status;
    }
    const int subscribers = static_cast<int>(d["subscribers"].asNumber());
    if (gJson) {
        // The raw reply is already on stdout - README: "--json on anything
        // prints the raw reply" - so nothing further is printed here. The exit
        // code is another matter: it used to be kOk whatever the helper was
        // doing, which left a --json caller unable to learn anything from
        // either half. The payload is the daemon's view and has no pid in it,
        // because the pid file is this end's, so the exit code is the only
        // place the two views are combined. Same verdict as the text form
        // below, which is the whole point.
        return pid > 0 && subscribers > 0 ? kOk : kNotRunning;
    }
    if (pid > 0 && subscribers > 0) printf("asuna: helper running (pid %d)\n", pid);
    else if (pid > 0) printf("asuna: helper running (pid %d) but not connected to her\n", pid);
    else if (subscribers > 0) printf("asuna: something is subscribed, but it is not ours\n");
    else printf("asuna: helper not running\n");
    printf("  extensions   %s\n", d["enabled"].asBool() ? "enabled" : "off in the config");
    printf("  vision       %s\n", d["vision_enabled"].asBool() ? "allowed" : "not allowed");
    printf("  log          %s\n", paths::extLogPath().c_str());
    return pid > 0 && subscribers > 0 ? kOk : kNotRunning;
}

// Prints her events as they happen, until interrupted. The debugging companion
// to the helper - and the answer to "is anything actually coming out of this",
// which is otherwise a question only a Python script can ask.
int cmdSubscribe() {
    std::string reply, error;
    // Its own connection rather than ipc::call, which half-closes after the
    // request; a subscriber that does that is telling the daemon it has gone.
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return complain(std::string("socket: ") + strerror(errno));
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const std::string path = paths::socketPath();
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return complain("socket path too long: " + path);
    }
    memcpy(addr.sun_path, path.c_str(), path.size());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return complain("not running", kNotRunning);
    }
    const std::string request = ipc::request("subscribe") + "\n";
    if (write(fd, request.data(), request.size()) < 0) {
        close(fd);
        return complain("could not send the request");
    }
    std::string buffer;
    char chunk[1024];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        for (size_t eol; (eol = buffer.find('\n')) != std::string::npos;) {
            printf("%s\n", buffer.substr(0, eol).c_str());
            fflush(stdout);
            buffer.erase(0, eol + 1);
        }
    }
    close(fd);
    fprintf(stderr, "asuna: she hung up\n");
    return kOk;
}

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

}  // namespace

int dispatch(int argc, char** argv, RunShell runShell) {
    // --json anywhere, since it says how to print rather than what to do.
    std::vector<char*> args;
    args.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) gJson = true;
        else args.push_back(argv[i]);
    }
    argc = static_cast<int>(args.size());
    argv = args.data();

    // No subcommand, or a leading flag: the bare form, which runs her here.
    // Kept because it is what every phase before this one did, and because
    // `asuna --model 31 --no-persist` is still the fastest way to try something.
    if (argc < 2 || argv[1][0] == '-') {
        ShellOptions opt;
        if (const int bad = applyConfig(&opt); bad != kOk) return bad;
        const int parsed = parseOptions(argc, argv, 1, &opt, nullptr);
        if (parsed != kOk) return parsed < 0 ? kOk : parsed;
        if (const int busy = refuseIfRunning(opt); busy != kOk) return busy;
        return runShell(std::move(opt));
    }

    const std::string cmd = argv[1];
    const int rest = 2;

    if (cmd == "help") { usage(); return kOk; }
    if (cmd == "start") return cmdStart(argc, argv, rest, runShell);
    if (cmd == "exit" || cmd == "stop" || cmd == "quit") return cmdExit();
    if (cmd == "restart") {
        // Not an error if she was not running: restart means "be running".
        const int left = cmdExit();
        if (left != kOk && left != kNotRunning) return left;
        return cmdStart(argc, argv, rest, runShell);
    }
    if (cmd == "status") return cmdStatus();
    if (cmd == "ping") {
        int status = kError;
        Json d;
        if (!send("ping", "", &d, &status)) return status;
        if (!gJson) printf("asuna: running (pid %d)\n", static_cast<int>(d["pid"].asNumber()));
        return kOk;
    }
    if (cmd == "hide" || cmd == "show" || cmd == "toggle") return simple(cmd);
    if (cmd == "say") {
        ipc::Out args;
        // The four streaming flags may come with no text at all - `--release`
        // and `--clear` are about the line that is already up - so the text is
        // whatever is not a flag rather than a required first argument.
        bool haveText = false;
        for (int i = rest; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--for") {
                // Its own branch rather than `--for && i + 1 < argc`, which fell
                // through to the catch-all below and reported a missing value as
                // "say does not take '--for'".
                if (i + 1 >= argc) return complain("say --for needs a value", kUsage);
                double seconds = 0;
                std::string problem;
                // Strictly positive, and with no floor of its own: the daemon
                // reads 0 as "no timer at all", which is what leaving --for off
                // already means, but anything above it is a duration this end
                // has no business second-guessing.
                if (!argparse::realAbove("say --for", argv[++i], 0.0, &seconds, &problem))
                    return complain(problem, kUsage);
                args.num("seconds", seconds);
            }
            else if (a == "--hold") args.boolean("hold", true);
            else if (a == "--append") args.boolean("append", true);
            else if (a == "--release") args.boolean("release", true);
            else if (a == "--clear") args.boolean("clear", true);
            else if (a.compare(0, 2, "--") == 0)
                return complain("say does not take '" + a + "'", kUsage);
            else if (!haveText) {
                args.str("text", a);
                haveText = true;
            } else {
                return complain("say takes one line of text", kUsage);
            }
        }
        if (args.empty()) return complain("say needs some text", kUsage);
        return simple("say", args.done());
    }
    if (cmd == "think") {
        const std::string state = rest < argc ? argv[rest] : "on";
        if (state != "on" && state != "off") return complain("think takes on or off", kUsage);
        return simple("think", ipc::Out().boolean("on", state == "on").done());
    }
    if (cmd == "chat")
        return simple("chat", ipc::Out().str("text", rest < argc ? argv[rest] : "").done());
    if (cmd == "ext") return cmdExt(argc, argv, rest);
    if (cmd == "subscribe") return cmdSubscribe();
    if (cmd == "motion" || cmd == "expression") return cmdNamed(cmd, argc, argv, rest);
    if (cmd == "model") return cmdModel(argc, argv, rest);
    if (cmd == "move") {
        if (rest >= argc) return complain("move needs a position", kUsage);
        const std::string where = argv[rest];
        // A number is pixels; anything else is one of the three anchors, and
        // the daemon is the one that knows how wide the screen is. Something
        // made only of digits and dots but not actually a number - "1.2.3", or
        // a bare "." - is neither, and used to arrive as a partially parsed
        // 1.2 or a silent 0. No range: the daemon clamps x to the screen it
        // can see, and only it knows how wide that is.
        const bool numeric = where.find_first_not_of("-+0123456789.eE") == std::string::npos;
        if (numeric) {
            double x = 0;
            std::string problem;
            if (!argparse::realAny("move", where, &x, &problem)) return complain(problem, kUsage);
            return simple("move", ipc::Out().num("x", x).done());
        }
        return simple("move", ipc::Out().str("where", where).done());
    }
    if (cmd == "scale") {
        if (rest >= argc) return complain("scale needs a number", kUsage);
        double value = 0;
        std::string problem;
        // Also unranged, and for the same reason: setUserScale clamps to
        // 0.5-2.5 *and* to what the screen has room for, which is often the
        // tighter of the two. Refusing 3.0 here would refuse something that
        // currently works and lands at the ceiling.
        if (!argparse::realAny("scale", argv[rest], &value, &problem))
            return complain(problem, kUsage);
        return simple("scale", ipc::Out().num("value", value).done());
    }
    if (cmd == "layer") {
        if (rest >= argc) return complain("layer needs a name", kUsage);
        return simple("layer", ipc::Out().str("name", argv[rest]).done());
    }
    if (cmd == "menu")
        return simple("menu", ipc::Out().str("action", rest < argc ? argv[rest] : "toggle").done());
    if (cmd == "output") return cmdOutput(argc, argv, rest);
    if (cmd == "config") return cmdConfig(argc, argv, rest);
    if (cmd == "autostart") return cmdAutostart(argc, argv, rest);

    fprintf(stderr, "asuna: unknown command '%s'\n", cmd.c_str());
    usage();
    return kUsage;
}

}  // namespace cli
}  // namespace asuna
