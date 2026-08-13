#include "app/cli/internal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "app/argparse.hpp"
#include "app/config.hpp"
#include "app/daemon.hpp"
#include "app/ipc.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;

namespace asuna {
namespace cli {
namespace {

// How the middle process of `ext start`'s double fork reports back. Its exit
// status is all it has: it is the one process that knows the helper's pid, and
// everything it does happens on the other side of a fork from the caller.
constexpr int kStagedOk = 0;
constexpr int kStagedNoFork = 1;
constexpr int kStagedNoPidFile = 2;
constexpr int kStagedCleanupFailed = 3;
constexpr int kStagedNoGate = 4;

}  // namespace

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

}  // namespace cli
}  // namespace asuna
