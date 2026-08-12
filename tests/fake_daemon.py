#!/usr/bin/env python3
"""A control socket that answers with whatever it was told to, and nothing else.

Some of what the CLI does only happens once a daemon has replied - printing the
warnings that came back from `config reload`, or combining the daemon's
subscriber count with this end's pid file into one `ext status` verdict. Those
paths had no test at all, because testing them appeared to need her running: the
alternative would have been a suite that reloads somebody's real config and
reports on their real helper.

This is the third option. It speaks the client half of app/ipc.cpp - accept,
read one line, write one line, close - and says exactly what the case under test
needs said. Nothing here starts a pet, and the socket is in a temporary
directory the test removes.

    fake_daemon.py <socket path> <reply file>

`reply file` holds one JSON reply line per request, in order; the last line is
reused if more requests arrive than there are lines. Serves until killed.
"""

import os
import socket
import sys


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    path, replies_path = sys.argv[1], sys.argv[2]

    with open(replies_path, encoding="utf-8") as f:
        replies = [line.rstrip("\n") for line in f if line.strip()]
    if not replies:
        sys.stderr.write("fake_daemon: %s has no replies in it\n" % replies_path)
        return 2

    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.unlink(path)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(path)
    server.listen(8)
    # The parent waits for this line before running the client, so there is no
    # sleep anywhere in the test and no race about which came first.
    sys.stdout.write("ready\n")
    sys.stdout.flush()

    served = 0
    try:
        while True:
            conn, _ = server.accept()
            with conn:
                # The client half-closes after its request, so reading to EOF is
                # enough and there is no need to look for the newline.
                while conn.recv(4096):
                    pass
                reply = replies[min(served, len(replies) - 1)]
                served += 1
                conn.sendall(reply.encode("utf-8") + b"\n")
    except (KeyboardInterrupt, OSError):
        pass
    finally:
        server.close()
        try:
            os.unlink(path)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
