#!/bin/sh
# Command-level tests for the argument handling in src/app/cli.cpp.
#
# argparse_test.cpp covers the parsers themselves; this covers the wiring, which
# is where the two Phase 1 review findings actually lived. Neither was a bug in
# argparse: `--x` was handed the wrong bound by parseOptions, and `say --for`
# was handed a floor it should not have had. A leaf test cannot see either.
#
# Everything here is side-effect free and does not care whether she is running:
#
#   * every rejection is decided before the control socket is touched, so the
#     exit status and the message are the same either way;
#   * the acceptance cases end the argument list with --help, which parseOptions
#     handles by printing usage and returning "handled" - so the flag in front
#     of it is fully parsed and validated, and nothing is ever started. That is
#     the only way to assert "this value is accepted" without either forking a
#     daemon or requiring one.
#
# What is deliberately not here: that `move -500` and `scale 3.0` reach the
# daemon and get clamped by it. Both need a live daemon, and both would move or
# resize somebody's pet as a side effect of running the test suite. The CLI half
# - that those values parse rather than being refused - is asserted in
# argparse_test.cpp through realAny. The clamping half is Phase 8.

ASUNA="${1:?usage: cli_test.sh <path to asuna>}"

failures=0
checks=0

# run <expected status> <expected message substring, or -> <args...>
run() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$("$ASUNA" "$@" 2>&1)
    status=$?
    if [ "$status" != "$want_status" ]; then
        printf '  FAIL asuna %s\n    exit %s, wanted %s\n' "$*" "$status" "$want_status"
        failures=$((failures + 1))
        return
    fi
    if [ "$want_text" != "-" ]; then
        case "$output" in
            *"$want_text"*) ;;
            *)
                printf '  FAIL asuna %s\n    said: %s\n    wanted to contain: %s\n' \
                    "$*" "$output" "$want_text"
                failures=$((failures + 1))
                return
                ;;
        esac
    fi
}

# --- malformed values are refused, and say so ------------------------------
# kUsage is 2. None of these reaches the socket.

echo "malformed numbers"
run 2 "needs a whole number" start --height nope
run 2 "needs a whole number" start --fps 1.5
run 2 "needs a number"       start --scale nope
run 2 "needs a number"       scale nope
run 2 "needs a number"       move .
run 2 "needs a number"       move 1.2.3
run 2 "needs a number"       say hi --for nope

echo "out of range"
run 2 "at least 80"          start --height 40
run 2 "at least 0"           start --fps -1
run 2 "between 0.5 and 2.5"  start --scale 9
run 2 "between 0 and 1"      start --side 2
run 2 "is out of range"      start --height 2147483648
run 2 "is out of range"      start --height 99999999999999999999

echo "missing values"
run 2 "needs a value"        start --height
run 2 "needs a value"        start --scale
# Used to report "say does not take '--for'", which sent the reader looking for
# a spelling mistake in an option that was spelt correctly.
run 2 "needs a value"        say hi --for
run 2 "unknown argument"     start --bogus

echo "the two review findings"
# --x had a ceiling of INT_MAX it was never meant to have, and refused three
# billion with a complaint about zero.
run 0 "-"                    start --x 3000000000 --help
run 2 "at least 0"           start --x -1
# say --for had a millisecond floor that was never the stated rule.
run 2 "more than 0"          say hi --for 0
run 2 "more than 0"          say hi --for -3

# --- valid values are accepted ---------------------------------------------
# --help ends parsing with everything before it already validated.

echo "accepted, at the boundaries"
run 0 "-" start --height 80 --help
run 0 "-" start --fps 0 --help
run 0 "-" start --scale 0.5 --help
run 0 "-" start --scale 2.5 --help
run 0 "-" start --side 0 --help
run 0 "-" start --side 1 --help
run 0 "-" start --x 0 --help
run 0 "-" start --margin 0 --help
run 0 "-" start --gaze-halo 0 --help
run 0 "-" --help

if [ "$failures" -gt 0 ]; then
    printf '\n%s of %s checks failed\n' "$failures" "$checks"
    exit 1
fi
printf '\nall %s cli checks passed\n' "$checks"
