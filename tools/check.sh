#!/usr/bin/env bash
# Run the checks that do not need a compositor, GPU, network, or model assets.
set -euo pipefail

build_dir="${1:-build}"
export PYTHONPYCACHEPREFIX="${TMPDIR:-/tmp}/asuna-pycache-${UID}"

cmake --build "$build_dir"
ctest --test-dir "$build_dir" --output-on-failure
python3 -m compileall -q tools tests/ext_test.py tests/fake_daemon.py
ruff check tools tests/ext_test.py
if command -v pyright >/dev/null 2>&1; then
    pyright
else
    npx --yes pyright
fi
