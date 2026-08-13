"""Unix control-socket requests and event subscriptions."""

import json
import os
import socket

from .log import log


def socket_path():
    """The same path paths.cpp computes, by the same rules."""
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    base = os.path.join(runtime, "asuna") if runtime else "/tmp/asuna-%d" % os.getuid()
    return os.path.join(base, "control.sock")


class Control:
    """Her control socket: one request, one reply, connection closed.

    `subscribe` is the exception - see the protocol note in app/ipc.hpp. The
    subscribing socket must keep its write end open, because a half-close is
    how the daemon is told a subscriber has gone.
    """

    def __init__(self, path):
        self.path = path

    def call(self, cmd, **args):
        request = {"cmd": cmd}
        if args:
            request["args"] = args
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(10)
            s.connect(self.path)
            s.sendall((json.dumps(request, ensure_ascii=False) + "\n").encode())
            s.shutdown(socket.SHUT_WR)
            buf = b""
            while b"\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
        if not buf:
            raise IOError("she closed the connection without replying")
        reply = json.loads(buf.split(b"\n", 1)[0].decode())
        if not reply.get("ok"):
            raise IOError(reply.get("error", "the command failed"))
        return reply.get("data", {})

    def events(self):
        """Yields her events until she hangs up or the socket is closed."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.path)
        s.sendall(b'{"cmd":"subscribe"}\n')
        # Note the absence of a shutdown(SHUT_WR) here: that is how every other
        # request ends, and on this one it would tell her the subscriber has
        # gone before it has heard anything.
        buf = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    yield json.loads(line.decode())
                except ValueError:
                    log("ignoring unreadable line:", line[:120])
