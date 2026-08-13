#!/usr/bin/env python3
"""Compatibility entry point for the Asuna chat helper."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# These exports preserve the import surface used by the offline regression
# suite and by local integrations written against the former single script.
from chat.__main__ import main  # noqa: E402
from chat.control import Control, socket_path  # noqa: E402, F401
from chat.conversation import COOLDOWN, Chat  # noqa: E402, F401
from chat.helper import (  # noqa: E402, F401
    FLUSH_INTERVAL,
    FOLLOW_UP_WINDOW,
    PROMPT_TIMEOUT,
    Helper,
)
from chat.log import log  # noqa: E402, F401
from chat.provider import (  # noqa: E402, F401
    CONNECT_TIMEOUT,
    READ_TIMEOUT,
    Provider,
    describe,
)
from chat.vision import capture, focused_app_id  # noqa: E402, F401


if __name__ == "__main__":
    sys.exit(main())
