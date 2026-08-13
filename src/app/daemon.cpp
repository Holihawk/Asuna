#include "app/daemon.hpp"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace asuna {
namespace daemon {
namespace {

// Rotate the log past this. A drag currently emits a few hundred Gdk warnings
// (it is a long-running daemon), so an unrotated log is not a hypothetical problem.
constexpr uintmax_t kMaxLogBytes = 1u << 20;

pid_t readPid(int fd) {
    char buf[32] = {0};
    const ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    return static_cast<pid_t>(atoi(buf));
}

}  // namespace

Lock::~Lock() { release(); }

bool Lock::acquire(const std::string& path, pid_t* holder) {
    release();
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (holder) *holder = 0;
        fprintf(stderr, "asuna: cannot open %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (holder) *holder = readPid(fd);
        close(fd);
        return false;
    }
    // Ours. The pid is written for `status` and for the shutdown fallback; the
    // lock itself is what makes us the only one.
    if (ftruncate(fd, 0) == 0) {
        const std::string text = std::to_string(getpid()) + "\n";
        if (write(fd, text.data(), text.size()) < 0) { /* advisory only */ }
    }
    mFd = fd;
    mPath = path;
    return true;
}

void Lock::release() {
    if (mFd < 0) return;
    // The file stays. Unlinking it would let a second daemon create a fresh one
    // and take a lock on a different inode while we were still shutting down.
    close(mFd);   // releases the flock
    mFd = -1;
    mPath.clear();
}

pid_t holderPid(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    pid_t pid = 0;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        pid = readPid(fd);   // somebody holds it
    } else {
        flock(fd, LOCK_UN);
    }
    close(fd);
    return pid;
}

bool redirectOutput(const std::string& path, std::string* error) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (fs::file_size(path, ec) > kMaxLogBytes && !ec)
        fs::rename(path, path + ".1", ec);

    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        if (error) *error = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        close(null);
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
    // Line buffering survives the dup2 (it is a property of the FILE*, and the
    // fd underneath just changed), but stderr is unbuffered and stdout was set
    // line-buffered in main() while it was still a terminal - restate it, since
    // the C library picks full buffering for a file and the interleaving of the
    // two streams in the log is the whole point of having one.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    return true;
}

namespace {

GLogWriterOutput filterWriter(GLogLevelFlags level, const GLogField* fields, gsize count,
                              gpointer data) {
    for (gsize i = 0; i < count; ++i) {
        if (strcmp(fields[i].key, "MESSAGE") != 0 || !fields[i].value) continue;
        const char* message = static_cast<const char*>(fields[i].value);
        if (!strstr(message, "gdk_wayland_toplevel_compute_size")) break;
        static bool announced = false;
        if (!announced) {
            announced = true;
            printf("asuna: suppressing repeated gdk_wayland_toplevel_compute_size warnings"
                   " - GDK asks a both-edges-anchored layer surface for a width the"
                   " compositor owns. Harmless; ASUNA_LOG_ALL=1 shows them.\n");
        }
        return G_LOG_WRITER_HANDLED;
    }
    return g_log_writer_default(level, fields, count, data);
}

}  // namespace

void installLogFilter() {
    if (const char* all = getenv("ASUNA_LOG_ALL"); all && *all && strcmp(all, "0") != 0) return;
    g_log_set_writer_func(filterWriter, nullptr, nullptr);
}

std::string tailLog(const std::string& path, int lines) {
    std::ifstream f(path);
    if (!f) return "";
    std::deque<std::string> kept;
    std::string line;
    while (std::getline(f, line)) {
        kept.push_back(line);
        if (static_cast<int>(kept.size()) > lines) kept.pop_front();
    }
    std::string out;
    for (const auto& l : kept) out += l + "\n";
    return out;
}

long rssKb() {
    // statm rather than status: two fields to parse instead of forty, and the
    // second one is exactly what we want.
    std::ifstream f("/proc/self/statm");
    long total = 0, resident = 0;
    if (!(f >> total >> resident)) return 0;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

unsigned long long startTime(pid_t pid, int* unreadable) {
    if (unreadable) *unreadable = 0;
    if (pid <= 0) return 0;
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    errno = 0;
    std::ifstream f(path);
    if (!f) {
        // Which zero this is. stat(2) needs no descriptor of its own, so it can
        // still answer when a full descriptor table is the reason the open above
        // failed - which is exactly the case where the difference matters.
        const int why = errno;
        struct stat sb;
        if (unreadable && ::stat(path.c_str(), &sb) == 0) *unreadable = why ? why : EIO;
        return 0;
    }
    std::string line;
    std::getline(f, line);

    // Field 2 is the executable name in parentheses, and it may contain both
    // spaces and parentheses - `(my prog (old))` is a legal comm. Splitting on
    // whitespace from the left is therefore wrong for every process whose name
    // has a space in it. The last ')' is the documented way through: everything
    // after it is fixed-width fields separated by single spaces.
    const size_t close = line.rfind(')');
    if (close == std::string::npos) return 0;

    // Field 3 (state) is the first one after that, so field 22 (starttime) is
    // the 20th token from here.
    std::istringstream rest(line.substr(close + 1));
    std::string token;
    for (int i = 0; i < 20; ++i)
        if (!(rest >> token)) return 0;

    errno = 0;
    char* end = nullptr;
    const unsigned long long v = strtoull(token.c_str(), &end, 10);
    if (end == token.c_str() || *end != '\0' || errno == ERANGE) return 0;
    // 0 is this function's "cannot tell", and no process has it: pid 1 is
    // started after boot, so every real starttime is at least a tick or two.
    return v;
}

bool writePidFile(const std::string& path, pid_t pid) {
    // Read before write, and via a rename: `ext status` may be running right
    // now, and a reader that catches a half-written line sees a truncated pid,
    // which is a different process. Same argument as State::save.
    const unsigned long long began = startTime(pid);
    // No start time, no file. Writing the pid alone would produce the old
    // one-field record, which readPidFile has to call kUnverified and `ext stop`
    // may not signal - so the helper would run under a number nothing could
    // ever be sure of. That state is for files written before this build
    // existed, not for this build to create; see the header.
    if (began == 0) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << pid << " " << began << "\n";
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

Identity readPidFile(const std::string& path) {
    Identity id;
    unsigned long long recorded = 0;
    bool haveStart = false;
    {
        // Closed before /proc is opened, rather than held across it. Two files
        // open at once needs two free descriptors, and with only one to spare the
        // /proc read failed - which this function had no way to tell from "no
        // such process", so a running helper was reported as gone. Reading them
        // one after the other needs one.
        std::ifstream f(path);
        if (!(f >> id.pid) || id.pid <= 0) {
            id.pid = 0;
            return id;   // kGone
        }
        haveStart = static_cast<bool>(f >> recorded);
    }

    const unsigned long long now = startTime(id.pid);
    if (now == 0) return id;   // kGone: no such process, whatever the file says
    if (!haveStart || recorded == 0) {
        // Written by a build from before this file had a second field, or by
        // one that could not read /proc at the time. Enough to say something is
        // there - which keeps `status` honest across an upgrade rather than
        // declaring a perfectly healthy helper absent - and deliberately not
        // enough to signal on. See Signal::open.
        id.state = Owner::kUnverified;
        return id;
    }
    id.began = recorded;
    id.state = recorded == now ? Owner::kAlive : Owner::kRecycled;
    return id;
}

Signal::~Signal() { shut(); }

void Signal::shut() {
    if (mFd >= 0) ::close(mFd);
    mFd = -1;
    mPid = 0;
    mBegan = 0;
    mRefusal = Refusal::kNone;
    mError = 0;
}

namespace {

// Both syscall numbers or neither. A handle that could be opened but not
// signalled through would be worse than no handle at all - it would report
// `exact()` and then fail every send - and the two calls arrived one kernel
// release apart, so there is no toolchain with one that wants the other.
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
#define ASUNA_HAVE_PIDFD 1
#endif

// What asking the kernel for a handle to a process came back with.
//
// An `int` that was either an fd or -1 used to be the whole answer, and it
// conflated two unrelated things: a kernel that has no pidfds, where re-reading
// the start time immediately before kill(2) is the best available and is what
// `exact()` exists to report, and a pidfd_open that failed for a reason of its
// own. Buying the weaker guarantee with EMFILE or ENOMEM is precisely backwards:
// those are the moments to fail closed.
struct Handle {
    int fd = -1;
    int err = 0;          // errno, when there is no fd
    bool absent = false;  // no pidfd_open here at all, rather than one that failed
};

// syscall() rather than the glibc wrappers: pidfd_open arrived in glibc 2.36
// and pidfd_send_signal has never had one, so going through the numbers is both
// the older-toolchain path and the only path for one of the two.
Handle openPidfd(pid_t pid) {
    Handle h;
#ifdef ASUNA_HAVE_PIDFD
    h.fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0u));
    if (h.fd >= 0) return h;
    h.err = errno;
    h.fd = -1;
    // ENOSYS is the kernel saying it has never heard of the call, which is the
    // one answer the bare-pid fallback is for. EMFILE, ENFILE and ENOMEM are
    // this machine being short of something; EPERM is a process we may not
    // touch; ESRCH is a process that has gone, which is a reason not to signal
    // that number rather than a reason to signal it by hand.
    h.absent = h.err == ENOSYS;
#else
    (void)pid;
    h.absent = true;
    h.err = ENOSYS;
#endif
    return h;
}

bool sendThroughPidfd(int fd, int sig, int* err) {
#ifdef ASUNA_HAVE_PIDFD
    if (syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0u) == 0) return true;
    if (err) *err = errno;
    return false;
#else
    (void)fd;
    (void)sig;
    if (err) *err = ENOSYS;
    return false;
#endif
}

}  // namespace

bool Signal::open(const Identity& id) {
    shut();
    if (!id.proven() || id.began == 0) {
        mRefusal = Refusal::kUnprovable;
        return false;
    }

    // The handle first, the confirmation second. The other order - confirm,
    // then open - leaves exactly the gap this class is for: the process can end
    // and its number be reissued in between, and the handle would then name the
    // newcomer.
    const Handle h = openPidfd(id.pid);
    if (h.fd < 0 && !h.absent) {
        mRefusal = h.err == ESRCH ? Refusal::kChanged : Refusal::kUnavailable;
        mError = h.err;
        return false;
    }
    // "Cannot look" is not "has gone". Comparing an unreadable /proc against the
    // recorded time would make a live helper look recycled, and the caller acts
    // on that by removing the file that names it - so the same distinction the
    // handle above is careful about applies to its confirmation.
    int unreadable = 0;
    if (startTime(id.pid, &unreadable) != id.began) {
        if (h.fd >= 0) ::close(h.fd);
        mRefusal = unreadable ? Refusal::kUnavailable : Refusal::kChanged;
        mError = unreadable;
        return false;
    }
    mPid = id.pid;
    mBegan = id.began;
    mFd = h.fd;   // -1 only where the kernel has no pidfds; send() falls back
    return true;
}

bool Signal::openUnproven(pid_t pid) {
    shut();
    if (pid <= 0) {
        mRefusal = Refusal::kUnprovable;
        return false;
    }
    const Handle h = openPidfd(pid);
    if (h.fd < 0 && !h.absent) {
        mRefusal = h.err == ESRCH ? Refusal::kChanged : Refusal::kUnavailable;
        mError = h.err;
        return false;
    }
    int unreadable = 0;
    const unsigned long long now = startTime(pid, &unreadable);
    if (now == 0) {
        if (h.fd >= 0) ::close(h.fd);
        mRefusal = unreadable ? Refusal::kUnavailable : Refusal::kChanged;
        mError = unreadable;
        return false;
    }
    // Whatever it is, it is pinned from here on: --force means "signal that
    // process", not "signal whatever holds that number by the time we get
    // round to it".
    mPid = pid;
    mBegan = now;
    mFd = h.fd;
    return true;
}

bool Signal::alive() const {
    if (mPid <= 0) return false;
    if (mFd >= 0) return sendThroughPidfd(mFd, 0, nullptr);
    return startTime(mPid) == mBegan;
}

bool Signal::send(int sig, int* err) const {
    if (err) *err = 0;
    if (mPid <= 0) {
        if (err) *err = ESRCH;
        return false;
    }
    if (mFd >= 0) return sendThroughPidfd(mFd, sig, err);
    // No pidfd. Re-reading the identity immediately before the signal narrows
    // the window rather than closing it, which is the best a pid can do.
    if (startTime(mPid) != mBegan) {
        if (err) *err = ESRCH;
        return false;
    }
    if (kill(mPid, sig) == 0) return true;
    if (err) *err = errno;
    return false;
}

}  // namespace daemon
}  // namespace asuna
