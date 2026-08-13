#include "app/cli/internal.hpp"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "app/ipc.hpp"
#include "paths.hpp"

namespace asuna {
namespace cli {

// Prints her events as they happen, until interrupted. The debugging companion
// to the helper - and the answer to "is anything actually coming out of this",
// which is otherwise a question only a Python script can ask.
int cmdSubscribe() {
    std::string reply, error;
    // Its own connection rather than ipc::call, which half-closes after the
    // request; a subscriber that does that is telling the daemon it has gone.
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return complain(std::string("socket: ") + strerror(errno));
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const std::string path = paths::socketPath();
    if (path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        return complain("socket path too long: " + path);
    }
    memcpy(addr.sun_path, path.c_str(), path.size());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return complain("not running", kNotRunning);
    }
    const std::string request = ipc::request("subscribe") + "\n";
    if (write(fd, request.data(), request.size()) < 0) {
        close(fd);
        return complain("could not send the request");
    }
    std::string buffer;
    char chunk[1024];
    for (;;) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        for (size_t eol; (eol = buffer.find('\n')) != std::string::npos;) {
            printf("%s\n", buffer.substr(0, eol).c_str());
            fflush(stdout);
            buffer.erase(0, eol + 1);
        }
    }
    close(fd);
    fprintf(stderr, "asuna: she hung up\n");
    return kOk;
}

}  // namespace cli
}  // namespace asuna
