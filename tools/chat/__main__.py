"""Command-line entry point for the Asuna chat helper."""

import argparse
import signal
import sys

from .helper import Helper
from .log import log


def main():
    parser = argparse.ArgumentParser(
        description="asuna-ext - the extension helper: a chat window, and the occasional remark"
    )
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
