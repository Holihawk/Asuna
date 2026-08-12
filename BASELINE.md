# Asuna — Phase 0 Baseline

Recorded 2026-08-12, before any change from `improve.md` is applied.

Purpose: pin down what the tree does *now*, so that any behaviour change in a
later phase can be attributed to that phase and nothing else. Nothing in this
document is a recommendation. No source file was modified to produce it.

---

## 1. Environment

The machine this baseline was taken on matches the README's stated
configuration exactly.

| | Recorded | README claims |
| --- | --- | --- |
| Compositor | niri (session live, `wayland-1`) | niri 26.04 |
| GTK | 4.22.4 | 4.22.4 |
| gtk4-layer-shell | 1.3.0 | 1.3.0 |
| GLib | 2.88.3 | 2.88.3 |
| GLFW | 3.4.0 (**found**) | — |
| Compiler | GCC 16.1.1 (Red Hat 16.1.1-2) | GCC 16.1.1 |
| CMake | 4.3.0 | CMake 4.3 |
| Python | 3.14.6 | — |
| Generator | Unix Makefiles (ninja 1.13.2 also present) | — |
| Build type | Release | Release |

`ruff` and `mypy` are **not installed**. Phase 7 assumes they can be run; on
this machine they cannot be, yet.

Link order is correct — `libgtk4-layer-shell.so.0` precedes `libgtk-4.so.1`
in `DT_NEEDED`, which is the invariant CMakeLists.txt:78-83 warns about:

```
readelf -d build/asuna | grep NEEDED
 [libgtk4-layer-shell.so.0]
 [libgtk-4.so.1]
```

---

## 2. Supported build and run commands

From README "Build and install", verified:

```sh
tools/vendor.sh                 # ⚠ DOES NOT EXIST — see §6
tools/fetch_models.py --all     # models/asuna_NN/, 39 MB (already present: 42 outfits)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./install.sh                    # into ~/.local; --prefix, --link, --uninstall
```

Reproducibility was confirmed by configuring and building into a **clean,
separate directory** (not the existing `build/`):

```sh
cmake -S . -B /tmp/build-clean -DCMAKE_BUILD_TYPE=Release   # exit 0
cmake --build /tmp/build-clean -j                           # exit 0
ctest --test-dir /tmp/build-clean --output-on-failure       # exit 0, 4/4 passed
```

Configure emits **no warnings** on this machine. `glfw3` is found, so
`asuna-render-test` is built and the `message(WARNING ...)` at
CMakeLists.txt:138 does **not** fire.

The build emits exactly **one** compiler warning, and it is in vendored
third-party code, not ours:

```
third_party/live2d-v2/Common/stb_image.h:5166:56:
  warning: writing 1 byte into a region of size 0 [-Wstringop-overflow=]
```

Relevant to Phase 7: turning on stricter warnings needs this vendored path
excluded or suppressed, or the build stops being warning-clean through no
fault of `src/`.

---

## 3. Tests

```sh
ctest --test-dir build --output-on-failure     # 4 suites at Phase 0
```

Result: **4/4 passed**, 0.04 s total, in both the existing `build/` and the
clean build. Phase 1 adds `argparse` and `cli`, Phase 2 adds `state`, and
Phase 2.1 adds `json`.

| Suite | Binary sources | Self-reported result |
| --- | --- | --- |
| `motion` | `motion.cpp` | `all 15 tests passed` |
| `behaviour` | `behaviour.cpp`, `dialogue.cpp`, `json.cpp` | `all 17 tests passed` — was 20; the three JSON tests moved to `json` in Phase 2.1 |
| `ipc` | `ipc.cpp`, `json.cpp`, `paths.cpp` | `ipc: all checks passed` (9 groups) |
| `config` | `config.cpp`, `paths.cpp` | `all config tests passed` (6 groups) |
| `argparse` | `argparse.cpp` | `all 16 tests passed` — **added in Phase 1** |
| `cli` | the built `asuna` binary | `all 44 cli checks passed` — **added in Phase 1**, extended in Phase 2.1; no case reaches a real daemon, so it neither needs her running nor disturbs her if she is |
| `state` | `state.cpp`, `json.cpp`, `paths.cpp` | `all 14 state tests passed` — **added in Phase 2**; runs against a temporary `XDG_STATE_HOME` and refuses to start if that redirect did not take |
| `json` | `json.cpp` | `all 12 json tests passed` — **added in Phase 2.1**; the number grammar, the string rules and the quote/parse pair |

`behaviour` must run from the source tree — it reads `data/dialogue.zh.json`
(CMakeLists.txt:120-121).

**`asuna-render-test` is built but never registered with `add_test`.** It is
not part of `ctest` and is not covered by "all tests pass". It needs a real GPU
and is run by hand (README "Tests"):

```sh
./build/asuna-render-test --framing auto|--hit|--extent|--below|--lean
```

This is deliberate, not an oversight — but it does mean the automated safety
net for `pet.cpp` and `framing.cpp` is **zero** unless someone runs it
manually. Phases 4-5 touch neither, but it is worth knowing before Phase 8.

Python baseline:

```sh
python3 -Wall -m py_compile tools/*.py     # exit 0, no warnings
```

Note: this command *creates* `tools/__pycache__/`. There is no `.gitignore`
and no VCS to ignore it with (§6). It was removed again after this baseline
was taken.

---

## 4. Runtime behaviour snapshot

Captured against a **live daemon** (pid 1318261, started 15:13:34, up 5h56m)
using `./build/asuna`. All four commands are read-only: `status`, `model list`
and `ext status` query the control socket, `config check` only parses the file.

### `asuna status` → exit 0

```
asuna: running (pid 1318261), up 5h 56m
  outfit    03   /home/hhk/Projects/Asuna/models/asuna_03/index.json
  placed    x=1  scale 1.00  layer top  output eDP-2
  strip     1920x713  fps cap 60
  memory    171.4 MB resident
  config    /home/hhk/.config/asuna/config.toml
```

### `asuna model list` → exit 0

42 outfits in `/home/hhk/Projects/Asuna/models`; `03` marked as worn; 14
marked `+` (full body, taller strip). Ids are non-contiguous — `01-09`, then
`12-31`, `33-41`, `43-46`.

### `asuna config check` → exit 0

```
asuna: /home/hhk/.config/asuna/config.toml is fine
```

### `asuna ext status` → **exit 3**

```
asuna: helper not running
  extensions   enabled
  vision       not allowed
  log          /home/hhk/.local/state/asuna/ext.log
```

Exit codes (`src/app/cli.hpp:26-29`): `kOk=0`, `kError=1`, `kUsage=2`,
`kNotRunning=3`, plus `kAlreadyRunning` used by `ext start`.

### JSON forms

`--json` replies were captured for `status`, `model list`, `ext status` and
`ext config`. Representative:

```json
{"ok":true,"data":{"pid":1318261,"uptime_s":21439,"rss_kb":175476,"hidden":false,
"ready":true,"model":"03","model_path":"…/models/asuna_03/index.json","layer":"top",
"output":"eDP-2","x":0.908616,"scale":1,"fps":60,"strip_width":1920,
"strip_height":713,"asleep":false,"config":"…/config.toml"}}
```

**Recorded discrepancy — `--json` changes the exit code.**
`asuna ext status` exits `3` when the helper is not running, but
`asuna --json ext status` exits `0` for the same state. `cli.cpp:885` returns
`kOk` early in JSON mode and never reaches the
`return pid > 0 && subscribers > 0 ? kOk : kNotRunning;` at `cli.cpp:894`.
The JSON body cannot substitute: it carries `subscribers`, but **not** the
helper pid, which is read locally via `extPid()`. So a `--json` caller cannot
determine helper liveness from either the exit code or the payload.

This is recorded as existing behaviour, not fixed here. It belongs to Phase 3.

### Live daemon file layout

```
~/.config/asuna/config.toml                     0600, 5494 bytes
~/.local/state/asuna/state.json                 x, scale, model, layer, output, hidden
~/.local/state/asuna/asuna.log                  ~508 KB
~/.local/state/asuna/ext.log                    ~4 KB
/run/user/1000/asuna/asuna.lock                 held open by the daemon on fd 8
/run/user/1000/asuna/control.sock
```

Current `state.json`:

```json
{"x": 1, "scale": 1, "model": "…/models/asuna_03/index.json",
 "layer": "top", "output": "", "hidden": false}
```

Note `"output": ""` in state while `status` reports `output eDP-2` — the
running value is resolved at runtime, not persisted. Phase 2 must not
"fix" this into persisting `eDP-2`; it would change monitor-fallback
behaviour.

---

## 5. `improve.md` claims, checked against the tree

All five "Confirmed Issues" are **real**. Verified locations:

| # | Claim | Verdict | Evidence |
| --- | --- | --- | --- |
| 1 | `atoi`/`atof` accept malformed input | **Confirmed** | 14 call sites: `cli.cpp:230-242` (`--height`, `--max-height`, `--margin`, `--bottom`, `--pad`, `--band`, `--side`, `--gaze-halo`, `--scale`, `--x`, `--fps`), `:1054` (`say --for`), `:1088` (`move`), `:1093` (`scale`) |
| 2 | Empty stream is treated as success | **Confirmed** | `asuna-ext.py:285` `first = next(stream, None)` returns `None` without raising; `:290` clears the cooldown, `:306` returns — the next provider is never tried |
| 3 | State strings not JSON-escaped | **Confirmed** | `state.cpp:84-86` writes `model`, `layer`, `output` raw between quotes |
| 4 | Extension commands split on spaces | **Confirmed** | two independent tokenizers, `cli.cpp:666` and `cli.cpp:772`, both `find(' ', i)` |
| 5 | `kill(pid, 0)` cannot prove identity | **Confirmed** | `extPid()`, `cli.cpp:642-646` |

Line counts in `improve.md` match this tree exactly (`src` 40 files / 9,937
lines; `tools/*.py` 4 files / 1,452 lines).

---

## 6. Gaps and corrections found while taking the baseline

Recorded here so they are not mistaken for damage done by a later phase.

1. ~~**There is no version control.**~~ **Resolved during Phase 0.** A private
   repository was initialised by the maintainer: one `Initial commit` (`e2198be`),
   with `/build/`, `/models/`, `__pycache__`, `*.pyc`, `*.pyo` ignored.
   `third_party/` is **tracked on purpose** (108 files) — it carries local
   patches and is not reproducible by a fetch. Each phase is reviewed and
   committed by hand, so no phase should commit on its own.

2. ~~**`tools/vendor.sh` does not exist.**~~ **Resolved during Phase 0.** It
   never existed; the vendoring was done by hand and the README described it
   aspirationally. The README was corrected in three places: the "Requirements"
   note, the "Build and install" block, and the licensing consequences. Two
   related statements were wrong and are also fixed — the build no longer opens
   with an uninstallable step, and the claim that `third_party/live2d-v2/`
   "need not be committed" is replaced with what is actually true: it is
   committed deliberately, because `PATCHES.md` holds nine local patches that a
   plain re-fetch would silently revert.

3. **`improve.md`'s GLFW note does not apply here.** It says the CMake warning
   "only indicates that the optional render test is skipped". On this machine
   `glfw3` **is** found, the warning never fires, and `asuna-render-test` is
   built. The real gap is different: that binary is not wired into `ctest`.

4. **`improve.md`'s `tools/__pycache__` note is stale.** No `__pycache__`
   existed in the tree before this baseline; running `py_compile` creates one.
   The actionable item is a `.gitignore`, which requires (1).

5. **Issue #5 is a documented, deliberate trade-off, not an oversight.**
   `cli.cpp:636-641` already states the reasoning: the helper is the user's own
   program named by `ext.command`, so it "cannot be made to hold a lock for
   us", and a recycled pid "is a risk worth naming rather than pretending
   away". Any Phase 3 change must argue against that stated position rather
   than assume it was missed. Note also that `improve.md`'s `pidfd_open`
   suggestion does not work as written: `ext start` and `ext stop` are separate
   processes, so no pidfd can survive between them. Storing process start-time
   metadata alongside the pid is the approach that actually closes the hole.

6. **Issue #2 has a consequence `improve.md` does not mention.** An empty
   stream is not merely returned as success — `asuna-ext.py:299-305` also
   appends an **empty assistant turn to the conversation history**, so the
   damage outlives the one request.

7. **A CMake comment contradicts Phase 4.** CMakeLists.txt:110-111 says
   "`Config::applyTo` lives in `ui/shell.cpp` precisely so this can be a parser
   test rather than a GTK one" — the `config` suite links only `config.cpp` and
   `paths.cpp`. Phase 4 proposes moving `applyTo` into the config layer, which
   as stated would pull GTK into `config.cpp` and break that property — a
   property Phase 4's own exit criteria demand ("keep configuration tests
   independent of GTK"). A separate translation unit for `applyTo`, excluded
   from the test target, satisfies both.

---

## 7. Re-run procedure

To be run before starting and after finishing each phase:

```sh
# build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# tests — 4/4 at Phase 0, 6/6 from Phase 1, 7/7 from Phase 2, 8/8 from Phase 2.1
ctest --test-dir build --output-on-failure

# python — expect exit 0, then clean up
python3 -Wall -m py_compile tools/*.py && rm -rf tools/__pycache__

# behaviour — compare against §4
./build/asuna status
./build/asuna model list
./build/asuna config check
./build/asuna ext status ; echo "exit $?"    # expect 3 with no helper
```

The four `--json` captures are the machine-comparable form; they are the ones
to diff after Phases 1-3.

## 8. Exit criteria

| Criterion | Status |
| --- | --- |
| The current build is reproducible | **Met** — clean out-of-tree configure+build+test, exit 0 throughout |
| The existing test suite remains green | **Met** — 4/4, both trees |
| Behaviour changes attributable to a phase | **Met for behaviour** (§4 snapshots + JSON fixtures); **not met for source changes** until version control exists (§6.1) |
