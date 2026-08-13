#include "app/cli/internal.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "app/daemon.hpp"
#include "app/ipc.hpp"
#include "paths.hpp"

namespace asuna {
namespace cli {
namespace {

// How long `start` waits for the socket to answer, and how long `exit` waits
// for the process to actually be gone before escalating.
constexpr int kStartTimeoutMs = 10000;

}  // namespace

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

}  // namespace cli
}  // namespace asuna
