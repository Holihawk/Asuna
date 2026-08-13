#include "app/cli/internal.hpp"

#include <cstdio>

#include "app/daemon.hpp"
#include "app/ipc.hpp"
#include "paths.hpp"

namespace asuna {
namespace cli {

// --- talking to the daemon --------------------------------------------------

// Sends one command. Prints whatever went wrong, so callers only ever handle
// success; `status` comes back as the exit code to use.
bool send(const std::string& cmd, const std::string& args, Json* data, int* status,
          bool quietWhenAbsent) {
    std::string reply, error;
    if (!ipc::call(paths::socketPath(), ipc::request(cmd, args), &reply, &error)) {
        // "not running" from the socket is only half an answer: a daemon whose
        // socket is gone but whose lock is held is wedged, not absent, and
        // saying "not running" would send the user looking in the wrong place.
        if (error == "not running" && daemon::holderPid(paths::lockPath()) > 0) {
            error = "the control socket is not answering, but a daemon is running"
                    " (`asuna exit` will signal it)";
        } else if (error == "not running") {
            *status = kNotRunning;
            // `asuna status` says it better, on stdout, where the answer to a
            // question belongs.
            if (quietWhenAbsent) return false;
        }
        fprintf(stderr, "asuna: %s\n", error.c_str());
        return false;
    }
    if (gJson) printf("%s\n", reply.c_str());
    std::string parseError;
    const Json parsed = Json::parse(reply, &parseError);
    if (!parsed.isObject())
        return !complain("unreadable reply: " + (parseError.empty() ? reply : parseError));
    // Handed back before the verdict, because a refusal can carry detail too -
    // `config reload` sends the list of problems with it, and making the caller
    // ask again for a state the daemon has already computed would be asking a
    // question it has already answered.
    if (data) *data = parsed["data"];
    if (!parsed["ok"].asBool()) {
        const std::string why = parsed["error"].asString();
        fprintf(stderr, "asuna: %s\n", why.empty() ? "the command failed" : why.c_str());
        return false;
    }
    return true;
}

// The common shape: send it, say nothing if it worked. Most verbs are this.
int simple(const std::string& cmd, const std::string& args) {
    int status = kError;
    Json data;
    return send(cmd, args, &data, &status) ? kOk : status;
}

}  // namespace cli
}  // namespace asuna
