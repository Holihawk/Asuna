#pragma once

#include <sys/types.h>

#include <string>

namespace asuna {

// The pieces a background process needs that the pet itself does not: being the
// only one, having somewhere for its output to go, and being able to say how
// much memory it is using.
//
// Primitives only. What `asuna start` actually does with them - fork twice,
// wait for the socket, print a confirmation - is policy and lives in cli.cpp.
namespace daemon {

// The single-instance lock: an `flock` held on a file for the lifetime of the
// process.
//
// A lock rather than a pid file because the kernel releases it however we die,
// including SIGKILL and a panic. A pid file has to be validated against
// /proc, and the validation is a guess: pids are reused, and "is pid 4711 an
// asuna" is a question with no reliable answer from a shell.
class Lock {
public:
    ~Lock();
    Lock() = default;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    // False if somebody else already holds it; `holder` then gets their pid (or
    // 0 if the file has not been written yet). The lock file records our pid as
    // a convenience for `status` and for the SIGTERM fallback - it is never the
    // thing that decides whether a daemon is running.
    bool acquire(const std::string& path, pid_t* holder = nullptr);
    void release();
    bool held() const { return mFd >= 0; }

private:
    std::string mPath;
    int mFd = -1;
};

// The pid of whoever holds the lock at `path`, or 0 if nobody does. Probing by
// trying to take the lock, so this cannot be fooled by a stale file.
pid_t holderPid(const std::string& path);

// Points stdout and stderr at `path`, appending, and rotates it first if it has
// grown past a megabyte. stdin becomes /dev/null.
bool redirectOutput(const std::string& path, std::string* error);

// Drops one specific GDK warning from the log, and says so once.
//
// `gdk_wayland_toplevel_compute_size: runtime check failed: (size.width > 0)`
// comes out of GDK's xdg-toplevel path, which assumes a surface that decides
// its own width. Ours is a layer surface anchored to both side edges, so the
// compositor decides it and GDK has nothing to compute - the check fires, the
// function returns early, and that early return is the correct outcome. It is
// not ours to fix and not ours to avoid either: it happens on any layout pass,
// so the only way to stop it would be to stop moving the speech bubble while
// she walks. Left unfiltered it was 30-40% of every line in the log.
//
// ASUNA_LOG_ALL=1 turns the filter off.
void installLogFilter();

// The last `lines` lines of `path` - what to show a user whose daemon failed to
// come up, since the reason is in there and they should not have to be told
// where "there" is.
std::string tailLog(const std::string& path, int lines);

// Resident set size in KiB, or 0 if /proc is not readable.
long rssKb();

// --- naming a process that is not ours --------------------------------------
//
// The extension helper cannot be held to the Lock above: it is somebody else's
// program - `[ext] command` can name anything - so we cannot make it take a
// lock on our behalf, and all we have to remember it by is a pid file.
//
// A bare pid is not an identity. Pids are reused, so a pid file outliving its
// process names whatever occupies that number next, and `ext stop` on a stale
// file would signal a stranger. What makes it an identity is the process's
// start time: the kernel's own record of when the pid was handed out, so a
// reused pid is a different start time, always. Pid plus start time is unique
// for as long as the machine has been up, which is longer than any pid file we
// write can be meaningful for.
//
// pidfd_open was the other candidate and does not fit: a pidfd is a live handle
// held by a running process, and the thing that has to do the remembering here
// is a pid file read by a CLI that exits between every command.

// Field 22 of /proc/<pid>/stat - when the process started, in clock ticks since
// boot. 0 if there is no such process or /proc cannot be read, which callers
// must treat as "cannot verify" rather than as a value to compare.
unsigned long long startTime(pid_t pid);

// What a pid file turned out to name.
enum class Owner {
    kAlive,       // the process is there and is the one that was recorded
    kGone,        // no such process; the file is stale and can be removed
    kRecycled,    // that pid exists but started at a different time: not ours
    kUnverified,  // the process is there, but written by an older build that
                  // recorded no start time, so this is the old kill(pid, 0)
};

// Writes `pid` and its start time to `path`, via a temporary and a rename so a
// reader can never see half a line. False if it could not be written.
bool writePidFile(const std::string& path, pid_t pid);

// Reads `path` and says what it names. `pid` gets the recorded pid whatever the
// verdict, so a caller can report the number in a stale file.
Owner readPidFile(const std::string& path, pid_t* pid);

}  // namespace daemon
}  // namespace asuna
