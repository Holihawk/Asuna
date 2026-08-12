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

#include <signal.h>
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
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kAlive);
    CHECK(id.proven());
    CHECK(id.present());
    CHECK(id.pid == getpid());
    CHECK(id.began == asuna::daemon::startTime(getpid()));

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
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kRecycled);
    CHECK(!id.proven());
    CHECK(!id.present());
    // Reported anyway, so a caller can name the number in the message.
    CHECK(id.pid == getpid());
}

void aStaleFileIsGoneRatherThanRecycled() {
    // No such process at all. Different from a recycled pid and worth saying
    // differently: this one just means the helper stopped.
    const pid_t dead = deadPid();
    write(pidFile(), std::to_string(dead) + " 12345\n");
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kGone);
    CHECK(!id.present());
    CHECK(id.pid == dead);
}

void aFileFromAnOlderBuildStillWorks() {
    // One field, which is what every version before this wrote. A helper
    // started before an upgrade must not be orphaned by it, so this falls back
    // to the old check rather than declaring the file unreadable.
    write(pidFile(), std::to_string(getpid()) + "\n");
    asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kUnverified);
    CHECK(id.pid == getpid());
    // Enough to report - `status` says so rather than calling a live helper
    // absent - and deliberately not enough to signal on.
    CHECK(id.present());
    CHECK(!id.proven());

    // Same shape, but the process is gone: still kGone, not kUnverified. An
    // old file is a reason to be lenient about the *check*, not about whether
    // anything is there.
    write(pidFile(), std::to_string(deadPid()) + "\n");
    id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kGone);
}

void rubbishInThePidFileIsNotAProcess() {
    const char* junk[] = {"", "   ", "not a pid", "0", "-5", "\n\n", "abc 123"};
    for (const char* text : junk) {
        write(pidFile(), text);
        const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
        CHECK(id.state == asuna::daemon::Owner::kGone);
        CHECK(id.pid == 0);
    }
    // A pid with a start time that is not a number falls back to the old check
    // rather than throwing the pid away - the pid is still readable, and it is
    // the field that matters most.
    write(pidFile(), std::to_string(getpid()) + " not-a-time\n");
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kUnverified);
    CHECK(id.pid == getpid());
}

void aMissingFileIsSimplyNobody() {
    std::error_code ec;
    fs::remove(pidFile(), ec);
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kGone);
    CHECK(id.pid == 0);
}

// --- signalling ------------------------------------------------------------
//
// Classifying a pid file is only half of it. The other half is what happens
// next, and that is where the damage would be done: every one of these is about
// refusing to send a signal rather than about reading a file.

// A child that waits to be signalled, so a real process can be opened and
// stopped. Returns its pid; the caller reaps it.
pid_t sleeper() {
    const pid_t child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    // Give it a moment to reach pause(), so a SIGTERM cannot arrive first.
    usleep(30 * 1000);
    return child;
}

void aProvenIdentityCanBeOpenedAndSignalled() {
    const pid_t child = sleeper();
    CHECK(asuna::daemon::writePidFile(pidFile().string(), child));
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.proven());

    asuna::daemon::Signal target;
    CHECK(target.open(id));
    // On this kernel it is a pidfd, which is the whole point; the fallback is
    // still correct but not race-free, and a test that could not tell them
    // apart would pass either way.
    CHECK(target.exact());
    CHECK(target.alive());
    CHECK(target.send(SIGTERM));

    int status = 0;
    waitpid(child, &status, 0);
    // Dead, and the handle knows it - rather than answering for whoever holds
    // the number next.
    CHECK(!target.alive());
    CHECK(WIFSIGNALED(status));
}

void anUnverifiedIdentityIsNotSomethingToSignal() {
    // The upgrade case, and the one this whole file exists for: a one-field pid
    // file from an older build. Something is running under that number and
    // nothing can show it is the helper, so `stop` must not signal it. Opening
    // is where that is enforced, so that no caller can get a handle by accident.
    const pid_t child = sleeper();
    write(pidFile(), std::to_string(child) + "\n");
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kUnverified);
    CHECK(id.present());   // `status` may say so

    asuna::daemon::Signal target;
    CHECK(!target.open(id));   // `stop` may not
    CHECK(!target.alive());
    CHECK(!target.send(SIGTERM));

    // --force is the only way through, and it is a different function - so
    // every place we signal without proof is one grep away.
    asuna::daemon::Signal forced;
    CHECK(forced.openUnproven(child));
    CHECK(forced.alive());
    CHECK(forced.send(SIGTERM));
    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status));
}

void arecycledIdentityCannotBeOpenedAtAll() {
    // The pid is alive - it is this process - but the file records a different
    // start time. Nothing may be sent to it, forced or otherwise, because the
    // file has been shown to name something that is gone.
    const unsigned long long wrong = asuna::daemon::startTime(getpid()) + 1;
    write(pidFile(), std::to_string(getpid()) + " " + std::to_string(wrong) + "\n");
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());
    CHECK(id.state == asuna::daemon::Owner::kRecycled);

    asuna::daemon::Signal target;
    CHECK(!target.open(id));
    CHECK(!target.send(SIGKILL));   // and would have killed this very test
}

void aHandleRefusesAnIdentityThatDoesNotMatch() {
    // open() confirms the start time itself rather than trusting the caller's
    // classification, because it is the last thing standing before a signal.
    const pid_t child = sleeper();
    asuna::daemon::Identity forged;
    forged.pid = child;
    forged.began = asuna::daemon::startTime(child) + 99;
    forged.state = asuna::daemon::Owner::kAlive;   // claimed, not true

    asuna::daemon::Signal target;
    CHECK(!target.open(forged));
    CHECK(!target.send(SIGTERM));

    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
}

void aHandleToADeadProcessSignalsNobody() {
    const pid_t child = sleeper();
    asuna::daemon::writePidFile(pidFile().string(), child);
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());

    asuna::daemon::Signal target;
    CHECK(target.open(id));
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);

    // The pid is now free for the kernel to reissue. The handle refers to the
    // process, so it reports gone and sends nothing - where a bare `kill(pid,
    // SIGKILL)` would have gone to whoever inherited the number.
    CHECK(!target.alive());
    CHECK(!target.send(SIGKILL));
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
        {"aProvenIdentityCanBeOpenedAndSignalled", aProvenIdentityCanBeOpenedAndSignalled},
        {"anUnverifiedIdentityIsNotSomethingToSignal", anUnverifiedIdentityIsNotSomethingToSignal},
        {"arecycledIdentityCannotBeOpenedAtAll", arecycledIdentityCannotBeOpenedAtAll},
        {"aHandleRefusesAnIdentityThatDoesNotMatch", aHandleRefusesAnIdentityThatDoesNotMatch},
        {"aHandleToADeadProcessSignalsNobody", aHandleToADeadProcessSignalsNobody},
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
