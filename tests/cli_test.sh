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
# Short, because a unix socket path has about 100 bytes to fit in and mktemp
# under a long TMPDIR does not. Only used when a fake daemon is wanted.
FAKEDIR=$(mktemp -d /tmp/asuna-fd-XXXXXX) || exit 1
FAKE_PID=""
cleanup() {
    [ -n "$FAKE_PID" ] && kill "$FAKE_PID" 2>/dev/null
    rm -rf "$NOWHERE" "$CFGROOT" "$FAKEDIR"
}
trap cleanup EXIT INT TERM
mkdir -p "$CFGROOT/asuna"
# kNotRunning, from src/app/cli.hpp.
ABSENT=3

HERE=$(dirname "$0")

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

# Neither her session nor her config: an empty runtime directory and the config
# file this script writes. For the verbs that read both, so that nothing here
# depends on how whoever runs the suite has configured Asuna.
run_alone() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$(XDG_RUNTIME_DIR="$NOWHERE" XDG_CONFIG_HOME="$CFGROOT" "$ASUNA" "$@" 2>&1)
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

# Starts tests/fake_daemon.py on a socket of our own, answering with the reply
# lines given as arguments. Waits for it to be listening, so nothing here sleeps
# or races. Everything after this until stop_fake() talks to it and not to her.
start_fake() {
    stop_fake
    : > "$FAKEDIR/replies"
    for line in "$@"; do
        printf '%s\n' "$line" >> "$FAKEDIR/replies"
    done
    mkdir -p "$FAKEDIR/asuna"
    python3 "$HERE/fake_daemon.py" "$FAKEDIR/asuna/control.sock" "$FAKEDIR/replies" \
        > "$FAKEDIR/ready" 2>"$FAKEDIR/err" &
    FAKE_PID=$!
    n=0
    while [ ! -s "$FAKEDIR/ready" ] && [ "$n" -lt 100 ]; do
        n=$((n + 1))
        # No `sleep 0`: this is a hundred short waits at most and the socket is
        # usually up on the first.
        command sleep 0.05
    done
    [ -s "$FAKEDIR/ready" ]
}

stop_fake() {
    if [ -n "$FAKE_PID" ]; then
        kill "$FAKE_PID" 2>/dev/null
        wait "$FAKE_PID" 2>/dev/null
        FAKE_PID=""
    fi
}

# Against the fake daemon rather than hers.
run_fake() {
    want_status="$1"
    want_text="$2"
    shift 2
    checks=$((checks + 1))
    output=$(XDG_RUNTIME_DIR="$FAKEDIR" XDG_CONFIG_HOME="$CFGROOT" "$ASUNA" "$@" 2>&1)
    status=$?
    verdict "$@"
}

# stdout on its own has to be exactly one JSON document. Checking a substring of
# stdout+stderr cannot see a stray human line printed alongside the reply, which
# is what would actually break `asuna --json ... | jq`.
run_json() {
    want_status="$1"
    shift
    checks=$((checks + 1))
    out_file="$FAKEDIR/stdout"
    XDG_CONFIG_HOME="$CFGROOT" XDG_RUNTIME_DIR="$FAKEDIR" "$ASUNA" "$@" \
        > "$out_file" 2>/dev/null
    status=$?
    if [ "$status" != "$want_status" ]; then
        printf '  FAIL asuna %s\n    exit %s, wanted %s\n' "$*" "$status" "$want_status"
        failures=$((failures + 1))
        return
    fi
    if ! python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$out_file" 2>/dev/null; then
        printf '  FAIL asuna %s\n    stdout is not exactly one JSON document:\n%s\n' \
            "$*" "$(cat "$out_file")"
        failures=$((failures + 1))
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

# --- the `ext` grammar -------------------------------------------------------
# Every one of these is refused before anything is read, sent or signalled, so
# they need neither a daemon nor a pid file. --force used to be looked for
# anywhere in the argument list and everything else ignored, which accepted four
# commands that read as if they did something: `ext status --force` treated a
# safety-sensitive option as irrelevant, and `ext stop garbage` exited 0.

echo "ext takes exactly its verb, and --force only where it means something"
# The verb is judged first, so a wrong one is answered with the list of verbs
# rather than with a complaint about an option that was never the problem.
run_alone 2 "ext takes"     ext bogus
run_alone 2 "ext takes"     ext bogus --force
run_alone 2 "does not take" ext stop garbage
run_alone 2 "does not take" ext cancel garbage
run_alone 2 "does not take" ext status garbage
run_alone 2 "does not take" ext restart garbage
run_alone 2 "does not take" ext stop --forced
run_alone 2 "only for"      ext status --force
run_alone 2 "only for"      ext start --force
run_alone 2 "only for"      ext test --force
run_alone 2 "only for"      ext cancel --force
run_alone 2 "only for"      ext config --force
run_alone 2 "given twice"   ext stop --force --force
run_alone 2 "given twice"   ext restart --force --force

echo "and the four forms that are the grammar still get through it"
# A 2 in any of these would mean the strictness had eaten something real.
# `stop` decides everything from the pid file, and there is none here.
rm -f "$NOWHERE/asuna/ext.pid"
run_alone 0 "not running" ext stop
run_alone 0 "not running" ext stop --force
# `restart` reads the config, which is this script's own with extensions off, so
# both forms are parsed, accepted, and stop at the first real gate.
write_config '[ext]' 'enabled = false'
run_alone 1 "extensions are off" ext restart
run_alone 1 "extensions are off" ext restart --force

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
run_cfg 0 "worth knowing" config check
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

echo "config check: stdout is one JSON document, not prose"
write_config '[strip]' 'height = 460'
run_json 0 --json config check
write_config '[strip]' 'height = 900'
run_json 0 --json config check
write_config '[strip]' 'height = 900' 'nonsense = 1'
run_json 1 --json config check

echo "config check: a key readable by others is said in both forms"
# Not a problem with the file's contents, so it lives outside `problems` - but
# it is the one diagnostic here with a security consequence, and the JSON form
# used to drop it entirely because each branch computed its own warnings.
write_config '[ext]' 'enabled = false' 'providers = ["x"]' \
    '[ext.provider.x]' 'base_url = "https://example.invalid/v1"' \
    'model = "m"' 'api_key = "sk-not-a-real-key"'
chmod 644 "$CFGROOT/asuna/config.toml"
run_cfg 0 "readable by others" config check
run_cfg 0 "readable by others" --json config check
run_cfg 0 '"warnings":["' --json config check
# And once it is only readable by its owner, neither form mentions it.
chmod 600 "$CFGROOT/asuna/config.toml"
run_cfg 0 "is fine"        config check
run_cfg 0 '"warnings":[]'  --json config check
# It is reported even when the file also has a fatal problem in it: a typo on
# line 12 does not make a world-readable key wait its turn.
write_config '[strip]' 'nonsense = 1' '[ext]' 'enabled = false' 'providers = ["x"]' \
    '[ext.provider.x]' 'base_url = "https://example.invalid/v1"' \
    'model = "m"' 'api_key = "sk-not-a-real-key"'
chmod 644 "$CFGROOT/asuna/config.toml"
run_cfg 1 "readable by others" config check
run_cfg 1 "readable by others" --json config check
rm -f "$CFGROOT/asuna/config.toml"

# --- what comes back from a daemon -----------------------------------------
# Against tests/fake_daemon.py, which answers with a canned line. These paths
# print what the *daemon* sent, so they cannot be reached with no daemon at all
# - and reaching them with the real one would mean reloading somebody's config
# and reporting on their helper.

echo "config reload"
start_fake '{"ok":true,"data":{"path":"/tmp/c.toml","note":"","warnings":[]}}' || exit 1
run_fake 0 "reloaded /tmp/c.toml" config reload
stop_fake

start_fake '{"ok":true,"data":{"path":"/tmp/c.toml","note":"","warnings":["max_height does nothing"]}}' || exit 1
# A reload that happened, and something it applied that does nothing. Both.
run_fake 0 "reloaded /tmp/c.toml"    config reload
run_fake 0 "max_height does nothing" config reload
stop_fake

# A refusal carries the problems *and* the warnings, so one edit can fix both.
# Holding the warnings back would mean fixing the typo, reloading, and only then
# being told about the setting that does nothing.
start_fake '{"ok":false,"error":"config not reloaded - 1 problem(s)","data":{"path":"/tmp/c.toml","problems":["line 12: bad"],"warnings":["max_height does nothing"]}}' || exit 1
run_fake 1 "line 12: bad"            config reload
run_fake 1 "max_height does nothing" config reload
stop_fake

echo "ext status: one verdict, whichever form asked"
# The helper is neither in the pid file nor subscribed.
rm -f "$FAKEDIR/asuna/ext.pid"
start_fake '{"ok":true,"data":{"subscribers":0,"enabled":true,"vision_enabled":false}}' || exit 1
run_fake "$ABSENT" "helper not running" ext status
# Same state, JSON: the payload is the raw reply, and the exit code is where the
# pid file and the subscriber count are combined. It used to be 0 regardless,
# which left a --json caller unable to tell from either half.
run_fake "$ABSENT" "-" --json ext status
run_json "$ABSENT" --json ext status
stop_fake

# Subscribed, but no pid file: something is listening and it is not ours.
start_fake '{"ok":true,"data":{"subscribers":1,"enabled":true,"vision_enabled":false}}' || exit 1
run_fake "$ABSENT" "not ours" ext status
run_fake "$ABSENT" "-"        --json ext status
# And with a pid file naming this very shell, which is alive and verifiable.
printf '%d %s\n' "$$" "$(awk '{print $22}' "/proc/$$/stat")" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "helper running" ext status
run_fake 0 "-"              --json ext status
run_json 0 --json ext status
# A pid file from an older build: `status` still reports it rather than calling
# a live helper absent...
printf '%d\n' "$$" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "helper running" ext status
# ...and `stop` refuses to signal what it cannot prove, naming the pid and the
# way through. This is the case that could otherwise SIGTERM a stranger.
run_fake 1 "cannot be"      ext stop
run_fake 1 "--force"        ext stop
# A pid file whose recorded start time does not match is not a helper at all,
# and is never signalled - forced or otherwise. The pid here is this script: if
# --force ever reached a recycled identity, the suite would die where it stands.
printf '%d 424242\n' "$$" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "different process" ext stop
run_fake 0 "not running"       ext stop
printf '%d 424242\n' "$$" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "different process" ext stop --force
run_fake 0 "not running"       ext stop --force
stop_fake
rm -f "$FAKEDIR/asuna/ext.pid"

# --- what `ext stop` actually signals ---------------------------------------
#
# The section above is about refusals, which can be checked against pid files
# naming this script. These are the paths that end in a real signal, so they need
# a process of their own to stand in for the helper: `sleep`, which this script
# starts and is therefore allowed to kill. Nothing here needs a daemon - `stop`
# decides everything from the pid file - and nothing here touches the user's
# session, because the pid file is the one under $FAKEDIR.

mkdir -p "$FAKEDIR/asuna"

# Reaps $1 and checks it was killed by SIGTERM. `wait` reporting 128 + 15 is what
# actually happened to that process; looking the pid up again afterwards would be
# asking about a number that is by then free to be reused.
killed_by_term() {
    checks=$((checks + 1))
    wait "$1" 2>/dev/null
    left=$?
    if [ "$left" != 143 ]; then
        printf '  FAIL the stand-in helper (pid %s) left with %s, wanted 143 (SIGTERM)\n' \
            "$1" "$left"
        failures=$((failures + 1))
    fi
}

killed_by_kill() {
    checks=$((checks + 1))
    wait "$1" 2>/dev/null
    left=$?
    if [ "$left" != 137 ]; then
        printf '  FAIL the stubborn helper (pid %s) left with %s, wanted 137 (SIGKILL)\n' \
            "$1" "$left"
        failures=$((failures + 1))
    fi
}

# gone_pidfile <what this was>: a stop that stopped something removes the file.
gone_pidfile() {
    checks=$((checks + 1))
    if [ -e "$FAKEDIR/asuna/ext.pid" ]; then
        printf '  FAIL %s: %s is still there\n' "$1" "$FAKEDIR/asuna/ext.pid"
        failures=$((failures + 1))
    fi
}

# kept_pidfile <what this was>: and a stop that could not happen leaves it, since
# it is the only record of what still needs stopping.
kept_pidfile() {
    checks=$((checks + 1))
    if [ ! -e "$FAKEDIR/asuna/ext.pid" ]; then
        printf '  FAIL %s: %s was deleted while its helper was still running\n' \
            "$1" "$FAKEDIR/asuna/ext.pid"
        failures=$((failures + 1))
    fi
}

# still_running <pid> <what this was>: field 3 of /proc/<pid>/stat, because
# `kill -0` cannot tell a live process from one this script has killed and not
# yet reaped - a zombie child accepts signal 0 from its parent.
still_running() {
    checks=$((checks + 1))
    if [ "$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)" != S ]; then
        printf '  FAIL %s: the stand-in helper (pid %s) was signalled anyway\n' "$2" "$1"
        failures=$((failures + 1))
    fi
}

# The start time as /proc records it, which is what makes the pid an identity.
started_at() { awk '{print $22}' "/proc/$1/stat"; }

echo "ext stop: a proven identity is signalled, once, and then forgotten"
sleep 300 &
SLEEPER=$!
printf '%d %s\n' "$SLEEPER" "$(started_at "$SLEEPER")" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "helper stopped" ext stop
killed_by_term "$SLEEPER"
gone_pidfile "a proven stop"

echo "ext stop: a helper that ignores SIGTERM is confirmed gone after SIGKILL"
# The stop path waits five seconds before escalating. The important second half
# is that success is not printed and the pid file is not removed until the
# pidfd reports the process has actually exited. This child stays unreaped by
# this shell during `run_fake`, so a signal-0 probe would incorrectly call its
# zombie alive and time out; poll(pidfd) reports the exit correctly.
python3 -c 'import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); signal.pause()' &
STUBBORN=$!
command sleep 0.05
printf '%d %s\n' "$STUBBORN" "$(started_at "$STUBBORN")" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "helper stopped" ext stop
killed_by_kill "$STUBBORN"
gone_pidfile "a SIGKILL stop confirmed through its pidfd"

echo "ext stop --force: the one identity it is for, and it does signal it"
# One field, so nothing can prove what it names - the upgrade case. Refused
# without --force, carried out with it, and said out loud either way.
sleep 300 &
SLEEPER=$!
printf '%d\n' "$SLEEPER" > "$FAKEDIR/asuna/ext.pid"
run_fake 1 "cannot be"      ext stop
run_fake 0 "helper stopped" ext stop --force
killed_by_term "$SLEEPER"
gone_pidfile "a forced stop"
# The warning that goes with it, which needs a second stand-in: each run asserts
# one message, and the run above ended the first one.
sleep 300 &
SLEEPER=$!
printf '%d\n' "$SLEEPER" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "without having proved" ext stop --force
killed_by_term "$SLEEPER"

echo "ext stop: a pid that is not a process is not signalled, forced or not"
# A pid this script owned and reaped, so it names nothing. --force has nothing to
# pin, which is the third condition on reaching the unproven path at all.
sleep 0 &
FREED=$!
wait "$FREED" 2>/dev/null
printf '%d %s\n' "$FREED" 424242 > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "not running" ext stop --force
gone_pidfile "a stop on a pid that is not a process"
printf '%d\n' "$FREED" > "$FAKEDIR/asuna/ext.pid"
run_fake 0 "not running" ext stop --force
gone_pidfile "a forced stop on a one-field file naming nothing"

echo "ext stop: a shortage of descriptors refuses rather than guesses"
# The one refusal where nothing can be proved and nothing has gone either. Under
# `ulimit -n 4` there is a descriptor for the loader but not for two files at
# once, so either pidfd_open or the /proc read that confirms the identity fails
# with EMFILE. That used to be indistinguishable from a start time that did not
# match: a live helper was reported gone and the one file naming it was deleted.
# It has to fail closed - nothing signalled, nothing removed - because a safety
# property that lapses under load is absent exactly when it is needed.
#
# Four is enough for the loader here; somewhere with more shared libraries the
# binary may not start at all, which is no failure of what is under test. So the
# two invariants are checked either way and the message only when it ran.
sleep 300 &
SLEEPER=$!
printf '%d %s\n' "$SLEEPER" "$(started_at "$SLEEPER")" > "$FAKEDIR/asuna/ext.pid"
checks=$((checks + 1))
starved=$( (ulimit -n 4
            XDG_RUNTIME_DIR="$FAKEDIR" XDG_CONFIG_HOME="$CFGROOT" "$ASUNA" ext stop 2>&1) )
starved_status=$?
if [ "$starved_status" = 127 ]; then
    printf '  note: `ulimit -n 4` will not start the binary here, so only the two\n'
    printf '        invariants were checked and not the message\n'
elif [ "$starved_status" = 0 ]; then
    printf '  FAIL ext stop under `ulimit -n 4` reported a stop that did not happen\n'
    printf '    said: %s\n' "$starved"
    failures=$((failures + 1))
else
    case "$starved" in
        *"safe hold"*) ;;
        *)
            printf '  FAIL ext stop under `ulimit -n 4`\n    said: %s\n' "$starved"
            printf '    wanted to contain: safe hold\n'
            failures=$((failures + 1))
            ;;
    esac
fi
still_running "$SLEEPER" "a stop that ran out of descriptors"
kept_pidfile "a stop that ran out of descriptors"
kill "$SLEEPER" 2>/dev/null
wait "$SLEEPER" 2>/dev/null
rm -f "$FAKEDIR/asuna/ext.pid"

# --- what `ext start` does about a pid file it could not write ---------------
#
# The only process that knows the helper's pid is the middle one of the double
# fork, so it is the only one that can write the file - and its exit status was
# discarded. A failed write was therefore reported as a successful start, leaving
# a helper that `ext status` printed as pid 0 and `ext stop` could not signal at
# all. Both halves are checked here: that the failure is reported, and that the
# helper is taken back down rather than left running under a name nothing has.
#
# The stand-in helper writes an exec marker and then becomes `sleep 314159`.
# The argument is distinctive so that looking for strays cannot match anything
# else on the machine, and long enough that a leaked one would be obvious.
MARKER="sleep 314159"
EXEC_MARKER="$FAKEDIR/helper-execed"
HELPER="$FAKEDIR/helper.sh"
printf '%s\n' '#!/bin/sh' ': > "$1"' 'exec sleep 314159' > "$HELPER"
chmod +x "$HELPER"

# no_stray <what this was>: nothing of ours is still running.
no_stray() {
    if ! command -v pgrep > /dev/null 2>&1; then
        printf '  note: no pgrep here, so strays after %s were not checked\n' "$1"
        return
    fi
    checks=$((checks + 1))
    if pgrep -f "$MARKER" > /dev/null 2>&1; then
        printf '  FAIL %s: left a helper running that nothing could name\n' "$1"
        failures=$((failures + 1))
        pkill -f "$MARKER" 2>/dev/null
    fi
}

write_config '[ext]' 'enabled = true' "command = \"$HELPER $EXEC_MARKER\"" 'providers = ["x"]' \
    '[ext.provider.x]' 'base_url = "https://example.invalid/v1"' 'model = "m"'

echo "ext start: a helper it could not record is not left running"
# The write is made to fail with a *directory* where the pid file goes: the
# temporary is written normally and the rename onto it cannot succeed. The
# helper waits behind the startup gate until that rename succeeds, so this also
# proves a failed identity write never releases the configured program to exec.
start_fake '{"ok":true,"data":{"subscribers":1,"enabled":true,"vision_enabled":false}}' || exit 1
rm -f "$EXEC_MARKER"
rm -f "$FAKEDIR/asuna/ext.pid"
mkdir -p "$FAKEDIR/asuna/ext.pid"
run_fake 1 "could not write" ext start
rmdir "$FAKEDIR/asuna/ext.pid" 2>/dev/null
rm -f "$FAKEDIR/asuna/ext.pid.tmp"
no_stray "a start that could not write its pid file"
checks=$((checks + 1))
if [ -e "$EXEC_MARKER" ]; then
    printf '  FAIL a failed pid-file write released the configured helper to exec\n'
    failures=$((failures + 1))
fi

echo "ext start: and the ordinary path still starts one and records it"
# The other side of the same change: propagating the failure must not have made
# a working start report one. This goes all the way through - the helper is
# launched, the file names it, and a plain `ext stop` proves that identity and
# signals it.
rm -f "$FAKEDIR/asuna/ext.pid"
run_fake 0 "helper running" ext start
kept_pidfile "a helper that started"
checks=$((checks + 1))
if [ ! -e "$EXEC_MARKER" ]; then
    printf '  FAIL a successful pid-file write did not release the configured helper\n'
    failures=$((failures + 1))
fi
run_fake 0 "helper stopped" ext stop
gone_pidfile "a stop after a real start"
no_stray "a start followed by a stop"
stop_fake

if [ "$failures" -gt 0 ]; then
    printf '\n%s of %s checks failed\n' "$failures" "$checks"
    exit 1
fi
printf '\nall %s cli checks passed\n' "$checks"
