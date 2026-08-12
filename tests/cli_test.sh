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
#     daemon or requiring one;
#   * the subcommands that go to the daemon are run with XDG_RUNTIME_DIR pointed
#     at an empty temporary directory, which is where paths::socketPath() and
#     paths::lockPath() are built from. There is no socket there and no lock, so
#     the command is fully parsed, sends nothing, and exits kNotRunning. A real
#     daemon is neither needed nor reachable, and the only thing left behind is
#     an empty directory this script removes. That is what separates "rejected
#     as bad usage" (2) from "accepted, nobody home" (3), which is exactly the
#     distinction the say --for wiring bug lived in.
#
# What is deliberately not here: that `move -500` and `scale 3.0` reach a
# running daemon and get *clamped* by it. That needs a live daemon and would
# move or resize somebody's pet as a side effect of running the test suite. What
# is here is the half above it - that those values leave the CLI intact rather
# than being refused. The clamping itself is Phase 8.

ASUNA="${1:?usage: cli_test.sh <path to asuna>}"

failures=0
checks=0

# An empty session directory: no control socket, no lock. See the note above.
NOWHERE=$(mktemp -d "${TMPDIR:-/tmp}/asuna-cli-test-XXXXXX") || exit 1
# And a config directory of our own. Config::path() is built from
# XDG_CONFIG_HOME, so `config check` can be pointed at a file this script wrote
# without going anywhere near the one the user actually runs on.
CFGROOT=$(mktemp -d "${TMPDIR:-/tmp}/asuna-cli-cfg-XXXXXX") || exit 1
trap 'rm -rf "$NOWHERE" "$CFGROOT"' EXIT INT TERM
mkdir -p "$CFGROOT/asuna"
# kNotRunning, from src/app/cli.hpp.
ABSENT=3

# Judges $status and $output against $want_status and $want_text.
verdict() {
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

# run <expected status> <expected message substring, or -> <args...>
run() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$("$ASUNA" "$@" 2>&1)
    status=$?
    verdict "$@"
}

# The same, with the session directory pointed somewhere empty, so a verb that
# talks to the daemon gets all the way through parsing and then finds nobody.
# A 2 here would mean the command was refused before it ever tried to send.
run_absent() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$(XDG_RUNTIME_DIR="$NOWHERE" "$ASUNA" "$@" 2>&1)
    status=$?
    verdict "$@"
}

# Against the config file this script wrote, not the user's.
run_cfg() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$(XDG_CONFIG_HOME="$CFGROOT" "$ASUNA" "$@" 2>&1)
    status=$?
    verdict "$@"
}

# Replaces the test config file with its arguments, one line each.
write_config() {
    : > "$CFGROOT/asuna/config.toml"
    for line in "$@"; do
        printf '%s\n' "$line" >> "$CFGROOT/asuna/config.toml"
    done
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

# --- the subcommands, accepted all the way to the socket --------------------
# Exit 3 means the value survived dispatch and the send found nobody home.

echo "accepted by the subcommands"
# The one the review asked for: proving `say --for 0` is refused does not prove
# the floor is gone, because the bug was a *two-sided* range with a 0.001 floor
# and every rejection case would still have passed with it in place. This is the
# value that separates the two, and it has to get past dispatch to say so.
run_absent "$ABSENT" "not running" say hi --for 0.0001
run_absent "$ABSENT" "not running" say hi --for 0.001
run_absent "$ABSENT" "not running" say hi --for 900
run_absent "$ABSENT" "not running" say hi
# Unranged on purpose: the daemon clamps to 0.5-2.5 and to what the screen has
# room for. Refusing 3.0 here would refuse something that currently works.
run_absent "$ABSENT" "not running" scale 3.0
run_absent "$ABSENT" "not running" scale 0.1
# Likewise: the daemon clamps x to the screen, which is the only thing that
# knows how wide it is.
run_absent "$ABSENT" "not running" move -500
run_absent "$ABSENT" "not running" move 999999
run_absent "$ABSENT" "not running" move left
run_absent "$ABSENT" "not running" hide
# And a bad value is still refused there rather than sent - a 3 would mean the
# rubbish went to the daemon to be dealt with.
run_absent 2 "needs a number" scale nope
run_absent 2 "needs a number" move 1.2.3
run_absent 2 "more than 0"    say hi --for 0

# --- `config check`, and what it says in each shape --------------------------
# Reads a file this script wrote, via XDG_CONFIG_HOME. Nothing here starts a
# daemon or looks at the user's own config.

echo "config check"
write_config '[strip]' 'height = 460'
run_cfg 0 "is fine" config check
# A file that parses and contains a setting another setting overrides. Non-fatal
# on purpose - `config check` exiting non-zero here would fail a config that
# runs perfectly well, which is the whole reason this is not a `problem`.
write_config '[strip]' 'height = 900'
run_cfg 0 "do nothing"  config check
run_cfg 0 "max_height"  config check
# A real problem still fails, and still names itself.
write_config '[strip]' 'height = "tall"'
run_cfg 1 "should be a number" config check
# No file at all is a valid state, not an error.
rm -f "$CFGROOT/asuna/config.toml"
run_cfg 0 "all defaults" config check

echo "config check --json"
# Human text where a machine-readable reply was asked for is the bug this
# covers: every one of these used to print the same prose as the runs above.
write_config '[strip]' 'height = 460'
run_cfg 0 '"ok":true'      --json config check
run_cfg 0 '"warnings":[]'  --json config check
run_cfg 0 '"exists":true'  --json config check
write_config '[strip]' 'height = 900'
run_cfg 0 '"ok":true'      --json config check
run_cfg 0 "max_height"     --json config check
# Problems and warnings arrive together, so one edit can fix both.
write_config '[strip]' 'height = 900' 'nonsense = 1'
run_cfg 1 '"ok":false'     --json config check
run_cfg 1 '"problems":["' --json config check
run_cfg 1 "max_height"     --json config check
rm -f "$CFGROOT/asuna/config.toml"
run_cfg 0 '"exists":false' --json config check

if [ "$failures" -gt 0 ]; then
    printf '\n%s of %s checks failed\n' "$failures" "$checks"
    exit 1
fi
printf '\nall %s cli checks passed\n' "$checks"
