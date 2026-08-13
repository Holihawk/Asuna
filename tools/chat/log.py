"""Small shared runtime utilities for the chat helper."""

import time


def log(*parts):
    """The log is a record of what it did, never of what was said.

    What was typed is logged truncated (it is the one thing worth having when a
    reply goes wrong); what came back is not logged at all. The reply is
    streamed into her bubble and the bubble is the only place it ever lives -
    there is no transcript on disk, here or anywhere else.
    """
    print(time.strftime("%H:%M:%S"), *parts, flush=True)
