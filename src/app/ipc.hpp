#pragma once

#include <glib.h>

#include <functional>
#include <string>
#include <vector>

#include "json.hpp"

namespace asuna {

// The control socket.
//
// One JSON object per line, in both directions, on a `SOCK_STREAM` AF_UNIX
// socket: `{"cmd":"…","args":{…}}` in, `{"ok":true,"data":{…}}` or
// `{"ok":false,"error":"…"}` out, then the connection closes. There is no
// session and no state on the wire, which is what lets `asuna status` be a
// shell script's business rather than a protocol implementation.
//
// `subscribe` is the single exception: that connection stays open and receives
// `{"event":"…",…}` lines until one end closes it. It is read-only from then
// on - a subscriber that wants to ask her something opens an ordinary
// connection for it, so replies never interleave with events - and it must keep
// its own write end open, because a half-close is how the daemon is told the
// subscriber has gone.
//
// Line-delimited JSON rather than something like D-Bus because the daemon is
// single-user, single-instance and local: a D-Bus name would buy discovery and
// activation we do not want (a pet that respawns when you poke it is a bug),
// and cost a dependency and a schema.
namespace ipc {

// --- writing ---------------------------------------------------------------
// The object builder is here because the protocol is the only thing that needs
// one, and it is deliberately just enough for one flat object of scalars plus
// the odd array. String escaping is *not* here: that is Json::quote, so the one
// function that decides what a quote or a backslash becomes is the one sitting
// next to the decoder that has to undo it. The state file writes through the
// same one.

std::string quote(const std::string& s);   // Json::quote, under the local name

// A JSON object under construction. Values go in already encoded, so nesting is
// `raw(key, other.done())`.
class Out {
public:
    Out& str(const char* key, const std::string& value);
    Out& num(const char* key, double value);
    Out& integer(const char* key, long long value);
    Out& boolean(const char* key, bool value);
    Out& raw(const char* key, const std::string& json);
    bool empty() const { return mBody.empty(); }
    std::string done() const { return "{" + mBody + "}"; }

    static std::string array(const std::vector<std::string>& strings);
    // The same, for elements that are already JSON - an array of objects, which
    // the provider list is and nothing else here is.
    static std::string rawArray(const std::vector<std::string>& values);

private:
    std::string mBody;
};

// Complete response lines. `data` is an object, or "" for a bare acknowledgement.
std::string ok(const std::string& data = "");
// `data` is optional detail about the failure - the list of config problems, for
// instance - so a client does not have to ask a second question to print it.
std::string fail(const std::string& message, const std::string& data = "");

// --- request ---------------------------------------------------------------
std::string request(const std::string& cmd, const std::string& args = "");

// --- server ----------------------------------------------------------------

// What a handler wants done with the connection once it has answered. Almost
// always "reply and hang up", which is what the plain string constructor means,
// so a handler that knows nothing about subscriptions still reads as one.
struct Reply {
    Reply(std::string line = {}) : line(std::move(line)) {}
    // Reply, then leave the connection open and feed it published events until
    // one end closes it. The one exception to "no session state on the wire",
    // and it buys the extension helper the difference between reacting and
    // polling: see `publish`.
    static Reply subscription(std::string line) {
        Reply r(std::move(line));
        r.subscribe = true;
        return r;
    }

    std::string line;
    bool subscribe = false;
};

// Listens on `path` and dispatches each line through `handler` on the GLib main
// context. Only ever run by the daemon, which holds the single-instance lock -
// that is what makes unlinking a socket file left behind by a killed process
// safe rather than a race.
class Server {
public:
    // Returns a complete response line for one request.
    using Handler = std::function<Reply(const Json& request)>;

    ~Server();
    bool start(const std::string& path, Handler handler, std::string* error);
    void stop();

    // Sends one already-encoded object to every subscriber, or does nothing if
    // there are none. A subscriber that will not take it is dropped rather than
    // waited on: this is called from the frame loop, and a wedged client must
    // not be able to stutter her.
    void publish(const std::string& line);
    // How many connections are subscribed. Nothing about the protocol depends
    // on it; the daemon uses it to decide whether anyone is listening at all -
    // which is what her own chatter is suppressed against.
    int subscribers() const;
    // Called after that count changes, on the main loop. A subscriber arriving
    // is visible from the handler, but one *leaving* is not the result of any
    // command - it is a dropped connection - so it needs a way in of its own.
    void onSubscribersChanged(std::function<void()> fn) { mOnChange = std::move(fn); }

private:
    struct Conn;
    static gboolean onListenReadable(gint fd, GIOCondition condition, gpointer self);
    static gboolean onConnReadable(gint fd, GIOCondition condition, gpointer conn);
    static gboolean onConnTimeout(gpointer conn);
    // Removes whichever of the connection's two sources is still armed, closes
    // the fd and frees it. A source destroying itself must zero its own id
    // first, or this would remove it from inside its own callback.
    static void closeConn(Conn* conn);

    std::string mPath;
    Handler mHandler;
    std::function<void()> mOnChange;
    std::vector<Conn*> mConns;
    int mFd = -1;
    guint mWatch = 0;
};

// --- client ----------------------------------------------------------------

// One request, one reply. False (with `error` filled in) if the daemon could
// not be reached, hung up, or did not answer inside `timeoutMs`.
bool call(const std::string& path, const std::string& request, std::string* reply,
          std::string* error, int timeoutMs = 10000);

// Is anything actually listening? A `connect()` probe: the socket file existing
// proves nothing, since a SIGKILLed daemon leaves one behind.
bool alive(const std::string& path);

}  // namespace ipc
}  // namespace asuna
