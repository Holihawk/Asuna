// The control protocol, asserted without a compositor.
//
// Phase 5's socket is the one part of the pet a person can drive by hand, which
// means it is also the one part where a mistake is a mistake in someone else's
// script rather than a wobble on screen. None of it needs a GPU or a Wayland
// session: a Server, a GMainLoop and a client on a second thread exercise the
// whole path - encode, write, frame, parse, dispatch, reply - in a few
// milliseconds.
//
// What is deliberately not here: the commands themselves. Those need a Shell,
// and a Shell needs a compositor. The line between the two is exactly
// Shell::handleCommand, and everything on this side of it is tested here.

#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "app/ipc.hpp"
#include "paths.hpp"

namespace {

int gFailures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        printf("  FAIL  %s\n", what.c_str());
        ++gFailures;
    }
}

void checkEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) {
        printf("  FAIL  %s\n        got  %s\n        want %s\n", what.c_str(), got.c_str(),
               want.c_str());
        ++gFailures;
    }
}

// --- the writer -------------------------------------------------------------

void testQuoting() {
    printf("quoting\n");
    checkEq(asuna::ipc::quote("hello"), "\"hello\"", "plain text");
    checkEq(asuna::ipc::quote("a\"b"), "\"a\\\"b\"", "an embedded quote");
    checkEq(asuna::ipc::quote("a\\b"), "\"a\\\\b\"", "a backslash");
    checkEq(asuna::ipc::quote("one\ntwo"), "\"one\\ntwo\"", "a newline - which would"
            " otherwise end the message early, so this one is load-bearing");
    checkEq(asuna::ipc::quote("\x01"), "\"\\u0001\"", "a control character");
    // Her dialogue is Chinese, and a \uXXXX-escaping encoder would turn every
    // reply into something no human can check by eye.
    checkEq(asuna::ipc::quote("早安"), "\"早安\"", "UTF-8 passes through intact");
}

void testOut() {
    printf("building objects\n");
    asuna::ipc::Out out;
    out.str("cmd", "say").integer("n", 42).boolean("list", true).num("x", 1.5);
    checkEq(out.done(), "{\"cmd\":\"say\",\"n\":42,\"list\":true,\"x\":1.5}", "a flat object");

    checkEq(asuna::ipc::Out().done(), "{}", "an empty object");
    checkEq(asuna::ipc::Out::array({"a", "b"}), "[\"a\",\"b\"]", "an array of strings");
    checkEq(asuna::ipc::Out::array({}), "[]", "an empty array");
    checkEq(asuna::ipc::ok(), "{\"ok\":true}", "a bare acknowledgement");
    checkEq(asuna::ipc::ok("{\"x\":1}"), "{\"ok\":true,\"data\":{\"x\":1}}", "ok with data");
    checkEq(asuna::ipc::fail("no"), "{\"ok\":false,\"error\":\"no\"}", "a failure");
    checkEq(asuna::ipc::request("hide"), "{\"cmd\":\"hide\"}", "a request with no arguments");
    checkEq(asuna::ipc::request("say", "{\"text\":\"hi\"}"),
            "{\"cmd\":\"say\",\"args\":{\"text\":\"hi\"}}", "a request with arguments");
}

// Everything the writer produces has to survive the reader, or the two halves
// of the protocol are only agreeing by luck.
void testRoundTripThroughTheParser() {
    printf("write, then read back\n");
    const std::string line =
        asuna::ipc::ok(asuna::ipc::Out()
                           .str("text", "她说：\"你好\"\n再见")
                           .num("scale", 0.75)
                           .boolean("hidden", false)
                           .raw("models", asuna::ipc::Out::array({"02", "31"}))
                           .done());
    std::string error;
    const asuna::Json parsed = asuna::Json::parse(line, &error);
    check(error.empty(), "it parses: " + error);
    check(parsed["ok"].asBool(), "ok survives");
    checkEq(parsed["data"]["text"].asString(), "她说：\"你好\"\n再见",
            "quotes and newlines come back exactly");
    check(parsed["data"]["scale"].asNumber() == 0.75, "the number comes back");
    check(!parsed["data"]["hidden"].asBool(), "false is false, not missing");
    checkEq(parsed["data"]["models"][1].asString(), "31", "the array comes back");
}

// --- the socket -------------------------------------------------------------

std::string tempSocket(const char* name) {
    return "/tmp/asuna-test-" + std::to_string(getpid()) + "-" + name + ".sock";
}

// Runs `body` on a second thread while a main loop turns, and stops the loop
// when it finishes. The client half of ipc:: is blocking by design - a CLI has
// nothing else to do - so it cannot share a thread with the server it is
// talking to.
void withLoop(const std::function<void()>& body) {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    std::thread worker([&] {
        body();
        g_main_loop_quit(loop);
    });
    g_main_loop_run(loop);
    worker.join();
    g_main_loop_unref(loop);
}

void testRequestResponse() {
    printf("a request and a reply\n");
    const std::string path = tempSocket("basic");
    asuna::ipc::Server server;
    std::string error;
    std::vector<std::string> seen;

    check(server.start(path, [&](const asuna::Json& req) {
              seen.push_back(req["cmd"].asString());
              if (req["cmd"].asString() == "echo")
                  return asuna::ipc::ok(
                      asuna::ipc::Out().str("text", req["args"]["text"].asString()).done());
              return asuna::ipc::fail("unknown command");
          }, &error),
          "the server starts: " + error);

    check(asuna::ipc::alive(path), "alive() sees it");

    withLoop([&] {
        std::string reply, callError;
        check(asuna::ipc::call(path,
                               asuna::ipc::request("echo", "{\"text\":\"早安\"}"), &reply,
                               &callError, 2000),
              "the call goes through: " + callError);
        checkEq(reply, "{\"ok\":true,\"data\":{\"text\":\"早安\"}}", "the reply comes back whole");

        // A second connection on the same server: one request per connection is
        // the protocol, so this is the ordinary case rather than an edge one.
        check(asuna::ipc::call(path, asuna::ipc::request("nope"), &reply, &callError, 2000),
              "a second call: " + callError);
        checkEq(reply, "{\"ok\":false,\"error\":\"unknown command\"}", "a refusal comes back");

        // Garbage must be answered, not dropped: a client that sends nonsense
        // and gets silence back cannot tell that from a daemon that has hung.
        check(asuna::ipc::call(path, "this is not json", &reply, &callError, 2000),
              "malformed input still gets an answer: " + callError);
        const asuna::Json parsed = asuna::Json::parse(reply);
        check(parsed.isObject() && !parsed["ok"].asBool(), "and the answer is a failure");
    });

    check(seen.size() == 2, "the handler saw exactly the two well-formed requests");
    server.stop();
    check(!asuna::ipc::alive(path), "and it is gone once stopped");
}

void testNobodyListening() {
    printf("nobody listening\n");
    const std::string path = tempSocket("absent");
    unlink(path.c_str());
    check(!asuna::ipc::alive(path), "alive() says no");
    std::string reply, error;
    check(!asuna::ipc::call(path, asuna::ipc::request("ping"), &reply, &error, 500),
          "the call fails");
    checkEq(error, "not running", "with the message the CLI turns into its own");
}

// A socket file left behind by a process that was killed. The lock, not the
// socket, is what says whether a daemon is running - so starting has to be able
// to take the path back.
void testStaleSocketFile() {
    printf("a stale socket file\n");
    const std::string path = tempSocket("stale");
    {
        asuna::ipc::Server first;
        std::string error;
        check(first.start(path, [](const asuna::Json&) { return asuna::ipc::ok(); }, &error),
              "the first server starts: " + error);
        // Leak the socket file the way a SIGKILL would: no stop(), no unlink.
        first.~Server();
        new (&first) asuna::ipc::Server();
    }
    check(access(path.c_str(), F_OK) != 0 || true, "the file may or may not still be there");

    asuna::ipc::Server second;
    std::string error;
    check(second.start(path, [](const asuna::Json&) {
              return asuna::ipc::ok(asuna::ipc::Out().integer("n", 2).done());
          }, &error),
          "a second server binds over it: " + error);
    withLoop([&] {
        std::string reply, callError;
        check(asuna::ipc::call(path, asuna::ipc::request("ping"), &reply, &callError, 2000),
              "and answers: " + callError);
        checkEq(reply, "{\"ok\":true,\"data\":{\"n\":2}}", "as the new server");
    });
    second.stop();
}

// The reply has to survive being longer than one read - `asuna model list` with
// 42 outfits is already a kilobyte, and there is nothing stopping it growing.
void testLargeReply() {
    printf("a reply larger than a buffer\n");
    const std::string path = tempSocket("large");
    std::vector<std::string> many;
    for (int i = 0; i < 4000; ++i) many.push_back("outfit-" + std::to_string(i));

    asuna::ipc::Server server;
    std::string error;
    check(server.start(path, [&](const asuna::Json&) {
              return asuna::ipc::ok(
                  asuna::ipc::Out().raw("models", asuna::ipc::Out::array(many)).done());
          }, &error),
          "the server starts: " + error);
    withLoop([&] {
        std::string reply, callError;
        check(asuna::ipc::call(path, asuna::ipc::request("model"), &reply, &callError, 4000),
              "the call goes through: " + callError);
        const asuna::Json parsed = asuna::Json::parse(reply);
        check(parsed["data"]["models"].size() == many.size(),
              "every element arrives (" + std::to_string(parsed["data"]["models"].size()) +
                  " of " + std::to_string(many.size()) + ")");
        checkEq(parsed["data"]["models"][3999].asString(), "outfit-3999", "including the last");
    });
    server.stop();
}

// --- subscriptions ----------------------------------------------------------
//
// The one connection that is not a request and a reply. Everything about it is
// a departure from the rest of the protocol, so it is worth asserting the shape
// rather than just the delivery: the reply still comes first, events arrive one
// per line after it, an ordinary caller is unaffected and is not counted, and
// the daemon finds out when the subscriber goes away - which is the half that
// nothing else can tell it.

// A raw client, because ipc::call() half-closes after its request and a
// subscriber that does that has told the server it is leaving.
int subscribeRaw(const std::string& path) {
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path.c_str(), path.size());
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const std::string request = asuna::ipc::request("subscribe") + "\n";
    if (write(fd, request.data(), request.size()) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Reads one newline-terminated line, or "" if the two-second timeout passes.
std::string readLine(int fd, std::string* carry) {
    for (;;) {
        const size_t eol = carry->find('\n');
        if (eol != std::string::npos) {
            const std::string line = carry->substr(0, eol);
            carry->erase(0, eol + 1);
            return line;
        }
        char buf[512];
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return "";
        carry->append(buf, static_cast<size_t>(n));
    }
}

void testSubscription() {
    printf("subscribing to events\n");
    const std::string path = tempSocket("events");
    asuna::ipc::Server server;
    std::string error;
    std::atomic<int> changes{0};

    check(server.start(path, [&](const asuna::Json& req) {
              if (req["cmd"].asString() == "subscribe")
                  return asuna::ipc::Reply::subscription(asuna::ipc::ok());
              return asuna::ipc::Reply(asuna::ipc::ok(
                  asuna::ipc::Out().integer("subscribers", server.subscribers()).done()));
          }, &error),
          "the server starts: " + error);
    server.onSubscribersChanged([&] { ++changes; });

    // Nobody is listening yet. Publishing into that must be a no-op rather than
    // anything at all - it is called from her frame loop several times a
    // second, and the usual state of the machine is that there is no helper.
    server.publish("{\"event\":\"nobody\"}");
    check(server.subscribers() == 0, "no subscribers before anyone subscribes");

    withLoop([&] {
        const int fd = subscribeRaw(path);
        check(fd >= 0, "the subscriber connects");
        std::string carry;
        checkEq(readLine(fd, &carry), "{\"ok\":true}", "the reply still comes first");

        // publish() belongs to the main loop, like everything else the daemon
        // does; scheduling it from here is how the real thing reaches it too.
        static asuna::ipc::Server* target = nullptr;
        target = &server;
        g_idle_add([](gpointer) -> gboolean {
            target->publish("{\"event\":\"touch\",\"area\":\"head\"}");
            target->publish("{\"event\":\"outfit\",\"id\":\"31\"}");
            return G_SOURCE_REMOVE;
        }, nullptr);

        checkEq(readLine(fd, &carry), "{\"event\":\"touch\",\"area\":\"head\"}",
                "the first event arrives");
        checkEq(readLine(fd, &carry), "{\"event\":\"outfit\",\"id\":\"31\"}",
                "and the second, in order and one per line");

        // An ordinary caller alongside it. Its reply must not go to the
        // subscriber, and the subscriber's events must not go to it.
        std::string reply, callError;
        check(asuna::ipc::call(path, asuna::ipc::request("count"), &reply, &callError, 2000),
              "an ordinary call still works: " + callError);
        checkEq(reply, "{\"ok\":true,\"data\":{\"subscribers\":1}}",
                "one subscriber counted, and the plain caller is not one of them");

        g_idle_add([](gpointer) -> gboolean {
            target->publish("{\"event\":\"bye\"}");
            return G_SOURCE_REMOVE;
        }, nullptr);
        checkEq(readLine(fd, &carry), "{\"event\":\"bye\"}",
                "the subscriber's stream is unaffected by the other connection");

        check(changes.load() == 1, "one subscriber arriving is reported once");

        // Going away. Nothing asks the daemon about this - the helper is a
        // separate process that can be killed - so the drop is the only way it
        // ever finds out, and it is what her own voice comes back on.
        close(fd);
        for (int i = 0; i < 40 && changes.load() < 2; ++i) usleep(25 * 1000);
        check(changes.load() == 2, "and its leaving is reported too");
        check(server.subscribers() == 0, "leaving nobody subscribed");
    });
    server.stop();
}

// --- paths ------------------------------------------------------------------

void testPaths() {
    printf("paths\n");
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("XDG_STATE_HOME", "/tmp/asuna-test-state", 1);
    checkEq(asuna::paths::socketPath(), "/tmp/asuna/control.sock", "the socket path");
    checkEq(asuna::paths::lockPath(), "/tmp/asuna/asuna.lock", "the lock path");
    checkEq(asuna::paths::logPath(), "/tmp/asuna-test-state/asuna/asuna.log", "the log path");
    // The state directory must not be created just by asking where it is: a
    // --no-persist run should leave nothing behind.
    check(access("/tmp/asuna-test-state", F_OK) != 0,
          "asking for the state path does not create it");
}

}  // namespace

int main() {
    testQuoting();
    testOut();
    testRoundTripThroughTheParser();
    testRequestResponse();
    testNobodyListening();
    testStaleSocketFile();
    testLargeReply();
    testSubscription();
    testPaths();

    if (gFailures) {
        printf("\n%d failure(s)\n", gFailures);
        return 1;
    }
    printf("\nipc: all checks passed\n");
    return 0;
}
