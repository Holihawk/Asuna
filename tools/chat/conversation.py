"""Streaming conversation history, cooldown, and provider failover."""

import base64
import http.client
import json
import time
import urllib.error

from .log import log
from .provider import Provider, describe

# How long a provider that just failed is skipped for. Long enough that a dead
# endpoint is not retried on every message, short enough that one coming back
# is noticed within a conversation.
COOLDOWN = 120.0


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
        unreadable = 0
        with provider.request(body) as response:
            for raw in response:
                if cancelled.is_set():
                    return
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                payload = line[5:].strip()
                if payload == "[DONE]":
                    break
                # Every container checked before it is indexed or asked for a
                # key. Catching ValueError/KeyError/IndexError around the whole
                # chain was not enough: `{"choices":[{"delta":null}]}` is valid
                # JSON, and `None.get` is an AttributeError, which escaped past
                # the failover in ask() entirely - the helper survived it at the
                # supervisory boundary, but the next provider was never tried
                # and the answer was simply lost. One malformed event is not
                # worth abandoning an answer that is otherwise arriving, so
                # these are skipped and counted; a stream that is *all* of them
                # yields nothing, which is the empty case, which fails over.
                try:
                    event = json.loads(payload)
                except ValueError:
                    unreadable += 1
                    continue
                choices = event.get("choices") if isinstance(event, dict) else None
                if not isinstance(choices, list) or not choices:
                    unreadable += 1
                    continue
                delta = choices[0].get("delta") if isinstance(choices[0], dict) else None
                if not isinstance(delta, dict):
                    unreadable += 1
                    continue
                piece = delta.get("content")
                if piece is None:
                    continue   # a metadata-only delta: role, finish_reason
                if not isinstance(piece, str):
                    unreadable += 1
                    continue
                if piece:
                    yield piece
        if unreadable:
            log("%s: skipped %d unreadable event(s) in the stream"
                % (provider.name, unreadable))

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
        refusals = []
        for provider in ready or self.providers:
            answer = []
            try:
                stream = self._stream(provider, messages, cancelled)
                first = next(stream, None)
            except (urllib.error.URLError, OSError, ValueError,
                    http.client.HTTPException) as e:
                # describe() separates the three that arrive here: an HTTP
                # status with the endpoint's own message where there is one, a
                # connection failure with the reason, and anything else as
                # itself.
                provider.blocked_until = time.time() + COOLDOWN
                refusals.append("%s: %s" % (provider.name, describe(e)))
                log("%s did not answer (%s), trying the next" % (provider.name, describe(e)))
                continue

            if first is None:
                # HTTP 200 and not one token in it - an immediate [DONE], an
                # empty choices list, a stream of deltas that were all metadata.
                # This used to count as a successful answer, so the fallback
                # provider was never tried and an empty assistant turn went into
                # the history, where it stayed for the rest of the conversation.
                #
                # Cancellation looks identical from here and is not the
                # provider's fault, so it is checked first and simply stops:
                # she was told to be quiet, and blaming the endpoint for that
                # would cool down a provider that did nothing wrong.
                if cancelled.is_set():
                    return
                provider.blocked_until = time.time() + COOLDOWN
                refusals.append("%s: answered with nothing" % provider.name)
                log("%s answered with nothing, trying the next" % provider.name)
                continue

            provider.blocked_until = 0.0
            if len(self.providers) > 1:
                log("answering through", provider.name)
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
        # Which provider failed and how, rather than one word for every cause.
        # This is what ends up in ext.log after "could not answer".
        if refusals:
            raise IOError("no provider answered - " + "; ".join(refusals))
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
        except (urllib.error.URLError, OSError, ValueError,
                http.client.HTTPException) as e:
            return False, describe(e), time.time() - started
