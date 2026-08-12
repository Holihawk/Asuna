#include "app/ipc.hpp"

#include <errno.h>
#include <glib-unix.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace asuna {
namespace ipc {
namespace {

// A request is one short line. Anything past this is a client that has lost the
// plot, and the connection is dropped rather than buffered indefinitely.
constexpr size_t kMaxRequest = 64 * 1024;
// How long a connection may sit without completing a line.
constexpr unsigned kConnTimeoutMs = 5000;

bool fillAddress(const std::string& path, sockaddr_un* addr, std::string* error) {
    if (path.size() >= sizeof(addr->sun_path)) {
        if (error) *error = "socket path too long: " + path;
        return false;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path.c_str(), path.size());
    return true;
}

// Writes everything, waiting on the fd when it will not take more. Replies are
// a line of a few hundred bytes against a socket buffer measured in hundreds of
// kilobytes, so this only ever loops against a client that has stopped reading -
// and then it gives up rather than stalling the animation.
//
// send(MSG_NOSIGNAL) rather than write(): writing to a socket whose peer has
// gone raises SIGPIPE, whose default action is to kill the process. Harmless
// while every connection was one request and one reply, and not harmless at all
// once a subscription can sit open across a helper being killed.
bool writeAll(int fd, const std::string& data, int timeoutMs) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, timeoutMs) <= 0) return false;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

}  // namespace

// --- writing ----------------------------------------------------------------

// The wire format and the state file are both JSON, so they escape through the
// same function - see Json::quote, which sits next to the decoder that has to
// agree with it. Kept as a name here because the protocol tests and every
// Out::str call already say `quote`.
std::string quote(const std::string& s) { return Json::quote(s); }

Out& Out::raw(const char* key, const std::string& json) {
    if (!mBody.empty()) mBody += ',';
    mBody += quote(key);
    mBody += ':';
    mBody += json;
    return *this;
}

Out& Out::str(const char* key, const std::string& value) { return raw(key, quote(value)); }

Out& Out::num(const char* key, double value) {
    // JSON has no spelling for infinity or NaN, and "%.6g" would write `inf` or
    // `nan` - which the parser at the other end refuses, taking the whole reply
    // down with it over one member. Nothing validated reaches here non-finite;
    // this is the writer keeping the promise that everything we send, we can
    // read. `null` rather than a made-up number, because there is no number.
    if (!std::isfinite(value)) return raw(key, "null");
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", value);
    return raw(key, buf);
}

Out& Out::integer(const char* key, long long value) {
    return raw(key, std::to_string(value));
}

Out& Out::boolean(const char* key, bool value) { return raw(key, value ? "true" : "false"); }

std::string Out::array(const std::vector<std::string>& strings) {
    std::string out = "[";
    for (size_t i = 0; i < strings.size(); ++i) {
        if (i) out += ',';
        out += quote(strings[i]);
    }
    return out + "]";
}

std::string Out::rawArray(const std::vector<std::string>& values) {
    std::string out = "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ',';
        out += values[i];
    }
    return out + "]";
}

std::string ok(const std::string& data) {
    return data.empty() ? std::string("{\"ok\":true}")
                        : "{\"ok\":true,\"data\":" + data + "}";
}

std::string fail(const std::string& message, const std::string& data) {
    std::string line = "{\"ok\":false,\"error\":" + quote(message);
    // A failure may still have something to say beyond why. `config reload`
    // carries the list of problems this way, so the client can print them
    // without a second round trip for a state that has already been computed.
    if (!data.empty()) line += ",\"data\":" + data;
    return line + "}";
}

std::string request(const std::string& cmd, const std::string& args) {
    Out out;
    out.str("cmd", cmd);
    if (!args.empty()) out.raw("args", args);
    return out.done();
}

// --- server -----------------------------------------------------------------

struct Server::Conn {
    Server* server = nullptr;
    int fd = -1;
    guint watch = 0;
    guint timer = 0;
    std::string buffer;
    bool subscribed = false;
};

Server::~Server() { stop(); }

bool Server::start(const std::string& path, Handler handler, std::string* error) {
    stop();

    sockaddr_un addr;
    if (!fillAddress(path, &addr, error)) return false;

    mFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (mFd < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return false;
    }

    // A socket file left behind by a daemon that was killed rather than asked
    // to leave. Removing it is only safe because the caller holds the
    // single-instance lock, which is the whole reason the lock is a separate
    // file: it answers "is one running" without anything having to guess
    // whether this socket is stale.
    unlink(path.c_str());

    // 0700 on the directory already, but the socket's own mode is what stops
    // another user on a shared machine from driving the pet.
    const mode_t saved = umask(0177);
    const int rc = bind(mFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    umask(saved);
    if (rc < 0) {
        if (error) *error = "bind " + path + ": " + strerror(errno);
        close(mFd);
        mFd = -1;
        return false;
    }
    if (::listen(mFd, 8) < 0) {
        if (error) *error = std::string("listen: ") + strerror(errno);
        close(mFd);
        mFd = -1;
        unlink(path.c_str());
        return false;
    }

    mPath = path;
    mHandler = std::move(handler);
    mWatch = g_unix_fd_add(mFd, G_IO_IN, onListenReadable, this);
    return true;
}

void Server::stop() {
    mOnChange = nullptr;   // see closeConn
    if (mWatch) g_source_remove(mWatch);
    mWatch = 0;
    if (mFd >= 0) close(mFd);
    mFd = -1;
    while (!mConns.empty()) closeConn(mConns.back());
    // Only ever our own: bind() would have failed if it were somebody else's,
    // and the lock keeps a second daemon from having got that far.
    if (!mPath.empty()) unlink(mPath.c_str());
    mPath.clear();
}

gboolean Server::onListenReadable(gint fd, GIOCondition, gpointer data) {
    auto* self = static_cast<Server*>(data);
    for (;;) {
        const int cfd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            // EAGAIN is the normal end of the accept loop; anything else is a
            // per-connection failure that must not take the listener down.
            return G_SOURCE_CONTINUE;
        }
        auto* conn = new Conn{self, cfd, 0, 0, {}, false};
        self->mConns.push_back(conn);
        conn->watch = g_unix_fd_add(
            cfd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), onConnReadable, conn);
        // A connection that never completes a line must not sit here forever
        // holding an fd; the client is a one-shot request, so five seconds is
        // already generous.
        conn->timer = g_timeout_add(kConnTimeoutMs, onConnTimeout, conn);
    }
}

gboolean Server::onConnTimeout(gpointer data) {
    auto* conn = static_cast<Conn*>(data);
    conn->timer = 0;   // GLib destroys it when this returns G_SOURCE_REMOVE
    closeConn(conn);
    return G_SOURCE_REMOVE;
}

gboolean Server::onConnReadable(gint fd, GIOCondition condition, gpointer data) {
    auto* conn = static_cast<Conn*>(data);
    // Every path out of here that closes the connection returns
    // G_SOURCE_REMOVE, so the watch is always the source destroying itself.
    const auto finish = [conn] {
        conn->watch = 0;
        closeConn(conn);
        return G_SOURCE_REMOVE;
    };

    bool hangup = (condition & (G_IO_HUP | G_IO_ERR)) != 0;

    if (condition & G_IO_IN) {
        char buf[4096];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn->buffer.append(buf, static_cast<size_t>(n));
                if (conn->buffer.size() > kMaxRequest) {
                    writeAll(fd, fail("request too large") + "\n", 500);
                    return finish();
                }
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            // 0 is the client half-closing after its request, which is exactly
            // what our own client does, so it is an end of input rather than an
            // error - but there is nothing more coming either way.
            if (n == 0) hangup = true;
            else if (n < 0 && (errno != EAGAIN && errno != EWOULDBLOCK)) return finish();
            break;
        }
    }

    // A subscriber has said everything it is going to say. Anything further on
    // this connection is ignored, and the only thing left to watch for is it
    // going away - which is exactly what a hangup here is.
    if (conn->subscribed) {
        conn->buffer.clear();
        return hangup ? finish() : G_SOURCE_CONTINUE;
    }

    const size_t eol = conn->buffer.find('\n');
    if (eol == std::string::npos) {
        // A hangup with no complete line is a client that changed its mind.
        if (hangup) return finish();
        return G_SOURCE_CONTINUE;
    }

    const std::string line = conn->buffer.substr(0, eol);
    std::string error;
    const Json req = Json::parse(line, &error);
    // The handler can re-enter the main loop (`exit` runs her farewell on a
    // timer before the loop actually stops), so the reply is produced and sent
    // before anything else is touched.
    const Reply reply =
        req.isObject() ? conn->server->mHandler(req)
                       : Reply(fail("malformed request: " +
                                    (error.empty() ? "not an object" : error)));
    if (!writeAll(fd, reply.line + "\n", 2000) || !reply.subscribe) return finish();

    // From here it is a subscriber. The timeout has to go with the rest of the
    // request machinery: it exists to stop a connection sitting on an fd
    // without completing a line, and sitting there is now the whole job.
    conn->subscribed = true;
    conn->buffer.clear();
    if (conn->timer) g_source_remove(conn->timer);
    conn->timer = 0;
    // A client that half-closed after its request - which is what our own
    // client does - has already told us it is not staying.
    if (hangup) return finish();
    if (conn->server->mOnChange) conn->server->mOnChange();
    return G_SOURCE_CONTINUE;
}

void Server::publish(const std::string& line) {
    if (mConns.empty()) return;
    const std::string framed = line + "\n";
    // By value: closeConn() erases from mConns, so iterating it directly would
    // be walking a vector that a failed write is rearranging underneath.
    std::vector<Conn*> targets;
    for (Conn* conn : mConns)
        if (conn->subscribed) targets.push_back(conn);
    for (Conn* conn : targets) {
        // A short wait, not none: the buffer is measured in hundreds of
        // kilobytes against events of a few dozen bytes, so this can only block
        // on a subscriber that has stopped reading entirely - and then it is
        // dropped rather than allowed to hold up a frame.
        if (!writeAll(conn->fd, framed, 200)) closeConn(conn);
    }
}

int Server::subscribers() const {
    int n = 0;
    for (const Conn* conn : mConns)
        if (conn->subscribed) ++n;
    return n;
}

void Server::closeConn(Conn* conn) {
    if (conn->watch) g_source_remove(conn->watch);
    if (conn->timer) g_source_remove(conn->timer);
    if (conn->fd >= 0) close(conn->fd);
    Server* server = conn->server;
    const bool wasSubscribed = conn->subscribed;
    auto& conns = server->mConns;
    conns.erase(std::remove(conns.begin(), conns.end(), conn), conns.end());
    delete conn;
    // After the erase and the delete, so the count the callback reads is the
    // one that is now true. stop() drops the callback first, which is what
    // keeps a teardown from reporting a helper "gone" to a half-dismantled
    // daemon.
    if (wasSubscribed && server->mOnChange) server->mOnChange();
}

// --- client -----------------------------------------------------------------

namespace {

int connectTo(const std::string& path, int timeoutMs, std::string* error) {
    sockaddr_un addr;
    if (!fillAddress(path, &addr, error)) return -1;

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (error) *error = std::string("socket: ") + strerror(errno);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (error) {
            // ENOENT and ECONNREFUSED are both "nobody is home"; the caller
            // turns that into the one message a user wants to read.
            *error = (errno == ENOENT || errno == ECONNREFUSED)
                         ? "not running"
                         : std::string("connect: ") + strerror(errno);
        }
        close(fd);
        return -1;
    }
    timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

}  // namespace

bool call(const std::string& path, const std::string& req, std::string* reply,
          std::string* error, int timeoutMs) {
    const int fd = connectTo(path, timeoutMs, error);
    if (fd < 0) return false;

    const std::string line = req + "\n";
    if (!writeAll(fd, line, timeoutMs)) {
        if (error) *error = "could not send the request";
        close(fd);
        return false;
    }
    // Half-close, so a server reading to EOF sees the end of the request
    // without having to find the newline first.
    shutdown(fd, SHUT_WR);

    std::string got;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            got.append(buf, static_cast<size_t>(n));
            if (got.find('\n') != std::string::npos) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) break;
        if (error)
            *error = (errno == EAGAIN || errno == EWOULDBLOCK)
                         ? "timed out waiting for a reply"
                         : std::string("read: ") + strerror(errno);
        close(fd);
        return false;
    }
    close(fd);

    const size_t eol = got.find('\n');
    if (got.empty()) {
        if (error) *error = "the daemon closed the connection without replying";
        return false;
    }
    if (reply) *reply = got.substr(0, eol == std::string::npos ? got.size() : eol);
    return true;
}

bool alive(const std::string& path) {
    std::string error;
    const int fd = connectTo(path, 1000, &error);
    if (fd < 0) return false;
    close(fd);
    return true;
}

}  // namespace ipc
}  // namespace asuna
