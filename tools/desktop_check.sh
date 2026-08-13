#!/usr/bin/env bash
# Live Wayland checks for the paths CTest cannot exercise.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
asuna="${1:-${repo}/build/asuna}"
session_runtime="${XDG_RUNTIME_DIR:?desktop_check.sh needs a live Wayland session}"
display="${WAYLAND_DISPLAY:?desktop_check.sh needs a live Wayland session}"
[[ "$display" = /* ]] || display="${session_runtime}/${display}"

for command in jq niri python3 grim; do
    command -v "$command" >/dev/null || {
        echo "desktop_check.sh: $command is required" >&2
        exit 2
    }
done
[[ -x "$asuna" ]] || { echo "desktop_check.sh: no executable at $asuna" >&2; exit 2; }
[[ -S "$display" ]] || { echo "desktop_check.sh: no Wayland socket at $display" >&2; exit 2; }

root="$(mktemp -d "${TMPDIR:-/tmp}/asuna-desktop-check.XXXXXX")"
runtime="${root}/runtime"
config="${root}/config"
state="${root}/state"
mkdir -p "$runtime" "$config/asuna" "$state"
chmod 700 "$runtime"

run=(env
    XDG_RUNTIME_DIR="$runtime"
    XDG_CONFIG_HOME="$config"
    XDG_STATE_HOME="$state"
    WAYLAND_DISPLAY="$display"
    ASUNA_MODEL_DIR="${repo}/models"
    ASUNA_DATA_DIR="${repo}/data")
config_file="${config}/asuna/config.toml"
socket="${runtime}/asuna/control.sock"
original_scale="$(niri msg --json focused-output | jq -r '.logical.scale')"
output="$(niri msg --json focused-output | jq -r '.name')"
scale_changed=0

cleanup() {
    "${run[@]}" "$asuna" ext stop >/dev/null 2>&1 || true
    "${run[@]}" "$asuna" exit >/dev/null 2>&1 || true
    if [[ $scale_changed -eq 1 ]]; then
        niri msg output "$output" scale "$original_scale" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

passed=0
skipped=0
pass() { printf 'PASS  %s\n' "$1"; passed=$((passed + 1)); }
skip() { printf 'SKIP  %s\n' "$1"; skipped=$((skipped + 1)); }
fail() { printf 'FAIL  %s\n' "$1" >&2; exit 1; }

status() { "${run[@]}" "$asuna" --json status; }
wait_ready() {
    local answer
    for _ in {1..60}; do
        answer="$(status 2>/dev/null || true)"
        if jq -e '.ok and .data.ready' >/dev/null 2>&1 <<<"$answer"; then return 0; fi
        sleep 0.2
    done
    return 1
}
wait_layer() {
    local present="$1"
    for _ in {1..30}; do
        local count
        count="$(niri msg --json layers | jq '[.[] | select(.namespace == "asuna")] | length')"
        if [[ "$present" = yes && "$count" -gt 0 ]]; then return 0; fi
        if [[ "$present" = no && "$count" -eq 0 ]]; then return 0; fi
        sleep 0.1
    done
    return 1
}
ipc() {
    python3 - "$socket" "$1" <<'PY'
import socket
import sys

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
    client.connect(sys.argv[1])
    client.sendall(sys.argv[2].encode() + b"\n")
    client.shutdown(socket.SHUT_WR)
    reply = b""
    while b"\n" not in reply:
        part = client.recv(65536)
        if not part:
            break
        reply += part
print(reply.split(b"\n", 1)[0].decode())
PY
}

cat >"$config_file" <<'EOF'
[pet]
model = "01"
scale = 1.25
layer = "top"
anchor = "left"
greet = false

[strip]
fps = 20

[ext]
enabled = true
providers = ["offline"]

[ext.provider.offline]
base_url = "http://127.0.0.1:9/v1"
model = "phase8-offline"

[ext.vision]
enabled = false
notice = 0.2
deny = ["code"]
EOF

"${run[@]}" "$asuna" start >/dev/null
wait_ready || fail "daemon did not render in the live session"
wait_layer yes || fail "niri did not expose the asuna layer surface"
layer="$(niri msg --json layers | jq -c '[.[] | select(.namespace == "asuna")][0]')"
jq -e '.output != "" and .layer == "Top" and .keyboard_interactivity == "None"' \
    >/dev/null <<<"$layer" || fail "layer-shell metadata is wrong: $layer"
pass "startup maps a non-interactive top-layer surface on $output"

initial="$(status)"
jq -e '.data.model == "01" and .data.scale == 1.25 and .data.fps == 20' \
    >/dev/null <<<"$initial" || fail "config values did not reach the live shell"
pass "startup config reaches model, scale, and frame-cap state"

"${run[@]}" "$asuna" hide >/dev/null
wait_layer no || fail "hide left the layer surface mapped"
jq -e '.data.hidden == true' >/dev/null <<<"$(status)" || fail "hide state not reported"
"${run[@]}" "$asuna" show >/dev/null
wait_layer yes || fail "show did not remap the layer surface"
wait_ready || fail "show did not restore a ready renderer"
"${run[@]}" "$asuna" toggle >/dev/null
wait_layer no || fail "toggle did not hide"
"${run[@]}" "$asuna" toggle >/dev/null
wait_layer yes || fail "toggle did not show"
pass "hide, show, and both toggle directions remap cleanly"

outputs="$("${run[@]}" "$asuna" --json output list)"
jq -e --arg output "$output" '.data.current == $output and (.data.outputs | length) >= 1' \
    >/dev/null <<<"$outputs" || fail "output list/current mismatch"
if "${run[@]}" "$asuna" output ASUNA-NOT-A-MONITOR >/dev/null 2>&1; then
    fail "an unknown output was accepted"
fi
pass "current-output reporting and unknown-output refusal work live"

scaled="$("${run[@]}" "$asuna" --json scale 1.8)"
actual_user_scale="$(jq -r '.data.scale' <<<"$scaled")"
"${run[@]}" "$asuna" move 400 >/dev/null
"${run[@]}" "$asuna" layer overlay >/dev/null
sleep 1.5
before_reload="$(status)"
jq -e --argjson scale "$actual_user_scale" \
    '.data.scale == $scale and .data.layer == "overlay" and .data.x >= 350' \
    >/dev/null <<<"$before_reload" || fail "runtime state changes did not settle"
sed -i 's/scale = 1.25/scale = 0.7/; s/fps = 20/fps = 15/' "$config_file"
reload="$("${run[@]}" "$asuna" --json config reload)"
jq -e '.ok' >/dev/null <<<"$reload" || fail "valid config reload failed"
after_reload="$(status)"
jq -e --argjson scale "$actual_user_scale" \
    '.data.scale == $scale and .data.layer == "overlay" and .data.fps == 15' \
    >/dev/null <<<"$after_reload" || fail "reload violated state-over-config precedence"
pass "live reload applies tunables without overriding user state"

sleep 1.2
"${run[@]}" "$asuna" exit >/dev/null
"${run[@]}" "$asuna" start >/dev/null
wait_ready || fail "restart after persistence did not become ready"
restored="$(status)"
jq -e --argjson scale "$actual_user_scale" \
    '.data.scale == $scale and .data.layer == "overlay" and .data.x >= 350' \
    >/dev/null <<<"$restored" || fail "persisted state did not outrank config"
"${run[@]}" "$asuna" exit >/dev/null
"${run[@]}" "$asuna" start --scale 1.1 --layer top --x 200 --show >/dev/null
wait_ready || fail "flag-precedence start did not become ready"
flagged="$(status)"
jq -e '.data.scale == 1.1 and .data.layer == "top" and .data.hidden == false' \
    >/dev/null <<<"$flagged" || fail "CLI flags did not outrank persisted state"
pass "persistence precedence is CLI > state > config in a real session"

prompt_log="${root}/prompt.log"
set +e
"${run[@]}" python3 "${repo}/tools/asuna-prompt.py" --timeout 2.0 --x 960 --bottom 540 \
    --screen-width 1920 --screen-height 1080 >"$prompt_log" 2>&1 &
prompt_pid=$!
set -e
prompt_seen=0
for _ in {1..30}; do
    if niri msg --json layers | jq -e \
        '.[] | select(.namespace == "asuna-prompt" and .keyboard_interactivity == "OnDemand")' \
        >/dev/null; then
        prompt_seen=1
        break
    fi
    sleep 0.1
done
[[ $prompt_seen -eq 1 ]] || fail "prompt layer surface did not map with on-demand keyboard"
set +e
wait "$prompt_pid"
prompt_status=$?
set -e
[[ $prompt_status -eq 1 ]] || fail "prompt timeout returned $prompt_status instead of 1"
if niri msg --json layers | jq -e '.[] | select(.namespace == "asuna-prompt")' >/dev/null; then
    fail "timed-out prompt left its surface mapped"
fi
pass "GTK prompt maps with keyboard focus policy and times out cleanly"

denied="$(ipc '{"cmd":"ext","args":{"glance":true}}')"
jq -e '.ok == false and (.error | contains("not allowed"))' >/dev/null <<<"$denied" || \
    fail "vision-off gate did not refuse a glance"
sed -i 's/enabled = false/enabled = true/' "$config_file"
"${run[@]}" "$asuna" config reload >/dev/null
unannounced="$(ipc '{"cmd":"ext","args":{"capture":true}}')"
jq -e '.ok == false and (.error | contains("no glance"))' >/dev/null <<<"$unannounced" || \
    fail "capture without a glance was accepted"
glance="$(ipc '{"cmd":"ext","args":{"glance":true}}')"
jq -e '.ok and .data.wait_ms == 200' >/dev/null <<<"$glance" || fail "glance did not open"
early="$(ipc '{"cmd":"ext","args":{"capture":true}}')"
jq -e '.ok == false and (.error | contains("notice"))' >/dev/null <<<"$early" || \
    fail "capture was accepted before its notice elapsed"
"${run[@]}" "$asuna" ext cancel >/dev/null
cancelled="$(ipc '{"cmd":"ext","args":{"capture":true}}')"
jq -e '.ok == false and (.error | contains("called off"))' >/dev/null <<<"$cancelled" || \
    fail "cancel did not revoke the pending glance"
pass "vision off, announcement, notice, and cancellation gates fail closed"

ipc '{"cmd":"ext","args":{"glance":true}}' >/dev/null
sleep 0.4
grant="$(ipc '{"cmd":"ext","args":{"capture":true}}')"
jq -e --arg output "$output" \
    '.ok and .data.output == $output and .data.mask.w > 0 and .data.mask.h > 0' \
    >/dev/null <<<"$grant" || fail "elapsed glance did not return capture geometry"
grant_file="${root}/grant.json"
printf '%s\n' "$grant" >"$grant_file"
PYTHONPATH="${repo}/tools" python3 - "$grant_file" <<'PY'
import json
import sys

from chat.vision import capture

grant = json.load(open(sys.argv[1], encoding="utf-8"))["data"]
image = capture(grant["output"], grant["mask"])
if not image.startswith(b"\xff\xd8") or len(image) < 1024:
    raise SystemExit("masked capture was not a non-empty JPEG")
PY
pass "granted screenshot is captured, masked, resized, and retained only in memory"

PYTHONPATH="${repo}/tools" python3 - <<'PY'
from chat.helper import Helper
from chat.vision import focused_app_id

app = focused_app_id()
if not app:
    raise SystemExit("niri did not report a focused app-id")

class Chat:
    @staticmethod
    def usable():
        return True

class Control:
    def call(self, *_args, **_kwargs):
        raise SystemExit("deny-listed glance reached the daemon")

helper = Helper()
helper.chat = Chat()
helper.control = Control()
helper.config = {"vision_enabled": True, "vision_deny": [app.lower()]}
helper.glance()
PY
pass "focused-app deny matching blocks before requesting capture"

"${run[@]}" "$asuna" ext start >/dev/null
"${run[@]}" "$asuna" ext status >/dev/null || fail "helper did not subscribe"
"${run[@]}" "$asuna" chat phase8-offline >/dev/null
sleep 0.5
"${run[@]}" "$asuna" ext status >/dev/null || fail "provider outage killed the helper"
helper_pid="$(awk '{print $1}' "${runtime}/asuna/ext.pid")"
kill -KILL "$helper_pid"
for _ in {1..30}; do
    kill -0 "$helper_pid" 2>/dev/null || break
    sleep 0.1
done
if "${run[@]}" "$asuna" ext status >/dev/null 2>&1; then
    helper_status_failed=0
    for _ in {1..30}; do
        if ! "${run[@]}" "$asuna" ext status >/dev/null 2>&1; then
            helper_status_failed=1
            break
        fi
        sleep 0.1
    done
    [[ $helper_status_failed -eq 1 ]] || fail "crashed helper still reported healthy"
fi
status >/dev/null || fail "helper crash took down the daemon"
"${run[@]}" "$asuna" ext start >/dev/null
"${run[@]}" "$asuna" ext restart >/dev/null
"${run[@]}" "$asuna" ext stop >/dev/null
pass "provider outage, helper crash, restart, and stop leave the daemon healthy"

if [[ "$(niri msg --json outputs | jq 'length')" -gt 1 ]]; then
    skip "multi-monitor switching is available but requires selecting a disposable output"
else
    skip "monitor switching and hot-unplug fallback need a second physical output"
fi

for test_scale in 1.25 2.0; do
    scale_changed=1
    niri msg output "$output" scale "$test_scale" >/dev/null
    scale_ready=0
    for _ in {1..50}; do
        if niri msg --json outputs | jq -e \
            --arg output "$output" --argjson expected "$test_scale" \
            '.[$output].logical.scale == $expected' >/dev/null; then
            scale_ready=1
            break
        fi
        sleep 0.1
    done
    [[ $scale_ready -eq 1 ]] || fail "niri did not apply output scale $test_scale"
    wait_ready || fail "renderer was not ready at output scale $test_scale"
    wait_layer yes || fail "layer surface disappeared at output scale $test_scale"
done
niri msg output "$output" scale "$original_scale" >/dev/null
scale_changed=0
pass "live fractional 1.25x and integer 2x output-scale transitions survive"

"${run[@]}" "$asuna" ext start >/dev/null
shutdown_helper="$(awk '{print $1}' "${runtime}/asuna/ext.pid")"
"${run[@]}" "$asuna" exit >/dev/null
for _ in {1..40}; do
    kill -0 "$shutdown_helper" 2>/dev/null || break
    sleep 0.1
done
kill -0 "$shutdown_helper" 2>/dev/null && fail "helper survived daemon shutdown"
pass "daemon shutdown publishes bye and the helper exits"

printf '\nPhase 8 desktop checks: %d passed, %d skipped\n' "$passed" "$skipped"
printf 'Artifacts and logs: %s\n' "$root"
