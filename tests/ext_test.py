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
import queue
import sys
import threading
import traceback
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


def an_invalid_event_shape_falls_over_like_any_other_unreadable_one():
    # Valid JSON, invalid SSE. Each of these used to escape the failover as an
    # AttributeError or a TypeError rather than being counted as an unreadable
    # event: the helper survived at the supervisory boundary, the next provider
    # was never tried, and the answer was lost.
    shapes = [
        b'data: {"choices":[{"delta":null}]}\n',          # AttributeError
        b'data: {"choices":[null]}\n',
        b'data: {"choices":{"delta":{}}}\n',              # choices not a list
        b'data: {"choices":[{"delta":[1,2]}]}\n',         # delta not an object
        b'data: ["not", "an", "object"]\n',               # root not an object
        b'data: "a string"\n',
        b'data: 42\n',
        b'data: {"choices":[{"delta":{"content":7}}]}\n',  # content not text
    ]
    for shape in shapes:
        chat = chat_with(replies_with([shape, b"data: [DONE]\n"]), answers("fallback"))
        check_eq(ask(chat), "fallback", "failover past %r" % shape.strip())
        check(chat.providers[0].blocked_until > 0, "and the odd one is cooled down")

    # And one bad shape among good events still delivers the answer.
    chat = chat_with(replies_with([shapes[0]] + sse("real ", "answer")))
    check_eq(ask(chat), "real answer", "one invalid event does not lose the answer")

    # A delta with no content at all is normal, not malformed - it is how a
    # stream announces a role or a finish reason - so it must not fail over on
    # its own account when content follows.
    ok = [b'data: {"choices":[{"delta":{"role":"assistant"}}]}\n'] + sse("hi")
    check_eq(ask(chat_with(replies_with(ok))), "hi", "a metadata delta is not an error")


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


def the_bundled_prompt_remains_beside_the_compatibility_wrapper():
    helper = ext.Helper.__new__(ext.Helper)
    helper.config = {}
    argv = helper.prompt_argv({})
    check_eq(argv[0], sys.executable, "the bundled prompt uses this Python")
    check_eq(argv[1], os.path.join(os.path.dirname(HELPER), "asuna-prompt.py"),
             "the package resolves the wrapper in its parent directory")
    check(os.path.exists(argv[1]), "the resolved prompt wrapper exists")


def prompt_position_is_process_local_and_restored_only_for_the_bundled_prompt():
    helper = ext.Helper.__new__(ext.Helper)
    helper.config = {"prompt_command": ""}
    helper.prompt_position = None
    helper.remember_prompt_position(
        'not json\n{"event":"position","left":120,"bottom":80}\n'
        '{"event":"position","left":240,"bottom":160}\n')
    check_eq(helper.prompt_position, (240, 160), "the latest complete report wins")
    argv = helper.prompt_argv({}, position_fd=9)
    check_eq(argv[-6:], ["--position-left", "240", "--position-bottom", "160",
                         "--position-fd", "9"],
             "the bundled prompt restores and reports its position")

    helper.config = {"prompt_command": "custom-prompt"}
    check_eq(helper.prompt_argv({}, position_fd=9), ["custom-prompt"],
             "a custom prompt keeps its existing argv contract")

    helper.remember_prompt_position('{"event":"reset"}\n')
    check_eq(helper.prompt_position, None, "a handle double-click releases the pin")
    argv = helper.prompt_argv({}, position_fd=9)
    check("--position-left" not in argv and "--position-bottom" not in argv,
          "an unpinned prompt goes back to daemon-chosen placement")

    helper.remember_prompt_position(
        '{"event":"reset"}\n{"event":"position","left":300,"bottom":200}\n')
    check_eq(helper.prompt_position, (300, 200),
             "dragging after a reset pins the new position again")


def a_cancel_lands_while_the_run_loop_is_busy_answering():
    """`asuna ext cancel` has to work at the one moment it is wanted.

    The run loop dispatches a `chat` event by calling converse() synchronously,
    and converse() then blocks in the prompt and in Chat.ask(). While it is
    there the loop is not reading its queue, so a `cancel` that only got acted
    on by the loop would sit in that queue until the conversation it was meant
    to interrupt had already ended. The flag is therefore set in the subscriber
    thread, the moment the event arrives.

    This drives the real subscriber, and deliberately never drains the queue -
    which is exactly the state converse() leaves the loop in.
    """
    helper = ext.Helper.__new__(ext.Helper)
    helper.events = queue.Queue()
    helper.cancelled = threading.Event()
    helper.running = True
    busy = threading.Event()

    class Feed:
        path = "(test)"

        def events(self):
            yield {"event": "chat", "text": "hi"}
            # The loop is now "inside converse()": nothing will read the queue
            # again until this generator is long finished.
            busy.set()
            yield {"event": "cancel"}

    helper.control = Feed()
    worker = threading.Thread(target=helper.subscriber, daemon=True)
    worker.start()
    worker.join(5.0)

    check(busy.is_set(), "the feed delivered both events")
    check(helper.cancelled.is_set(),
          "the cancel took effect without the run loop ever dequeuing it")
    # Still queued, so a loop that is idle handles it the same way it always did.
    check(helper.events.qsize() >= 2, "and the event is still there for the loop")


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


def provider_only_imports_do_not_load_gtk():
    # Provider and conversation tests must remain usable on a server with no
    # GTK stack. The compatibility wrapper imports the whole non-GTK helper
    # surface, so its successful import above is already the strongest version
    # of that check; pin down the dependency boundary as well.
    check("chat.prompt" not in sys.modules, "the prompt module was not imported")
    check("gi" not in sys.modules, "PyGObject was not imported")


TESTS = [
    an_empty_stream_falls_over_to_the_next_provider,
    an_empty_answer_is_not_written_into_the_history,
    every_provider_empty_is_a_failure_that_names_them,
    a_connection_failure_falls_over,
    an_http_error_falls_over_and_keeps_the_status,
    a_malformed_stream_is_skipped_and_an_unreadable_one_falls_over,
    an_invalid_event_shape_falls_over_like_any_other_unreadable_one,
    cancellation_is_not_the_providers_fault,
    a_provider_that_has_started_answering_is_not_abandoned,
    an_answer_is_remembered_and_the_history_is_capped,
    describe_separates_the_three_kinds,
    a_provider_with_no_working_neighbour_still_reports_every_reason,
    all_providers_cooling_down_are_tried_anyway,
    the_prompt_command_is_split_by_shlex_not_by_spaces,
    the_bundled_prompt_remains_beside_the_compatibility_wrapper,
    prompt_position_is_process_local_and_restored_only_for_the_bundled_prompt,
    a_cancel_lands_while_the_run_loop_is_busy_answering,
    the_helper_survives_work_that_throws,
    provider_only_imports_do_not_load_gtk,
]


def main():
    global current, failures
    for test in TESTS:
        current = test.__name__
        before = failures
        try:
            test()
        except Exception:   # supervisory boundary; see below
            # A test that throws is a failing test, not a reason to stop
            # running the others. It matters here in particular: the shapes
            # this suite feeds the helper are exactly the ones that used to
            # escape as an AttributeError, so the natural way for that
            # regression to come back is an exception rather than a wrong
            # value - and one traceback should not hide the nine tests after it.
            print("  FAIL %s raised:\n%s" % (current, traceback.format_exc().rstrip()))
            failures += 1
        print("%-58s %s" % (test.__name__, "ok" if failures == before else "FAILED"))
    if failures:
        print("\n%d check(s) failed" % failures)
        return 1
    print("\nall %d ext tests passed" % len(TESTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
