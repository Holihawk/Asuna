#!/usr/bin/env python3
"""Offline tests for tools/asuna-ext.py - the provider failover in particular.

No network, no daemon, no compositor, no API key. `Provider.request` is the only
thing between this file and an endpoint, so every case here replaces it with a
canned response: a stream of tokens, a stream of nothing, a connection that
refuses, an HTTP status, a body of rubbish.

What is being pinned down is the rule the Chat docstring states and the code
used not to keep: *failover happens before the first token, and never after*.
The gap was the successful-but-empty answer. A provider returning HTTP 200 and
an immediate [DONE] produced no tokens, and that counted as having answered - so
the fallback provider was never tried, and an empty assistant turn went into the
history where it stayed for the rest of the conversation. Both halves are here.

Run directly, or through ctest as the `ext` suite.
"""

import importlib.util
import io
import json
import os
import sys
import threading
import urllib.error

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(os.path.dirname(HERE), "tools", "asuna-ext.py")


def load_helper():
    """Imports asuna-ext.py, whose filename is not a legal module name."""
    spec = importlib.util.spec_from_file_location("asuna_ext", HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ext = load_helper()

failures = 0
current = ""


def check(ok, what):
    global failures
    if ok:
        return
    print("  FAIL %s: %s" % (current, what))
    failures += 1


def check_eq(got, want, what):
    global failures
    if got == want:
        return
    print("  FAIL %s: %s\n    got  %r\n    want %r" % (current, what, got, want))
    failures += 1


# --- canned endpoints -------------------------------------------------------


class FakeResponse:
    """What urlopen returns, as far as _stream is concerned: a context manager
    that iterates lines of bytes."""

    def __init__(self, lines):
        self.lines = lines
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.closed = True
        return False

    def __iter__(self):
        return iter(self.lines)


def sse(*pieces):
    """A well-formed streaming reply carrying `pieces`, then [DONE]."""
    out = []
    for piece in pieces:
        body = {"choices": [{"delta": {"content": piece}}]}
        out.append(b"data: " + json.dumps(body).encode() + b"\n")
    out.append(b"data: [DONE]\n")
    return out


EMPTY = [b"data: [DONE]\n"]
# HTTP 200, a stream that opens and closes with nothing in it at all.
SILENT = []
# Deltas with no content in them - a role announcement and a finish reason,
# which is what several endpoints really do send around an empty answer.
NO_CONTENT = [
    b'data: {"choices":[{"delta":{"role":"assistant"}}]}\n',
    b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n',
    b"data: [DONE]\n",
]


def answers(*pieces):
    def request(body, stream=True):
        return FakeResponse(sse(*pieces))
    return request


def replies_with(lines):
    def request(body, stream=True):
        return FakeResponse(lines)
    return request


def refuses(error):
    def request(body, stream=True):
        raise error
    return request


def http_error(code, message=None):
    body = None
    if message is not None:
        body = io.BytesIO(json.dumps({"error": {"message": message}}).encode())
    return urllib.error.HTTPError("https://x/v1/chat/completions", code, "no", {}, body)


def chat_with(*requests):
    """A Chat over one provider per `requests` entry, named p0, p1, ..."""
    specs = [{"name": "p%d" % i, "base_url": "https://x/v1", "model": "m"}
             for i in range(len(requests))]
    chat = ext.Chat({"providers": specs, "history_turns": 8})
    for provider, request in zip(chat.providers, requests):
        provider.request = request
    return chat


def ask(chat, text="hello", cancelled=None, remember=True):
    return "".join(chat.ask(text, cancelled or threading.Event(), remember=remember))


# --- the tests --------------------------------------------------------------


def an_empty_stream_falls_over_to_the_next_provider():
    # The bug: HTTP 200 with no tokens counted as an answer, so the healthy
    # provider behind it was never reached and the user got silence.
    for name, lines in (("[DONE] straight away", EMPTY),
                        ("nothing at all", SILENT),
                        ("deltas with no content", NO_CONTENT)):
        chat = chat_with(replies_with(lines), answers("你好", "呀"))
        check_eq(ask(chat), "你好呀", "failover past a provider that sent " + name)
        check(chat.providers[0].blocked_until > 0, "the empty provider is cooled down")
        check(chat.providers[1].blocked_until == 0, "the one that answered is not")


def an_empty_answer_is_not_written_into_the_history():
    # The second half of the same bug, and the longer-lived one: an empty
    # assistant turn stayed in the history for the rest of the conversation and
    # was sent back to the model with every later question.
    chat = chat_with(replies_with(EMPTY), answers("hi"))
    ask(chat)
    check_eq(len(chat.history), 2, "one exchange is two entries")
    check_eq(chat.history[1]["content"], "hi", "and the assistant entry is the real answer")
    check(all(entry["content"] for entry in chat.history), "no empty entry anywhere")


def every_provider_empty_is_a_failure_that_names_them():
    chat = chat_with(replies_with(EMPTY), replies_with(SILENT))
    try:
        ask(chat)
        check(False, "an answer from nobody should raise")
    except IOError as e:
        check("p0" in str(e) and "p1" in str(e), "the failure names both providers: %s" % e)
        check("nothing" in str(e), "and says what was wrong with them: %s" % e)
    check_eq(chat.history, [], "and nothing is remembered")


def a_connection_failure_falls_over():
    chat = chat_with(refuses(urllib.error.URLError("connection refused")),
                     answers("still here"))
    check_eq(ask(chat), "still here", "a dead endpoint is stepped over")
    check(chat.providers[0].blocked_until > 0, "and cooled down")


def an_http_error_falls_over_and_keeps_the_status():
    chat = chat_with(refuses(http_error(429, "rate limit exceeded")), answers("ok"))
    check_eq(ask(chat), "ok", "an HTTP error is stepped over")
    try:
        chat_with(refuses(http_error(401, "bad key"))).ask("x", threading.Event()).__next__()
        check(False, "the only provider failing should raise")
    except IOError as e:
        check("HTTP 401" in str(e), "the status survives into the message: %s" % e)
        check("bad key" in str(e), "and so does the endpoint's own reason: %s" % e)


def a_malformed_stream_is_skipped_and_an_unreadable_one_falls_over():
    # One bad event in an otherwise good stream is skipped: an answer that is
    # arriving is worth more than strictness about one line of it.
    mixed = [b"data: {not json}\n"] + sse("real ", "answer")
    chat = chat_with(replies_with(mixed))
    check_eq(ask(chat), "real answer", "one bad event does not lose the answer")

    # A stream that is *entirely* unreadable produced no tokens, which is the
    # empty case again - and has to fail over rather than pass for an answer.
    rubbish = [b"data: {not json}\n", b"data: {\"choices\":[]}\n", b"data: [DONE]\n"]
    chat = chat_with(replies_with(rubbish), answers("fallback"))
    check_eq(ask(chat), "fallback", "a wholly unreadable stream falls over")

    # And lines that are not SSE at all are simply not events.
    chat = chat_with(replies_with([b": keep-alive\n", b"\n"] + sse("hi")))
    check_eq(ask(chat), "hi", "comments and blank lines are ignored")


def cancellation_is_not_the_providers_fault():
    # Cancelled before the first token looks exactly like an empty answer from
    # here. It is not: she was told to be quiet. Blaming the endpoint would cool
    # down a provider that did nothing wrong, and trying the next one would be
    # answering a question that was withdrawn.
    cancelled = threading.Event()
    cancelled.set()
    chat = chat_with(answers("never seen"), answers("nor this"))
    check_eq(ask(chat, cancelled=cancelled), "", "a cancelled ask yields nothing")
    check(chat.providers[0].blocked_until == 0, "and cools nobody down")
    check(chat.providers[1].blocked_until == 0, "and does not move on to the next")
    check_eq(chat.history, [], "and remembers nothing")


def a_provider_that_has_started_answering_is_not_abandoned():
    # The rule the whole design rests on: once a piece of an answer is on
    # screen, switching providers would write a second answer on top of it. So a
    # failure after the first token is a failure, not a reason to fail over.
    def dies_after_one(body, stream=True):
        def lines():
            yield b"data: " + json.dumps(
                {"choices": [{"delta": {"content": "half "}}]}).encode() + b"\n"
            raise urllib.error.URLError("dropped")
        return FakeResponse(lines())

    chat = chat_with(dies_after_one, answers("SHOULD NOT BE USED"))
    got = []
    try:
        for piece in chat.ask("x", threading.Event()):
            got.append(piece)
        check(False, "a stream that dies mid-answer should raise")
    except urllib.error.URLError:
        pass
    check_eq("".join(got), "half ", "what did arrive was delivered")
    check(chat.providers[1].blocked_until == 0, "and the next provider was never tried")


def an_answer_is_remembered_and_the_history_is_capped():
    chat = chat_with(answers("a"))
    chat.turns = 2
    for _ in range(5):
        chat.providers[0].request = answers("a")
        ask(chat, text="q")
    check_eq(len(chat.history), 4, "two turns is four entries")
    check_eq(chat.history[0]["role"], "user", "and the window starts on a question")

    # remember=False is what a glance uses: a remark about a screenshot is not
    # something she should still be holding three questions into a later chat.
    chat = chat_with(answers("nice desktop"))
    ask(chat, remember=False)
    check_eq(chat.history, [], "a glance leaves no trace")


def describe_separates_the_three_kinds():
    check_eq(ext.describe(http_error(429, "slow down")), "HTTP 429: slow down",
             "an HTTP error carries the endpoint's own words")
    # No body to read: describe() used to swallow this with a bare `except
    # Exception`. Narrowed, it must still come back with the status.
    check_eq(ext.describe(http_error(500)), "HTTP 500", "a bodyless HTTP error keeps its status")
    # A body that is JSON but not an object, and one that is not JSON at all.
    check_eq(ext.describe(urllib.error.HTTPError("u", 503, "no", {}, io.BytesIO(b"<html>"))),
             "HTTP 503", "an unparseable body does not lose the status")
    check_eq(ext.describe(urllib.error.HTTPError("u", 502, "no", {}, io.BytesIO(b'"text"'))),
             "HTTP 502", "a JSON body that is not an object does not lose the status")
    check("did not connect" in ext.describe(urllib.error.URLError("no route")),
          "a connection failure says so")
    check("unreadable reply" in ext.describe(ValueError("bad json")),
          "and anything else is named as what it is")


def a_provider_with_no_working_neighbour_still_reports_every_reason():
    chat = chat_with(refuses(urllib.error.URLError("refused")),
                     refuses(http_error(401, "bad key")),
                     replies_with(EMPTY))
    try:
        ask(chat)
        check(False, "three failures should raise")
    except IOError as e:
        for want in ("did not connect", "HTTP 401", "nothing"):
            check(want in str(e), "the message keeps '%s': %s" % (want, e))


def all_providers_cooling_down_are_tried_anyway():
    # A cooldown is a hint, not a ban. If every provider is inside one, the
    # hints have stopped being information and refusing would be worse.
    chat = chat_with(answers("late but here"))
    chat.providers[0].blocked_until = float("inf")
    check_eq(ask(chat), "late but here", "the last resort is tried rather than refused")


def the_prompt_command_is_split_by_shlex_not_by_spaces():
    # The same grammar the C++ end applies to `[ext] command` (app/argparse.cpp),
    # so one config file cannot mean two things depending on which end reads it.
    helper = ext.Helper.__new__(ext.Helper)
    helper.config = {"prompt_command": '/opt/my tools/ask --title "say something"'}
    check_eq(helper.prompt_argv({}),
             ["/opt/my", "tools/ask", "--title", "say something"],
             "quoting is honoured and bare spaces still split")
    helper.config = {"prompt_command": '"/opt/my tools/ask" -x'}
    check_eq(helper.prompt_argv({}), ["/opt/my tools/ask", "-x"],
             "a quoted path with a space in it is one argument")
    helper.config = {"prompt_command": 'ask --title "unclosed'}
    try:
        helper.prompt_argv({})
        check(False, "an unterminated quote should be refused")
    except IOError as e:
        check("prompt_command" in str(e), "and the message names the setting: %s" % e)


def the_helper_survives_work_that_throws():
    # The supervisory boundary. Below it are an HTTP client, a JSON decoder, a
    # subprocess and a socket; an exception nobody predicted used to unwind
    # straight out of run() and end the helper, which then stays ended because
    # nothing supervises it.
    helper = ext.Helper.__new__(ext.Helper)
    reached = []

    def boom():
        raise RuntimeError("something nobody predicted")

    helper.guarded(boom)
    helper.guarded(reached.append, "still running")
    check_eq(reached, ["still running"], "the loop carries on to the next piece of work")


TESTS = [
    an_empty_stream_falls_over_to_the_next_provider,
    an_empty_answer_is_not_written_into_the_history,
    every_provider_empty_is_a_failure_that_names_them,
    a_connection_failure_falls_over,
    an_http_error_falls_over_and_keeps_the_status,
    a_malformed_stream_is_skipped_and_an_unreadable_one_falls_over,
    cancellation_is_not_the_providers_fault,
    a_provider_that_has_started_answering_is_not_abandoned,
    an_answer_is_remembered_and_the_history_is_capped,
    describe_separates_the_three_kinds,
    a_provider_with_no_working_neighbour_still_reports_every_reason,
    all_providers_cooling_down_are_tried_anyway,
    the_prompt_command_is_split_by_shlex_not_by_spaces,
    the_helper_survives_work_that_throws,
]


def main():
    global current
    for test in TESTS:
        current = test.__name__
        before = failures
        test()
        print("%-58s %s" % (test.__name__, "ok" if failures == before else "FAILED"))
    if failures:
        print("\n%d check(s) failed" % failures)
        return 1
    print("\nall %d ext tests passed" % len(TESTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
