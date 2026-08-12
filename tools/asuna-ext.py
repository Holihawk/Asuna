#!/usr/bin/env python3
"""asuna-ext - the extension helper: a chat window, and the occasional remark
about what is on screen.

Runs *out of process* and drives her through the control socket that already
exists, which is the whole architecture in one sentence. The reasons, from the
README:

  - Her main loop is her render loop. A TLS handshake or a slow first token
    inside it is a visible freeze, and a pet that freezes is a pet that looks
    broken.
  - The API key lives here and only here. In the daemon it would share an
    address space with a GL context, a Live2D runtime and a socket anything on
    the machine can connect to; here it is in a script you can read in one
    sitting - which is the point of it being this file and not a library.
  - It can be killed, edited and restarted without her reloading 113 MB of
    model, so prompts are cheap to iterate on.

If this program dies, she carries on exactly as she does with it switched off.
Nothing here is load-bearing for the pet.

Everything it needs is in her config file, under [ext] - it asks *her* for
those settings rather than parsing the file itself, so there is one parser, one
set of complaints, and her persona sits in the same file as her idle timings.

Started by `asuna ext start`. `asuna ext test` runs it with --test, which
probes every configured provider and exits without touching her.

One rule about the prompt, because it decides how the whole conversation feels:
it only ever opens because you just asked for it. `asuna chat`, the Chat entry
in her menu, or - once she has answered something - a poke. It never reappears
by itself after an answer, because the prompt is a layer surface and a layer
surface takes the keyboard the moment it maps, so one that appears uninvited
takes the sentence you were typing somewhere else with it.
"""

import argparse
import base64
import io
import json
import os
import queue
import random
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

# How often the token stream is pushed into her bubble. Every token would be a
# socket round trip per token; a tenth of a second still reads as arriving.
FLUSH_INTERVAL = 0.10

# How long a provider that just failed is skipped for. Long enough that a dead
# endpoint is not retried on every message, short enough that one coming back
# is noticed within a conversation.
COOLDOWN = 120.0

# How long the conversation stays open after an answer, waiting to be picked up
# again. Poke her (or run `asuna chat`) inside this window and the next question
# keeps the history; let it lapse and the conversation is over and forgotten.
FOLLOW_UP_WINDOW = 90.0

# How long an unanswered prompt stays on screen. It is a layer surface on the
# overlay, so a prompt left open sits above everything else on the desktop
# until it is answered - which is fine for a minute and silly after five.
PROMPT_TIMEOUT = 300.0

# Connect/first-byte budget per provider. Short, because it is also how long a
# dead provider delays the live one behind it in the list.
CONNECT_TIMEOUT = 8.0
READ_TIMEOUT = 60.0


def log(*parts):
    """The log is a record of what it did, never of what was said.

    What was typed is logged truncated (it is the one thing worth having when a
    reply goes wrong); what came back is not logged at all. The reply is
    streamed into her bubble and the bubble is the only place it ever lives -
    there is no transcript on disk, here or anywhere else.
    """
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


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


# --- the model ---------------------------------------------------------------


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
    """An HTTP failure in a form worth putting in front of a person."""
    if isinstance(error, urllib.error.HTTPError):
        detail = ""
        try:
            body = json.loads(error.read().decode("utf-8", "replace"))
            detail = body.get("error", {}).get("message", "") if isinstance(body, dict) else ""
        except Exception:
            pass
        return "HTTP %d%s" % (error.code, ": " + detail if detail else "")
    if isinstance(error, urllib.error.URLError):
        return str(error.reason)
    return str(error)


class Chat:
    """The providers, in priority order, and the conversation in front of them.

    Failover happens *before the first token*, and never after: once a piece of
    an answer is on screen, moving to another provider would write a second
    answer on top of the first. So a provider is committed to by producing
    something, and a failure after that is a failure.

    History is a plain list, capped at `history_turns` exchanges, and it is
    dropped the moment the chat window closes: she remembers inside one
    conversation and nothing between them. That is a deliberate choice and not a
    limitation to be worked around here - a pet that quietly accumulates a
    transcript of everything you ever typed is a different program.
    """

    def __init__(self, config):
        self.configure(config)
        self.history = []

    def configure(self, config):
        self.providers = [Provider(p) for p in config.get("providers", [])]
        self.persona = config.get("persona", "")
        self.turns = int(config.get("history_turns", 8))
        self.temperature = float(config.get("temperature", 0.8))
        self.max_tokens = int(config.get("max_tokens", 220))

    def usable(self):
        return bool(self.providers)

    def forget(self):
        self.history = []

    def _body(self, messages, stream):
        return {
            "model": None,   # filled in per provider
            "messages": messages,
            "stream": stream,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
        }

    def _stream(self, provider, messages, cancelled):
        body = self._body(messages, True)
        body["model"] = provider.model
        with provider.request(body) as response:
            for raw in response:
                if cancelled.is_set():
                    return
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                payload = line[5:].strip()
                if payload == "[DONE]":
                    return
                try:
                    delta = json.loads(payload)["choices"][0]["delta"]
                except (ValueError, KeyError, IndexError):
                    continue
                piece = delta.get("content")
                if piece:
                    yield piece

    def ask(self, text, cancelled, image=None, remember=True):
        """Streams an answer, yielding it in pieces. Raises if none answered."""
        content = text
        if image is not None:
            content = [
                {"type": "text", "text": text},
                {"type": "image_url",
                 "image_url": {"url": "data:image/jpeg;base64," + base64.b64encode(image).decode()}},
            ]
        messages = ([{"role": "system", "content": self.persona}] if self.persona else [])
        messages += self.history + [{"role": "user", "content": content}]

        ready = [p for p in self.providers if p.ready()]
        # All of them cooling down means the cooldowns have stopped being
        # information. Try everything rather than refuse.
        for provider in ready or self.providers:
            answer = []
            try:
                stream = self._stream(provider, messages, cancelled)
                first = next(stream, None)
            except (urllib.error.URLError, OSError, ValueError) as e:
                provider.blocked_until = time.time() + COOLDOWN
                log("%s did not answer (%s), trying the next" % (provider.name, describe(e)))
                continue
            provider.blocked_until = 0.0
            if len(self.providers) > 1:
                log("answering through", provider.name)
            if first is not None:
                answer.append(first)
                yield first
            for piece in stream:
                answer.append(piece)
                yield piece
            if remember:
                # The picture itself is never kept: it goes to the endpoint once
                # and is not carried into the next turn, where it would be sent
                # again.
                self.history.append({"role": "user", "content": text})
                self.history.append({"role": "assistant", "content": "".join(answer)})
                del self.history[: max(0, len(self.history) - 2 * self.turns)]
            return
        raise IOError("no provider answered")

    def probe(self, provider):
        """One tiny non-streaming request. Returns (ok, detail, seconds)."""
        if not provider.base_url:
            return False, "no base_url", 0.0
        body = self._body([{"role": "user", "content": "ping"}], False)
        body["model"] = provider.model
        body["max_tokens"] = 1
        started = time.time()
        try:
            with provider.request(body, stream=False) as response:
                json.load(response)
            return True, "", time.time() - started
        except (urllib.error.URLError, OSError, ValueError) as e:
            return False, describe(e), time.time() - started


# --- the screen --------------------------------------------------------------


def focused_app_id():
    """The app-id of the focused window, or "" if it cannot be determined.

    niri only. On anything else this returns "" and the deny list therefore
    matches nothing - which is why the deny list is not the only gate, and why
    vision defaults to off.
    """
    try:
        out = subprocess.run(
            ["niri", "msg", "--json", "focused-window"],
            capture_output=True, timeout=2, text=True,
        )
        if out.returncode != 0:
            return ""
        window = json.loads(out.stdout)
        return (window.get("app_id") or "") if isinstance(window, dict) else ""
    except (OSError, ValueError, subprocess.SubprocessError):
        return ""


def capture(output, mask, longest=1280):
    """One screenshot of `output`, with `mask` painted out, as JPEG bytes.

    The mask is her: her outline and her speech bubble, in device pixels,
    computed by the daemon (see Shell::handleExt). Painting it out keeps her own
    last sentence from being handed back to the model as "what is on screen",
    which is how a character ends up answering itself.
    """
    shot = subprocess.run(
        ["grim", "-o", output, "-t", "png", "-"], capture_output=True, timeout=10
    )
    if shot.returncode != 0:
        raise IOError("grim: " + shot.stderr.decode("utf-8", "replace").strip())

    box = (mask["x"], mask["y"], mask["x"] + mask["w"], mask["y"] + mask["h"])
    try:
        from PIL import Image, ImageDraw

        image = Image.open(io.BytesIO(shot.stdout)).convert("RGB")
        ImageDraw.Draw(image).rectangle(box, fill=(20, 20, 24))
        image.thumbnail((longest, longest))
        out = io.BytesIO()
        image.save(out, "JPEG", quality=72)
        return out.getvalue()
    except ImportError:
        pass

    if not shutil.which("magick"):
        raise IOError("neither python3-pillow nor ImageMagick is installed,"
                      " so the picture cannot be masked - refusing to send it")
    converted = subprocess.run(
        ["magick", "png:-", "-fill", "#141418", "-draw", "rectangle %d,%d %d,%d" % box,
         "-resize", "%dx%d>" % (longest, longest), "-quality", "72", "jpg:-"],
        input=shot.stdout, capture_output=True, timeout=20,
    )
    if converted.returncode != 0:
        raise IOError("magick: " + converted.stderr.decode("utf-8", "replace").strip())
    return converted.stdout


# --- the helper itself -------------------------------------------------------


class Helper:
    def __init__(self):
        self.control = Control(socket_path())
        self.events = queue.Queue()
        self.cancelled = threading.Event()
        self.running = True
        self.config = {}
        self.chat = None
        self.next_glance = None
        self.prompt = None   # the prompt process, while one is open

    # -- typing at her ------------------------------------------------------

    def prompt_argv(self, where, placeholder="说点什么…"):
        """What to run to ask for a line, and where to put it."""
        configured = (self.config.get("prompt_command") or "").strip()
        if configured:
            return configured.split()
        bundled = os.path.join(os.path.dirname(os.path.abspath(__file__)), "asuna-prompt.py")
        if not os.path.exists(bundled):
            raise IOError("asuna-prompt.py is missing - set `prompt_command` under [ext]")
        argv = [sys.executable, bundled, "--placeholder", placeholder]
        # Beside her, rather than in the middle of the screen. She may have been
        # dragged since the last question, so this is asked each time.
        if where.get("anchor_x"):
            argv += ["--x", str(int(where["anchor_x"])),
                     "--bottom", str(int(where.get("anchor_bottom", 0))),
                     # above her head, or beside her when she is scaled up far
                     # enough that there is no useful screen left above it
                     "--place", str(where.get("anchor_place", "above"))]
        # The screen she is standing on, which is the prompt's whole frame of
        # reference: its surface is that screen, the point above is measured off
        # the bottom of it, and the clamp that keeps a drag on it comes from the
        # far edges.
        if where.get("screen_width"):
            argv += ["--screen-width", str(int(where["screen_width"])),
                     "--screen-height", str(int(where.get("screen_height", 0)))]
        return argv

    def ask_the_user(self, timeout=PROMPT_TIMEOUT, placeholder="说点什么…"):
        """A one-line text entry. None if it was dismissed or timed out.

        Kept on `self` rather than local, because a prompt is the one thing here
        that blocks indefinitely: without a handle to it, `asuna ext stop` on a
        helper waiting at an open prompt needs a SIGKILL to land.

        This is only ever reached because the user just asked for it - see
        wait_to_be_asked. A prompt is a layer surface that takes the keyboard the
        moment it appears, which is right for one you asked for and unforgivable
        for one that appears by itself into the middle of a sentence you were
        typing somewhere else.
        """
        try:
            where = self.control.call("ext", status=True)
        except (IOError, OSError):
            where = {}
        self.prompt = subprocess.Popen(
            self.prompt_argv(where, placeholder), stdout=subprocess.PIPE, text=True
        )
        try:
            out, _ = self.prompt.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.prompt.terminate()
            self.prompt.communicate()
            log("the prompt was left open, so it has been put away")
            return None
        finally:
            code, self.prompt = self.prompt.returncode, None
        return (out.strip() or None) if code == 0 else None

    # -- her end ------------------------------------------------------------

    def load_config(self):
        self.config = self.control.call("ext", config=True, secrets=True)
        if self.chat is None:
            self.chat = Chat(self.config)
        else:
            self.chat.configure(self.config)
        if not self.chat.usable():
            log("no provider is configured - she can be talked to, but not answered")
        self.schedule_glance()

    def schedule_glance(self):
        """When to next look at the screen, or never if it is not allowed."""
        if not self.config.get("vision_enabled"):
            self.next_glance = None
            return
        lo = float(self.config.get("vision_min", 900))
        hi = float(self.config.get("vision_max", 2700))
        self.next_glance = time.time() + random.uniform(lo, max(lo, hi))
        log("next glance in %.0f min" % ((self.next_glance - time.time()) / 60))

    def stream_into_bubble(self, pieces):
        """Writes an answer into her bubble as it arrives.

        Buffered on a short timer rather than sent token by token: one socket
        round trip per token would be a few hundred for a paragraph, and at that
        rate she stops being a pet with something to say and becomes a load.
        """
        self.control.call("think", on=True)
        buffer, started, last = "", False, 0.0
        try:
            for piece in pieces:
                if self.cancelled.is_set():
                    break
                buffer += piece
                now = time.time()
                if now - last < FLUSH_INTERVAL:
                    continue
                if buffer:
                    self.control.call("say", text=buffer,
                                      **({"append": True} if started else {"hold": True}))
                    started, buffer, last = True, "", now
            if buffer and not self.cancelled.is_set():
                self.control.call("say", text=buffer,
                                  **({"append": True} if started else {"hold": True}))
                started = True
        finally:
            if self.cancelled.is_set():
                self.control.call("say", clear=True)
            elif started:
                self.control.call("say", release=True)
            else:
                # Nothing ever arrived: take the wait down rather than leave her
                # looking like she is still listening.
                self.control.call("think", on=False)
        return started

    def say_plainly(self, text):
        try:
            self.control.call("say", clear=True)
            self.control.call("say", text=text)
        except (IOError, OSError):
            pass

    # -- the two things it does ---------------------------------------------

    def wait_to_be_asked(self):
        """After an answer: wait to be invited back, or end the conversation.

        Returns the next question ("" for "open the prompt"), or None if the
        window lapsed. Poking her counts as the invitation, and so does another
        `asuna chat` - both are the user reaching for her deliberately, which is
        the only thing that may take their keyboard away.

        This replaces a follow-up prompt that opened by itself a second after
        the answer finished. There is no way for a layer surface to appear
        without taking the keyboard - `on_demand` is granted focus on map, and
        setting it after the fact is ignored (tested on niri) - so a prompt that
        appears uninvited is a prompt that eats the next thing you type into
        whatever you had been working in.
        """
        # Anything that arrived while the answer was being written is not an
        # invitation - a poke during an answer is a poke at the answer - so the
        # backlog is cleared before the window opens.
        while True:
            try:
                stale = self.events.get_nowait()
            except queue.Empty:
                break
            if stale.get("event") == "bye":
                self.events.put(stale)
                return None
            if stale.get("event") == "config":
                self.load_config()

        deadline = time.time() + FOLLOW_UP_WINDOW
        while self.running and time.time() < deadline:
            try:
                event = self.events.get(timeout=min(1.0, max(0.05, deadline - time.time())))
            except queue.Empty:
                continue
            name = event.get("event")
            if name == "bye":
                self.events.put(event)   # the run loop has to see this one too
                return None
            if name == "touch":
                return ""
            if name == "chat":
                return event.get("text", "")
            if name == "config":
                self.load_config()
        return None

    def converse(self, first=""):
        """One conversation: prompt, answer, and again for as long as she is
        asked back.

        The history lives on the Chat for exactly this long. Leaving here drops
        it, which is the whole of "no memory between conversations".
        """
        if not self.chat or not self.chat.usable():
            self.say_plainly("我这边还没接上……配置里还没写 provider 呢。")
            log("no provider configured, so there is nothing to answer with")
            return
        text, answered = first, False
        while self.running:
            if not text and answered:
                text = self.wait_to_be_asked()
                if text is None:
                    break
            if not text:
                try:
                    text = self.ask_the_user(
                        placeholder="接着说…" if answered else "说点什么…")
                except (IOError, OSError) as e:
                    log("prompt:", e)
                    return
            if not text:
                break
            self.cancelled.clear()
            log("asked:", text[:80])
            try:
                self.stream_into_bubble(self.chat.ask(text, self.cancelled))
                answered = True
            except (urllib.error.URLError, IOError, OSError) as e:
                # Not necessarily the endpoint: `say` fails too if she has been
                # hidden or has left since the question was asked, and calling
                # that a connection problem would send someone looking in the
                # wrong place. The log gets what actually happened.
                log("could not answer:", e)
                self.say_plainly("唔……我这边连不上，等一下再试试？")
                break
            text = ""
        self.chat.forget()
        log("conversation over, history dropped")

    def glance(self):
        """One remark about what is on screen, with consent either side of it.

        Three gates, and all three have to say yes: this process checks the
        config it was given, *she* checks her own copy and refuses to hand over
        the geometry otherwise, and in between she visibly looks up and can be
        called off by poking her. The middle one is the one that matters - it is
        the only check that does not live in the process holding the API key.
        """
        if not self.chat or not self.chat.usable():
            return
        if not self.config.get("vision_enabled"):
            return
        app = focused_app_id()
        deny = self.config.get("vision_deny") or []
        # Substring, case-insensitive: app-ids are reverse-DNS as often as not,
        # and "firefox" should keep her out of org.mozilla.firefox without
        # anyone having to look the full name up first.
        if app and any(d.lower() in app.lower() for d in deny):
            log("not looking: %s is on the deny list" % app)
            return

        try:
            wait = self.control.call("ext", glance=True).get("wait_ms", 3000)
        except IOError as e:
            log("she said no:", e)
            return
        # Her notice is a real pause, so this really waits it out; a poke during
        # it is what makes the next call fail.
        deadline = time.time() + wait / 1000.0 + 0.15
        while time.time() < deadline and self.running:
            time.sleep(0.05)
        if self.cancelled.is_set():
            log("called off during the notice")
            return

        try:
            grant = self.control.call("ext", capture=True)
            image = capture(grant["output"], grant["mask"])
        except (IOError, OSError, subprocess.SubprocessError) as e:
            log("no picture:", e)
            return
        log("looked at %s (%d KiB)" % (grant["output"], len(image) // 1024))

        self.cancelled.clear()
        try:
            # remember=False: a remark she made about a screenshot is not
            # something she should still be holding in her head three questions
            # into a later conversation.
            self.stream_into_bubble(
                self.chat.ask(self.config.get("glance_prompt", ""), self.cancelled,
                              image=image, remember=False)
            )
        except (urllib.error.URLError, IOError, OSError) as e:
            log("could not answer:", e)
            try:
                self.control.call("say", clear=True)
            except (IOError, OSError):
                pass

    # -- the loop -----------------------------------------------------------

    def subscriber(self):
        try:
            for event in self.control.events():
                self.events.put(event)
        except OSError as e:
            if self.running:
                log("subscription dropped:", e)
        finally:
            self.events.put({"event": "bye"})

    def run(self):
        self.load_config()
        threading.Thread(target=self.subscriber, daemon=True).start()
        log("connected to", self.control.path)

        while self.running:
            # Two seconds rather than the minutes this loop actually has to
            # wait, because it is also how a signal gets acted on: the handler
            # sets a flag from inside whatever the main thread was doing, and
            # this is where that flag is next read. `asuna ext stop` allows five
            # seconds before it escalates.
            timeout = 2.0
            if self.next_glance:
                timeout = max(0.2, min(timeout, self.next_glance - time.time()))
            try:
                event = self.events.get(timeout=timeout)
            except queue.Empty:
                event = None

            if event:
                name = event.get("event")
                if name == "bye":
                    log("she has gone")
                    break
                elif name == "chat":
                    self.converse(event.get("text", ""))
                elif name == "cancel":
                    self.cancelled.set()
                elif name == "config":
                    log("config reloaded")
                    self.load_config()
                elif name == "sleep" and event.get("asleep"):
                    # Asleep is not a state to wake her out of with an opinion
                    # about your screen. The next glance waits for her.
                    self.schedule_glance()
                continue

            if self.next_glance and time.time() >= self.next_glance:
                self.glance()
                self.schedule_glance()

    def stop(self, *_):
        """SIGTERM, from `asuna ext stop`.

        Runs in the main thread, wherever it happens to be. The prompt is the
        one place that would otherwise not come back: it is waited on with
        waitpid, which CPython retries across a signal, so a helper sitting at
        an open prompt would ignore SIGTERM until somebody typed something.
        """
        self.running = False
        self.cancelled.set()
        if self.prompt:
            self.prompt.terminate()
        self.events.put({"event": "bye"})

    # -- asuna ext test -----------------------------------------------------

    def test(self):
        """Probe every configured provider and report. Never touches her."""
        self.config = self.control.call("ext", config=True, secrets=True)
        chat = Chat(self.config)
        if not chat.usable():
            print("asuna: no provider is configured under [ext.provider.<name>]")
            return 1
        idle = self.config.get("idle_providers") or []
        print("%-12s %-22s %-18s %s" % ("provider", "model", "key", "result"))
        worked = 0
        for provider in chat.providers:
            ok, detail, seconds = chat.probe(provider)
            worked += ok
            print("%-12s %-22s %-18s %s" % (
                provider.name, provider.model[:22], provider.key_from[:18],
                "ok, %d ms" % (seconds * 1000) if ok else "FAILED  " + detail))
        for name in idle:
            print("%-12s %-22s %-18s %s" % (name, "", "", "not in `providers`, so unused"))
        if not worked:
            print("\nasuna: none of them answered. A key is read from the environment of"
                  "\n       the helper, so it has to be set where `asuna ext start` runs.")
        return 0 if worked else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--test", action="store_true",
                        help="probe every provider and exit")
    args = parser.parse_args()

    helper = Helper()
    try:
        if args.test:
            return helper.test()
        signal.signal(signal.SIGTERM, helper.stop)
        signal.signal(signal.SIGINT, helper.stop)
        helper.run()
    except (FileNotFoundError, ConnectionRefusedError):
        log("she is not running (no control socket at %s)" % helper.control.path)
        return 3
    except IOError as e:
        log("could not start:", e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
