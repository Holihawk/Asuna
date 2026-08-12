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

// A pid file's contents and what they were found to mean.
//
// Carried whole rather than reduced back to a pid_t. The start time is what
// makes the pid an identity, so handing a caller only the number returns it to
// the position this type exists to get out of - and the caller that most needs
// both is the one about to send a signal.
struct Identity {
    pid_t pid = 0;
    unsigned long long began = 0;   // as recorded in the file; 0 => not recorded
    Owner state = Owner::kGone;

    // Shown to be the process the file meant - not merely a live pid.
    bool proven() const { return state == Owner::kAlive; }
    // Something is there under that number, proven or not. The question
    // `status` asks, and never enough on its own to signal.
    bool present() const { return state == Owner::kAlive || state == Owner::kUnverified; }
};

// Writes `pid` and its start time to `path`, via a temporary and a rename so a
// reader can never see half a line. False if it could not be written.
bool writePidFile(const std::string& path, pid_t pid);

// Reads `path` and says what it names.
Identity readPidFile(const std::string& path);

// An open handle to one particular process, for signalling it and nothing else.
//
// The window this closes: between deciding a pid is the helper and sending it a
// signal, the helper can exit and the kernel can hand its number to something
// unrelated. Checking the start time again only moves the window - the check
// and the kill are still two operations.
//
// A pidfd refers to the *process*, not to the number, so once opened it can
// never come to mean anything else; if the process it names has gone,
// pidfd_send_signal fails with ESRCH rather than reaching a stranger. `open`
// therefore takes the handle first and confirms the start time second: a pid
// recycled before the open is caught by that check, and one recycled after it
// cannot be reached through the handle at all.
//
// This is why a pidfd is useful here even though one cannot be *persisted*
// between `ext start` and `ext stop`. The pid file stays the durable identity;
// the pidfd is opened fresh by whoever is about to signal.
class Signal {
public:
    ~Signal();
    Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    // True only if `id` is proven and still names a live process. An identity
    // the pid file could not prove (kUnverified) is refused: this class exists
    // to send signals, and "probably the right process" is not a standard to
    // kill on.
    bool open(const Identity& id);

    // The same for a pid whose identity cannot be proven - an old pid file, and
    // only ever because the user said so in as many words. Its own name so that
    // every place we signal without proof is one grep away.
    bool openUnproven(pid_t pid);

    // Still the process that was opened, rather than "some process has this
    // number". False once it has exited, even if the pid has been reused.
    bool alive() const;
    bool send(int sig) const;

    // False on a kernel without pidfds, where `send` falls back to re-reading
    // the start time immediately before kill(2) - narrower than no check at
    // all, and not atomic. Reported so a test can tell which path it exercised.
    bool exact() const { return mFd >= 0; }

private:
    void shut();

    pid_t mPid = 0;
    unsigned long long mBegan = 0;
    int mFd = -1;
};

}  // namespace daemon
}  // namespace asuna
