# Asuna Improvement Review

## Scope

This is a read-only review of the current source tree and tool scripts. No
production source files were changed as part of this review.

The current codebase is already split into `src/app`, `src/character`,
`src/pet`, and `src/ui`. That high-level split is sensible. The main problem is
not the number of directories; it is that several files still combine too many
policies and workflows.

## Code Size

Measured physical line counts:

| Area | Files | Physical lines | Non-empty lines |
| --- | ---: | ---: | ---: |
| `src` | 40 | 9,937 | 8,892 |
| `tools/*.py` | 4 | 1,452 | 1,261 |
| Total | 44 | 11,389 | 10,153 |

Largest files:

| File | Lines | Main concern |
| --- | ---: | --- |
| `src/app/cli.cpp` | 1,111 | Argument parsing, lifecycle, config, extension, subscription, and autostart commands are combined. |
| `src/ui/shell.cpp` | 779 | Window construction, monitor handling, configuration, persistence, and lifecycle coordination. |
| `src/ui/shell_render.cpp` | 716 | GL callbacks, framing, placement, scaling, and input-region sampling. |
| `src/app/config.cpp` | 670 | TOML parsing, validation, defaults, and configuration mapping. |
| `src/pet/pet.cpp` | 514 | Model ownership, model metadata, fitting, coordinate transforms, and rendering. |
| `src/ui/bubble.cpp` | 507 | Speech state machine, queueing, layout, animation, and GTK integration. |
| `tools/asuna-ext.py` | 794 | Socket client, provider access, streaming chat, vision, prompt processes, and the helper loop. |
| `tools/asuna-prompt.py` | 432 | GTK prompt window, placement, input-region handling, and drag behavior. |

The project currently builds successfully and all four configured CTest tests pass.

## Confirmed Issues

### 1. Invalid CLI numbers are silently accepted

`src/app/cli.cpp` uses `atoi` and `atof` for command-line and option parsing.
This accepts malformed values as zero or partial values instead of reporting a
usage error. The same pattern is used for `--height`, `--fps`, `--x`, `--scale`,
`say --for`, `move`, and related options.

Examples of unsafe behavior include:

```text
asuna scale nope       -> may be sent as 0
asuna move .           -> may be sent as 0
asuna move 1.2.3       -> may be partially parsed
```

Recommendation:

- Add strict integer and floating-point parsing helpers.
- Reject trailing characters, `nan`, `inf`, missing values, and out-of-range values.
- Use the same helpers for CLI flags and subcommands.
- Add tests for malformed, boundary, and overflow inputs.

### 2. Provider failover stops on an empty successful stream

In `tools/asuna-ext.py`, `Chat.ask()` switches providers when opening or
reading a provider raises an exception. However, a provider that returns HTTP
success and immediately emits `[DONE]` produces no token and is treated as a
successful answer. The helper then returns without trying the next provider.

Recommendation:

- Treat a stream with no content as a failed attempt before the first token.
- Mark that provider as cooling down and continue to the next provider.
- Add a test for an empty stream followed by a healthy fallback provider.

### 3. State-file strings are not JSON-escaped

`src/app/state.cpp` writes `model`, `layer`, and `output` directly into JSON.
The reader also performs a minimal string scan and does not handle escapes.
Although common connector names and paths usually work, a quote or backslash
can produce invalid JSON or corrupt the restored value.

Recommendation:

- Reuse the project JSON writer for string escaping, or add a small dedicated
  JSON string-escape function.
- Parse the state file through the existing JSON reader instead of scanning text.
- Add round-trip tests containing quotes, backslashes, and Unicode paths.

### 4. Extension commands are split only on spaces

The C++ extension launcher splits `ext.command` by literal spaces in
`src/app/cli.cpp`, and the Python helper splits `prompt_command` with
`str.split()` in `tools/asuna-ext.py`. Quoted arguments and paths containing
spaces therefore do not work.

Recommendation:

- Define one command-line grammar for configured commands.
- Use `shlex.split` in Python.
- Implement an equivalent quoted-argument parser in C++, or restrict the
  configuration format explicitly to an argv array.
- Avoid `sh -c`; preserving direct `exec` behavior is safer.
- Add tests for quoted arguments, escaped spaces, and empty arguments.

### 5. Extension PID management has PID-reuse risk

`src/app/cli.cpp` stores a helper PID and checks it with `kill(pid, 0)`.
This cannot prove that the PID still belongs to the Asuna helper. If the helper
dies and the PID is reused, `ext stop` could signal an unrelated process.

Recommendation:

- Prefer a lock file held by the helper itself.
- On Linux, consider `pidfd_open`/`pidfd_send_signal` where available.
- At minimum, store and validate process-start metadata in addition to the PID,
  and report stale PID files distinctly.

## Python Warnings and Static Quality

### Intentional warnings

The `# noqa: E402` comments in `tools/asuna-prompt.py` are intentional. GTK is
imported only after the `LD_PRELOAD` setup and one-time re-exec, so removing
these comments without changing the import sequence would be incorrect.

The CMake warning about missing GLFW only indicates that the optional render
test is skipped. It is not a production runtime failure, but the build output
should make the optional nature clear.

### Warnings worth addressing

- `tools/asuna-ext.py` has a broad `except Exception` while decoding an HTTP
  error body. Narrow this to expected JSON, Unicode, and I/O failures.
- `tools/fetch_models.py` uses `json.load(open(...))` without a context manager.
  Use `with open(...)` to avoid resource warnings and make ownership explicit.
- The Python tools have no configured lint/type-check stage. Add a lightweight
  `ruff` configuration and run it in CI or a project check command.
- Keep generated `tools/__pycache__` files out of the source tree; they are
  test artifacts, not project files.

`python3 -Wall -m py_compile tools/*.py` produced no additional warnings in the
review environment, and no unused imports were found by an AST-based check.

## Recommended C++ Structure

Do not split every small class into its own directory. The current top-level
layering is already useful. Split the large policy files by workflow:

```text
src/app/cli/
  dispatch.cpp          command dispatch and global output mode
  options.cpp           strict option parsing
  lifecycle.cpp         start, exit, restart, status, and PID handling
  config_commands.cpp   config path/show/init/check/edit/reload
  extension_commands.cpp extension start/stop/test/status/subscribe
  autostart.cpp         desktop autostart integration

src/app/config/
  toml.cpp              low-level TOML reader
  config.cpp             application settings and defaults
  validation.cpp         ranges, cross-field checks, and unused-key checks
```

Additional targeted changes:

- Move `ShellOptions` into an application-level header such as
  `src/app/options.hpp`. `src/app/cli.hpp` currently includes `ui/shell.hpp`,
  pulling GTK and the whole Shell dependency graph into the CLI interface.
- Move `Config::applyTo` out of `src/ui/shell.cpp`; configuration mapping belongs
  in the app/config layer.
- Split `src/ui/shell_render.cpp` into GL callbacks and layout/input-region
  sampling if it continues to grow.
- Keep `motion`, `behaviour`, `framing`, `ipc`, `dialogue`, and `outfits` mostly
  as they are. Their responsibilities and test boundaries are already clear.
- Treat `shell.hpp` size as a design signal: the Shell object owns many states
  and callbacks. Further decomposition may eventually require coordinator
  objects, not just more `.cpp` files.

## Recommended Python Structure

`asuna-ext.py` and `asuna-prompt.py` are part of one chat feature, but the prompt
is also a standalone GTK executable. Put the implementation in a package while
retaining thin compatibility entry points at the current paths:

```text
tools/chat/
  __main__.py       helper entry point and argument parsing
  control.py        Unix socket request and subscription client
  provider.py       OpenAI-compatible HTTP provider
  conversation.py   streaming, history, cooldown, and failover
  vision.py         focused-window checks and screenshot masking
  prompt.py         GTK prompt window and drag/placement behavior
  helper.py         event loop, conversation orchestration, and shutdown
```

Keep `tools/asuna-ext.py` and `tools/asuna-prompt.py` as small wrappers so
existing config files, `install.sh`, and user commands continue to work.
Avoid placing provider/network code in the GTK prompt module.

## Redundancy and Maintainability

- The extension command tokenizer is duplicated in two C++ branches; centralize it.
- Numeric parsing is duplicated across several CLI paths; centralize it and make
  it strict.
- `Shell` configuration, rendering, input, extension consent, persistence, and
  lifecycle are split across files but still share one very large state object.
  File splitting helps navigation, but coordinator boundaries should be the
  longer-term goal.
- `Config::applyTo` crossing from `app` into `ui` is an architectural mismatch.
- The small JSON implementation is used for IPC and state-related data. Define
  clearly which code owns parsing and escaping instead of maintaining multiple
  ad hoc encoders/scanners.

## Verification Gaps

The existing tests cover motion, IPC, configuration parsing, and behavior. Add
focused tests for:

- Invalid and boundary CLI numeric arguments.
- State-file JSON escaping and round trips.
- Empty provider streams and provider failover.
- Quoted extension and prompt commands.
- Stale or reused extension PID files.
- Screenshot consent cancellation, timeout, and monitor hot-unplug behavior.
- Multi-monitor and fractional HiDPI placement.

## Implementation Roadmap

The work should be delivered in nine phases, numbered `0` through `8`. Each
phase has an independently verifiable outcome. The order is intentional:
behavioral correctness comes before structural refactoring, and interfaces
should be stabilized before large files are split.

### Phase 0: Establish a Baseline

Purpose: record the current behavior before making changes.

Tasks:

- Record the supported build and run commands.
- Confirm the existing CMake build and all configured CTest tests pass.
- Capture representative behavior for `status`, `model list`, `config check`,
  and `ext status`.
- Check for unrelated local changes before each implementation phase.
- Re-run the baseline checks after every phase.

Exit criteria:

- The current build is reproducible.
- The existing test suite remains green.
- Any behavior change in later phases can be attributed to a specific phase.

### Phase 1: CLI Input and Command Correctness

Purpose: remove silent acceptance of malformed user input.

Tasks:

- Replace `atoi` and `atof` with strict integer and floating-point parsers.
- Reject trailing characters, `nan`, `inf`, overflow, missing values, and invalid
  ranges.
- Apply the same parser to `--height`, `--fps`, `--x`, `--scale`, `move`,
  `say --for`, and related options.
- Centralize the duplicated C++ extension-command tokenizer.
- Define whether configured commands support quoting and escaping; implement
  that grammar consistently if they do.

Verification:

- Add tests for malformed input, boundaries, negative values, overflow, and
  missing arguments.
- Test quoted arguments, escaped spaces, and empty arguments.

Exit criteria:

- Invalid numeric arguments return a clear usage error.
- No malformed value is silently converted to zero or partially parsed.

### Phase 2: State and Configuration Correctness

Purpose: make persistence and configuration round trips reliable.

Tasks:

- Escape `model`, `layer`, and `output` when writing state JSON.
- Replace the ad hoc state-file scanner with the existing JSON parser.
- Review type, range, and cross-field validation in the configuration loader.
- Preserve the existing precedence rule: CLI flags, state, config, then defaults.

Verification:

- Add state round-trip tests for quotes, backslashes, Unicode, and unusual paths.
- Test malformed state files and confirm startup remains controlled and explicit.
- Run the existing configuration tests unchanged.

Exit criteria:

- Every supported string survives save/load unchanged.
- Invalid persistence data cannot silently corrupt the next startup.

### Phase 3: Extension and Chat Reliability

Purpose: make the out-of-process chat feature fail safely and recoverably.

Tasks:

- Treat a provider response with no content before the first token as a failed
  attempt and try the next provider.
- Distinguish connection errors, HTTP errors, malformed streams, cancellation,
  and empty responses.
- Narrow broad Python exception handlers.
- Use context managers for all file reads in the tools.
- Improve extension process identity handling beyond a bare PID plus `kill(pid, 0)`.
- Prefer a lock file or Linux `pidfd` where supported; otherwise validate stale
  PID metadata and report it clearly.

Verification:

- Test connection failure followed by provider success.
- Test an empty stream followed by provider success.
- Test all providers failing, helper crashes, stale PID files, and restart.
- Confirm the main daemon continues operating when the helper fails.

Exit criteria:

- Provider failover works before the first token, including empty responses.
- `ext stop` cannot normally signal an unrelated process.

### Phase 4: Restore C++ Layer Boundaries

Purpose: remove avoidable coupling before splitting implementation files.

Tasks:

- Move `ShellOptions` into an application-level header such as
  `src/app/options.hpp`.
- Remove the `cli.hpp -> ui/shell.hpp` dependency that pulls GTK into the CLI
  interface.
- Move `Config::applyTo` out of `src/ui/shell.cpp` and into the app/config layer.
- Review and document the intended dependency direction between app, UI, pet,
  and character code.
- Keep configuration tests independent of GTK.

Exit criteria:

- CLI headers expose application interfaces rather than the complete GTK Shell.
- Configuration mapping is owned by the configuration layer.
- The build and tests still pass without adding UI dependencies to app tests.

### Phase 5: Split Large C++ Translation Units

Purpose: improve navigation and maintainability after interfaces are stable.

Recommended structure:

```text
src/app/cli/
  dispatch.cpp
  options.cpp
  lifecycle.cpp
  config_commands.cpp
  extension_commands.cpp
  autostart.cpp

src/app/config/
  toml.cpp
  config.cpp
  validation.cpp
```

Further UI decomposition, only if the boundaries remain clear:

```text
src/ui/
  shell.cpp
  shell_gl.cpp
  shell_layout.cpp
  shell_region.cpp
  shell_input.cpp
  shell_speech.cpp
  shell_command.cpp
  shell_ext.cpp
  shell_debug.cpp
```

Tasks:

- Move code by responsibility, not by arbitrary line count.
- Keep `motion`, `behaviour`, `framing`, `ipc`, `dialogue`, and `outfits` intact
  unless a new independent responsibility appears.
- Update CMake source lists and include paths together with each move.
- Keep each new file focused, generally below roughly 300 to 600 lines where
  practical.

Exit criteria:

- Each large workflow has a clear owner.
- No behavior changes are introduced by file movement.
- The dependency graph is easier to understand than before the split.

### Phase 6: Modularize the Chat Tools

Purpose: split the 794-line helper and 432-line prompt implementation without
breaking existing entry points.

Recommended structure:

```text
tools/chat/
  __main__.py
  control.py
  provider.py
  conversation.py
  vision.py
  prompt.py
  helper.py
```

Responsibilities:

- `control.py`: Unix socket requests and event subscriptions.
- `provider.py`: OpenAI-compatible HTTP access and error formatting.
- `conversation.py`: streaming, history, cooldown, and failover.
- `vision.py`: focused-window checks, screenshot capture, and masking.
- `prompt.py`: GTK prompt window, placement, input region, and dragging.
- `helper.py`: event loop, conversation orchestration, and shutdown.
- `__main__.py`: command-line entry point.

Keep `tools/asuna-ext.py` and `tools/asuna-prompt.py` as thin compatibility
wrappers so existing configuration, `install.sh`, and user commands continue to
work. Provider/network code must remain separate from the GTK prompt module.

Exit criteria:

- Existing script paths and command behavior remain compatible.
- Chat components can be tested independently.
- GTK imports do not occur in provider-only tests.

### Phase 7: Static Checks and Development Automation

Purpose: prevent the reviewed problems from returning.

Tasks:

- Add a lightweight `ruff` configuration for the Python tools.
- Add type checking with `mypy` or `pyright` where the maintenance cost is
  justified.
- Enable useful C++ compiler warnings and keep warning suppressions documented.
- Add automated checks for CLI parsing, state persistence, provider streams,
  tokenization, and extension process handling.
- Keep generated bytecode, logs, and temporary files out of the source tree.
- Update the README with the new source and tool layout.

Recommended verification commands:

```text
cmake --build build
ctest --test-dir build --output-on-failure
python -m compileall tools
ruff check tools
```

Exit criteria:

- Static checks run consistently in local development and CI.
- Intentional suppressions are documented and narrow.
- New code cannot silently reintroduce the original parsing and resource issues.

### Phase 8: Desktop Integration Validation

Purpose: validate the behavior that unit tests cannot fully model.

Tasks:

- Test Wayland layer-shell startup, shutdown, hiding, and showing.
- Test monitor switching, hot-unplug fallback, and fractional/2x HiDPI.
- Test config reload and persistence precedence in a real session.
- Test prompt placement, dragging, and input-method behavior.
- Test screenshot consent, cancellation, timeout, masking, and deny-list logic.
- Test helper restart, daemon shutdown, provider outage, and recovery.

Exit criteria:

- The desktop pet remains usable through the supported lifecycle scenarios.
- No regression is found in layer-shell input regions, placement, or rendering.
- Remaining platform-specific limitations are documented explicitly.

## Phase Dependencies

```text
Phase 0: Baseline
    |
    +--> Phase 1: CLI correctness
    +--> Phase 2: State/config correctness
    +--> Phase 3: Extension reliability
              |
              v
       Phase 4: C++ layer boundaries
              |
              v
       Phase 5: C++ file decomposition
              |
              v
       Phase 6: Python chat modularization
              |
              v
       Phase 7: Static checks and automation
              |
              v
       Phase 8: Desktop integration validation
```

Phases 1 through 3 can be developed in parallel after Phase 0, but they should
be completed before the structural refactoring in Phases 4 through 6. Phase 7
should cover the new structure, and Phase 8 is the final acceptance pass.
