#include "app/daemon.hpp"

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>

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

}  // namespace daemon
}  // namespace asuna
