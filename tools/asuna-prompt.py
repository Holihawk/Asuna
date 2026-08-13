#!/usr/bin/env python3
"""Compatibility entry point for the Asuna GTK prompt."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Importing prompt performs the required pre-GTK preload and one-time re-exec.
# Re-export the old script's public names for compatibility after that re-exec.
from chat.prompt import CSS, EDGE, Prompt, main  # noqa: F401


if __name__ == "__main__":
    sys.exit(main())
