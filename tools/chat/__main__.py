"""Command-line entry point for the Asuna chat helper."""

import argparse
import signal
import sys

from . import helper
from .log import log


def main():
    parser = argparse.ArgumentParser(
        description=helper.DESCRIPTION
    )
    parser.add_argument("--test", action="store_true",
                        help="probe every provider and exit")
    args = parser.parse_args()

    instance = helper.Helper()
    try:
        if args.test:
            return instance.test()
        signal.signal(signal.SIGTERM, instance.stop)
        signal.signal(signal.SIGINT, instance.stop)
        instance.run()
    except (FileNotFoundError, ConnectionRefusedError):
        log("she is not running (no control socket at %s)" % instance.control.path)
        return 3
    except OSError as e:
        log("could not start:", e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
