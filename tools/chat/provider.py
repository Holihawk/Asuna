"""OpenAI-compatible HTTP access and provider error reporting."""

import http.client
import json
import os
import time
import urllib.error
import urllib.request

# Non-streaming provider probes get a short connect/first-byte budget, so a dead
# provider delays the live one behind it by at most this long. Streaming chat
# requests use READ_TIMEOUT instead, because generation can legitimately take
# longer between response chunks.
CONNECT_TIMEOUT = 8.0
READ_TIMEOUT = 60.0


class Provider:
    """One place to reach a model, and whether it is currently worth trying."""

    def __init__(self, spec):
        self.name = spec.get("name", "?")
        self.base_url = spec.get("base_url", "").rstrip("/")
        self.model = spec.get("model", "")
        # A key given as `api_key` in the config file arrives over the socket; a
        # key given as `api_key_env` never does, and is read here out of this
        # process's own environment. The second is the one to prefer, and the
        # only one where the key is never anywhere but here.
        self.key = spec.get("api_key") or os.environ.get(spec.get("api_key_env") or "", "")
        self.key_from = (
            "config file" if spec.get("api_key_here")
            else ("$" + spec["api_key_env"] if spec.get("api_key_env") else "none")
        )
        self.blocked_until = 0.0

    def ready(self):
        return time.time() >= self.blocked_until

    def request(self, body, stream=True):
        payload = json.dumps(body, ensure_ascii=False).encode()
        headers = {"Content-Type": "application/json"}
        if self.key:
            headers["Authorization"] = "Bearer " + self.key
        request = urllib.request.Request(
            self.base_url + "/chat/completions", data=payload, headers=headers
        )
        return urllib.request.urlopen(request, timeout=READ_TIMEOUT if stream else CONNECT_TIMEOUT)


def describe(error):
    """A failed attempt in a form worth putting in front of a person.

    The three kinds arrive here as one exception type each, and they mean
    different things to whoever reads the log: an HTTP status is the endpoint
    answering and objecting - usually a key or a quota, and it says which if it
    can be asked - while a URLError with no status never reached one, and
    anything else is a malformed reply.
    """
    if isinstance(error, urllib.error.HTTPError):
        detail = ""
        # Reading the body is a courtesy, so nothing it can do is worth losing
        # the status code over. Narrow rather than bare, though: json.loads
        # raises ValueError, a body that is a JSON string rather than an object
        # makes `.get` an AttributeError, and the read itself can fail at the
        # socket. A NameError in this block is a bug and should surface as one.
        try:
            body = json.loads(error.read().decode("utf-8", "replace"))
            detail = body.get("error", {}).get("message", "") if isinstance(body, dict) else ""
        except (ValueError, AttributeError, OSError, http.client.HTTPException):
            pass
        return "HTTP %d%s" % (error.code, ": " + detail if detail else "")
    if isinstance(error, urllib.error.URLError):
        return "did not connect: %s" % (error.reason,)
    return "unreadable reply: %s" % (error,)
