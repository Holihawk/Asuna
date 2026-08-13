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

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

    // And when asked, it says *which* nothing this is: an absent process rather
    // than an unreadable one.
    int unreadable = -1;
    CHECK(asuna::daemon::startTime(deadPid(), &unreadable) == 0);
    CHECK(unreadable == 0);
    unreadable = -1;
    CHECK(asuna::daemon::startTime(getpid(), &unreadable) != 0);
    CHECK(unreadable == 0);
}

void anUnreadableProcIsNotAnAbsentProcess() {
    // The other zero. /proc/self/stat plainly exists, so a failure to open it is
    // this machine being short of descriptors and says nothing about whether the
    // process is there - and the difference matters because the caller that
    // compares this value is the one about to send a signal. Conflating them let
    // a full descriptor table read as "the helper has gone", which removed the
    // only file naming a helper that was still running.
    //
    // Exhausted for real rather than simulated: a low RLIMIT_NOFILE and every
    // descriptor under it taken, which is how it happens.
    rlimit was{};
    CHECK(getrlimit(RLIMIT_NOFILE, &was) == 0);
    rlimit few = was;
    few.rlim_cur = 8;
    CHECK(setrlimit(RLIMIT_NOFILE, &few) == 0);

    std::vector<int> held;
    for (;;) {
        const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) break;
        held.push_back(fd);
    }
    int unreadable = 0;
    const unsigned long long unknown = asuna::daemon::startTime(getpid(), &unreadable);
    for (const int fd : held) ::close(fd);
    setrlimit(RLIMIT_NOFILE, &was);

    CHECK(!held.empty());        // the table really was filled
    CHECK(unknown == 0);         // so it could not tell
    CHECK(unreadable != 0);      // and it says why, rather than implying absence
    // And with descriptors to spare again, the same pid answers as it did.
    CHECK(asuna::daemon::startTime(getpid()) != 0);
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
    CHECK(target.refusal() == asuna::daemon::Signal::Refusal::kNone);
    CHECK(target.error() == 0);
    // Both the pidfd and start-time fallback must preserve basic signalling;
    // pidfd-specific atomicity and zombie detection are asserted separately.
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kAlive);
    CHECK(target.send(SIGTERM));

    int status = 0;
    waitpid(child, &status, 0);
    // Dead, and the handle knows it - rather than answering for whoever holds
    // the number next.
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kGone);
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
    // And says which refusal it was. This is the *only* one --force may answer:
    // the identity was never provable, rather than shown to have moved on or
    // withheld by the kernel.
    CHECK(target.refusal() == asuna::daemon::Signal::Refusal::kUnprovable);
    CHECK(target.error() == 0);
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kGone);
    CHECK(!target.send(SIGTERM));

    // --force is the only way through, and it is a different function - so
    // every place we signal without proof is one grep away.
    asuna::daemon::Signal forced;
    CHECK(forced.openUnproven(child));
    CHECK(forced.probe() == asuna::daemon::Signal::Presence::kAlive);
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
    // Refused as unprovable rather than as changed: readPidFile already decided
    // this file does not name a live helper, so open() never got as far as
    // asking the kernel about the pid.
    CHECK(target.refusal() == asuna::daemon::Signal::Refusal::kUnprovable);
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
    // kChanged, and this is the distinction the CLI turns on: an identity that
    // *was* proven and no longer is may not be forced, where one that was never
    // provable may. Reported as the same bare `false` they were indistinguishable
    // from the caller, and a plain `ext stop` in this exact race went on to pin
    // the current holder of the number and signal it.
    CHECK(target.refusal() == asuna::daemon::Signal::Refusal::kChanged);
    CHECK(target.refusal() != asuna::daemon::Signal::Refusal::kUnprovable);
    // Nothing was withheld by the kernel, so there is no errno to report.
    CHECK(target.error() == 0);

    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);
}

void aProcessThatHasGoneIsNotAKernelWithoutPidfds() {
    // A pid that is not a process at all. pidfd_open answers ESRCH, and the
    // difference between that and ENOSYS is the whole of finding 2: ENOSYS is
    // the one answer the bare-pid fallback is for, and every other failure -
    // ESRCH here, EMFILE or ENOMEM on a busy machine - has to refuse instead.
    // Treating them alike would mean a shortage of file descriptors silently
    // buying the weaker check.
    const pid_t dead = deadPid();
    asuna::daemon::Identity forged;
    forged.pid = dead;
    forged.began = 12345;
    forged.state = asuna::daemon::Owner::kAlive;   // claimed, not true

    asuna::daemon::Signal target;
    CHECK(!target.open(forged));
    CHECK(target.refusal() == asuna::daemon::Signal::Refusal::kChanged);
    CHECK(target.refusal() != asuna::daemon::Signal::Refusal::kUnavailable);
    // Not a handle, so not `exact()` - but not the fallback either. There is no
    // process, and both roads end in sending nothing.
    CHECK(!target.exact());
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kGone);
    CHECK(!target.send(SIGTERM));

    // --force cannot reach it either: nothing to pin.
    asuna::daemon::Signal forced;
    CHECK(!forced.openUnproven(dead));
    CHECK(forced.refusal() == asuna::daemon::Signal::Refusal::kChanged);
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
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kGone);
    int err = -1;
    CHECK(!target.send(SIGKILL, &err));
    // ESRCH and not something else, because the caller has to tell "it had
    // already stopped", which is a stop, from "it is running and I could not
    // signal it", which must not be reported as one.
    CHECK(err == ESRCH);
}

void aSignalPermissionFailureDoesNotMeanTheProcessIsGone() {
    // PID 1 normally belongs to another uid in an ordinary test run. Signal 0
    // is harmless but performs the same permission check as a real signal: the
    // send returns EPERM while poll(pidfd) still says the process is alive. The
    // old boolean alive() repeated signal 0, got the same EPERM, called that
    // false, and let the CLI report a successful stop and delete its pid file.
    const unsigned long long began = asuna::daemon::startTime(1);
    if (began == 0) {
        printf("  (skipped: pid 1 is not visible through /proc)\n");
        return;
    }
    asuna::daemon::Identity init;
    init.pid = 1;
    init.began = began;
    init.state = asuna::daemon::Owner::kAlive;

    asuna::daemon::Signal target;
    if (!target.open(init)) {
        printf("  (skipped: pid 1 cannot be opened here)\n");
        return;
    }
    // Root would normally skip the permission half of this test. Drop only the
    // effective uid while sending signal 0 so root-run CI exercises EPERM too;
    // restore it before making an assertion or returning.
    const uid_t originalEuid = geteuid();
    bool lowered = false;
    if (originalEuid == 0) {
        if (seteuid(65534) != 0) {
            printf("  (skipped: root could not lower its effective uid)\n");
            return;
        }
        lowered = true;
    }
    int err = 0;
    const bool sent = target.send(0, &err);
    if (lowered && seteuid(originalEuid) != 0) {
        std::perror("seteuid restore");
        std::abort();
    }
    if (sent) {
        // Some containers preserve permission despite the uid change. No
        // signal is delivered by signal 0, so this remains harmless.
        printf("  (skipped: this environment may signal pid 1)\n");
        return;
    }
    CHECK(err == EPERM);
    CHECK(target.probe(&err) == asuna::daemon::Signal::Presence::kAlive);
    CHECK(err == 0);
}

void anExitedZombieIsGoneBeforeItsParentReapsIt() {
    // pidfd_send_signal(fd, 0) can still succeed for a zombie. That is not an
    // alive helper: it has exited and only its bookkeeping remains until the
    // parent waits. poll(pidfd) is the kernel interface that distinguishes the
    // two, and `ext stop` relies on it before deleting the identity file.
    const pid_t child = sleeper();
    CHECK(asuna::daemon::writePidFile(pidFile().string(), child));
    const asuna::daemon::Identity id = asuna::daemon::readPidFile(pidFile().string());

    asuna::daemon::Signal target;
    CHECK(target.open(id));
    if (!target.exact()) {
        // Re-reading /proc cannot distinguish a live process from its unreaped
        // zombie. That is the documented pre-pidfd fallback limitation, not a
        // property this pidfd-specific test can require from kernels before 5.3.
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        printf("  (skipped: kernel has no pidfd support)\n");
        return;
    }
    CHECK(target.send(SIGKILL));

    asuna::daemon::Signal::Presence present = asuna::daemon::Signal::Presence::kAlive;
    for (int i = 0; i < 100 && present == asuna::daemon::Signal::Presence::kAlive; ++i) {
        usleep(10 * 1000);
        present = target.probe();
    }
    // Deliberately before waitpid(): the process is an unreaped zombie here.
    CHECK(present == asuna::daemon::Signal::Presence::kGone);
    CHECK(target.probe() == asuna::daemon::Signal::Presence::kGone);

    int status = 0;
    waitpid(child, &status, 0);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
}

void aPidNothingCanBeNamedInIsNotWritten() {
    // A pid with no /proc entry: nothing to record a start time from. The file
    // that would be written is the old one-field format, which `ext stop` may
    // not signal - so a helper would be left running under a number no later
    // command could be sure of. Failing is what lets `ext start` decide not to
    // leave one; see cmdExtStart.
    std::error_code ec;
    fs::remove(pidFile(), ec);
    CHECK(!asuna::daemon::writePidFile(pidFile().string(), deadPid()));
    // And nothing left behind: neither the file nor the temporary it goes
    // through, because a half-made identity is the thing being avoided.
    CHECK(!fs::exists(pidFile()));
    CHECK(!fs::exists(pidFile().string() + ".tmp"));

    CHECK(!asuna::daemon::writePidFile(pidFile().string(), 0));
    CHECK(!asuna::daemon::writePidFile(pidFile().string(), -1));
    CHECK(!fs::exists(pidFile()));
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
        {"anUnreadableProcIsNotAnAbsentProcess", anUnreadableProcIsNotAnAbsentProcess},
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
        {"aProcessThatHasGoneIsNotAKernelWithoutPidfds", aProcessThatHasGoneIsNotAKernelWithoutPidfds},
        {"aHandleToADeadProcessSignalsNobody", aHandleToADeadProcessSignalsNobody},
        {"aSignalPermissionFailureDoesNotMeanTheProcessIsGone", aSignalPermissionFailureDoesNotMeanTheProcessIsGone},
        {"anExitedZombieIsGoneBeforeItsParentReapsIt", anExitedZombieIsGoneBeforeItsParentReapsIt},
        {"aPidNothingCanBeNamedInIsNotWritten", aPidNothingCanBeNamedInIsNotWritten},
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
