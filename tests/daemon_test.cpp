// Process identity: the half of src/app/daemon.cpp that decides whether a pid
// file still names the process that wrote it.
//
// This exists because `ext stop` sends a signal. The helper is somebody else's
// program - `[ext] command` can name anything - so it cannot be made to hold a
// lock for us, and a pid file is all there is. A bare pid is not an identity:
// pids are reused, so a file that outlives its process names whatever takes
// that number next, and stopping the helper would then signal a stranger.
//
// What makes it an identity is the start time the kernel records against the
// pid. A reused pid has a different one, always. Nothing here needs a
// compositor, a socket or root.

#include "app/daemon.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;
const char* current = "";
fs::path tmpDir;

void check(bool ok, const char* expr, int line) {
    if (ok) return;
    printf("  FAIL %s:%d  %s\n", current, line, expr);
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path pidFile() { return tmpDir / "ext.pid"; }

void write(const fs::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

// A child that exits immediately, reaped, so its pid is free and its /proc
// entry is gone. The nearest thing to "a pid that is not a process" that can be
// arranged without guessing at a number.
pid_t deadPid() {
    const pid_t child = fork();
    if (child == 0) _exit(0);
    int ignored = 0;
    waitpid(child, &ignored, 0);
    return child;
}

// --- the tests -------------------------------------------------------------

void aStartTimeIsStableAndSpecificToTheProcess() {
    const unsigned long long mine = asuna::daemon::startTime(getpid());
    CHECK(mine != 0);
    // Read twice is read the same. It is the moment the process began, not a
    // clock, and the whole scheme rests on it never changing under us.
    CHECK(asuna::daemon::startTime(getpid()) == mine);

    // pid 1 exists on every Linux and started before us.
    const unsigned long long init = asuna::daemon::startTime(1);
    CHECK(init != 0);
    CHECK(init <= mine);
}

void anAbsentProcessHasNoStartTime() {
    // 0 is "cannot tell", and must never be compared as if it were a value.
    CHECK(asuna::daemon::startTime(deadPid()) == 0);
    CHECK(asuna::daemon::startTime(0) == 0);
    CHECK(asuna::daemon::startTime(-1) == 0);
}

void aFileWeJustWroteNamesUs() {
    CHECK(asuna::daemon::writePidFile(pidFile().string(), getpid()));
    pid_t got = 0;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kAlive);
    CHECK(got == getpid());

    // Two fields, one line, and the second one is the start time - the format a
    // person may well end up reading over somebody's shoulder.
    std::istringstream f(slurp(pidFile()));
    pid_t pid = 0;
    unsigned long long began = 0;
    CHECK(static_cast<bool>(f >> pid >> began));
    CHECK(pid == getpid());
    CHECK(began == asuna::daemon::startTime(getpid()));
    // And nothing left behind by the rename.
    CHECK(!fs::exists(pidFile().string() + ".tmp"));
}

void aRecycledPidIsNotOurProcess() {
    // The whole point. The pid is alive - it is this very process - but the
    // start time recorded against it belongs to something that has gone. Under
    // the old kill(pid, 0) check this was indistinguishable from the helper
    // still running, and `ext stop` would have signalled whatever it now is.
    const unsigned long long wrong = asuna::daemon::startTime(getpid()) + 1;
    write(pidFile(), std::to_string(getpid()) + " " + std::to_string(wrong) + "\n");
    pid_t got = 0;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kRecycled);
    // Reported anyway, so a caller can name the number in the message.
    CHECK(got == getpid());
}

void aStaleFileIsGoneRatherThanRecycled() {
    // No such process at all. Different from a recycled pid and worth saying
    // differently: this one just means the helper stopped.
    const pid_t dead = deadPid();
    write(pidFile(), std::to_string(dead) + " 12345\n");
    pid_t got = 0;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kGone);
    CHECK(got == dead);
}

void aFileFromAnOlderBuildStillWorks() {
    // One field, which is what every version before this wrote. A helper
    // started before an upgrade must not be orphaned by it, so this falls back
    // to the old check rather than declaring the file unreadable.
    write(pidFile(), std::to_string(getpid()) + "\n");
    pid_t got = 0;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kUnverified);
    CHECK(got == getpid());

    // Same shape, but the process is gone: still kGone, not kUnverified. An
    // old file is a reason to be lenient about the *check*, not about whether
    // anything is there.
    write(pidFile(), std::to_string(deadPid()) + "\n");
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kGone);
}

void rubbishInThePidFileIsNotAProcess() {
    const char* junk[] = {"", "   ", "not a pid", "0", "-5", "\n\n", "abc 123"};
    for (const char* text : junk) {
        write(pidFile(), text);
        pid_t got = -1;
        CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
              asuna::daemon::Owner::kGone);
        CHECK(got == 0);
    }
    // A pid with a start time that is not a number falls back to the old check
    // rather than throwing the pid away - the pid is still readable, and it is
    // the field that matters most.
    write(pidFile(), std::to_string(getpid()) + " not-a-time\n");
    pid_t got = 0;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kUnverified);
    CHECK(got == getpid());
}

void aMissingFileIsSimplyNobody() {
    std::error_code ec;
    fs::remove(pidFile(), ec);
    pid_t got = -1;
    CHECK(asuna::daemon::readPidFile(pidFile().string(), &got) ==
          asuna::daemon::Owner::kGone);
    CHECK(got == 0);
}

void aProcessWithAnAwkwardNameIsStillReadable() {
    // /proc/<pid>/stat puts the executable name in parentheses in field 2, and
    // that name may contain both spaces and parentheses. Splitting the line on
    // whitespace from the left is therefore wrong, and wrong in a way that only
    // shows up for somebody whose `[ext] command` names such a program. The
    // name is settable through prctl, so this can be checked rather than
    // assumed.
    const unsigned long long before = asuna::daemon::startTime(getpid());
    const std::string original = slurp("/proc/self/comm");
    std::ofstream(("/proc/self/comm")) << "a b) (c";
    CHECK(asuna::daemon::startTime(getpid()) == before);
    // Put it back, so the rest of the suite reports under its own name.
    std::ofstream("/proc/self/comm") << original.substr(0, original.find('\n'));
}

}  // namespace

int main() {
    tmpDir = fs::temp_directory_path() / ("asuna-daemon-test-" + std::to_string(getpid()));
    fs::create_directories(tmpDir);

    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"aStartTimeIsStableAndSpecificToTheProcess", aStartTimeIsStableAndSpecificToTheProcess},
        {"anAbsentProcessHasNoStartTime", anAbsentProcessHasNoStartTime},
        {"aFileWeJustWroteNamesUs", aFileWeJustWroteNamesUs},
        {"aRecycledPidIsNotOurProcess", aRecycledPidIsNotOurProcess},
        {"aStaleFileIsGoneRatherThanRecycled", aStaleFileIsGoneRatherThanRecycled},
        {"aFileFromAnOlderBuildStillWorks", aFileFromAnOlderBuildStillWorks},
        {"rubbishInThePidFileIsNotAProcess", rubbishInThePidFileIsNotAProcess},
        {"aMissingFileIsSimplyNobody", aMissingFileIsSimplyNobody},
        {"aProcessWithAnAwkwardNameIsStillReadable", aProcessWithAnAwkwardNameIsStillReadable},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-48s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu daemon tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
