# asuna

A Live2D desktop pet for Wayland. She stands on the bottom edge of the screen, you drag
her along it, scroll to resize her, click her for a reaction, and she gets on with her own
business the rest of the time.

Native C++ on GTK4 + `wlr-layer-shell` + OpenGL ES, driving a Cubism 2.1 model through a
vendored port of the runtime. ~150 MB resident, ~7 % of one core while visible, nothing at
all while hidden. One binary, one socket, no daemon framework.

```
asuna start          # she appears, bottom-right
asuna hide           # she goes away without exiting
asuna say "在忙吗"    # she says it
asuna exit           # she says goodbye and leaves
```

**Status:** complete and in daily use. Everything below is implemented unless it says
otherwise.

**This repository must not be published as-is** — see [Licensing](#licensing), which is not
boilerplate.

---

## Contents

- [What she does](#what-she-does)
- [Requirements](#requirements)
- [Build and install](#build-and-install)
- [Command line](#command-line)
- [Configuration](#configuration)
- [Extensions](#extensions)
- [Outfits](#outfits)
- [Measurements](#measurements)
- [How it is built](#how-it-is-built)
- [Things that were not obvious](#things-that-were-not-obvious)
- [Tests](#tests)
- [Licensing](#licensing)
- [Known limits](#known-limits)
- [What is next](#what-is-next)

---

## What she does

**On her own.** Idle motions every 2.5–8 s from the model's `idle` group, rotated and never
cut short — the scheduler waits for the runtime's own "motion finished" rather than guessing
a duration. Occasional `REPEAT_0x` flourishes. Auto blink and breath. Physics-driven hair
and skirt sway. A look-around when nothing else is happening. An unprompted line every
90–300 s. Hourly greetings across 8 time bands and 6 seasonal dates. After 30 minutes
untouched she falls asleep, and waking her *is* the reaction.

**When you touch her.** The pointer is followed while it is on her, held for 1.2 s after it
leaves, then drifts back to idle. Clicking a part of her — head, chest, body, foot — gets
that part's own expression, motion and line:

| Hit area | Expression | Motion | Tone |
| --- | --- | --- | --- |
| `head` | any of this outfit's, at random | the one from the same family | whatever comes up |
| `chest` | `F_ANGRY` / `F_SURPRISE` | `I_ANGRY` | indignant |
| `body` | `F_FUN` | `I_FUN` | playful |
| `foot` | `F_SURPRISE` | `I_SURPRISE_S` | startled |

Her face is the exception to the fixed pairings, because it is the part anyone actually
looks at. A tap on it takes a face from the outfit's own list — never `F_NOMAL`, never
`F_SLEEP`, and never the one she is already wearing or the one the last tap chose — and then
the motion from the same family, which the asset set names to match: `F_FUN_SMILE` draws
from `I_FUN`, `I_FUN_S`, `I_FUN_W`. So the same two taps are never the same twice, and a
costume with an inventory this build has never heard of still pairs it correctly. The face
holds for `expression_hold` seconds and returns to `F_NOMAL`.

**Dragging.** Left-drag carries her along the bottom edge. She leans into her own velocity,
her hair follows, she is surprised when picked up and settles when put down. Scrolling over
her resizes her between 0.5× and 2.5×, re-solving the framing each time so she stays sharp
rather than being scaled as a bitmap — or between 0.5× and whatever the screen leaves room
for, which on a 1080p screen is usually the lower ceiling of the two. The wheel stops where
she does, so one notch back always shrinks her.

**Right-click** opens a menu: outfit, expression, motion, reset position, reset size, hide,
quit — and Chat…, when a helper is listening. `asuna menu` opens the same menu from a
compositor keybind.

Her lines live in `data/dialogue.zh.json`, keyed by trigger — `hit.head`, `drag.start`,
`drag.end`, `time.8-11`, `season.1224-1226`, `idle.chatter`, `sleep`, `wake`, `outfit`,
`greet.launch`, `farewell`, `hide`, `chat.open`, `glance` — each an array picked from at
random without immediate repeats. Missing file is not an error; she simply stops talking.

**If you turn them on**, [Extensions](#extensions) add a chat window and an occasional
remark about what is on screen. They are off, they run out of process, and she is complete
without them.

---

## Requirements

A Wayland compositor that supports `wlr-layer-shell`. Built and run on:

| | |
| --- | --- |
| Compositor | niri 26.04 |
| GTK | 4.22.4, `gtk4-layer-shell` 1.3.0 |
| GLib | 2.88.3 |
| GL | Mesa 26.1.6, AMD Radeon 780M (radeonsi), OpenGL ES 3.2 |
| Output | eDP-2, 1920×1080, scale 1 |
| Toolchain | GCC 16.1.1, CMake 4.3 |

GNOME/mutter has no layer-shell and will not work. KDE, sway, Hyprland and the wlroots
family should, but have not been tried.

Nothing above is needed for [Extensions](#extensions), and nothing there is needed for her:
they want `python3`, `python3-gobject`, `python3-cairo` and `gtk4-layer-shell` (for the prompt
window — cairo for the one region it hands the compositor), plus
`grim` and either `python3-pillow` or ImageMagick if you turn on screen capture. She runs
identically with none of it installed.

The model assets are not in this repository. `tools/fetch_models.py --all` downloads all 42
outfits (39 MB) from the source CDN. The Live2D runtime is the other way round: it *is* in
the repository, under `third_party/live2d-v2/`, vendored by hand and carrying nine local
patches — so there is nothing to fetch and no script to run. What was changed, and why, is
in `third_party/live2d-v2/PATCHES.md`.

---

## Build and install

```sh
tools/fetch_models.py --all     # models/asuna_NN/, 39 MB
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./install.sh                    # into ~/.local; --prefix, --link, --uninstall
```

`third_party/live2d-v2/` is committed, so there is no vendoring step: the runtime is
already pruned to `Common/`, `Glad/` and `V2/`, and already patched. Re-vendoring from
upstream is a manual job — pin `PINNED-COMMIT`, copy those three directories, then re-apply
every patch in `PATCHES.md`. A plain re-fetch silently reverts nine fixes, two of which are
visible rendering bugs.

`install.sh` puts the binary in `<prefix>/bin` and the data in `<prefix>/share/asuna/`.
That layout is not arbitrary: both the model search and the dialogue search walk up from
`/proc/self/exe`, so `<prefix>/bin/asuna` finds `<prefix>/share/asuna/models` with nothing
configured. That is what makes an autostarted daemon — whose working directory is your home
— find its own model.

```sh
asuna autostart enable          # ~/.config/autostart/asuna.desktop
```

niri does not run XDG autostart, so `autostart enable` also prints the line to paste into
`~/.config/niri/config.kdl`:

```kdl
spawn-at-startup "/home/you/.local/bin/asuna" "start" "--foreground"
```

---

## Command line

Every verb except the two that run her is a client: it opens the control socket, sends one
line of JSON, prints what came back, and exits. `--json` on anything prints the raw reply
instead of the summary.

### Lifecycle

| | |
| --- | --- |
| `asuna start [options]` | Detaches and returns in ~170 ms. Refuses if one is already running. `--foreground` stays in the terminal. |
| `asuna exit` | Graceful over the socket, then `SIGTERM` (5 s), then `SIGKILL` (2 s). Aliases `stop`, `quit`. |
| `asuna restart` | Not an error if she was not running. |
| `asuna status` | pid, uptime, outfit, position, scale, layer, output, strip, RSS, config file. |
| `asuna ping` | Is the socket answering, and whose pid. |

### On screen

| | |
| --- | --- |
| `asuna hide` / `show` / `toggle` | Put her away without exiting. |
| `asuna move <px\|left\|centre\|right>` | Glides rather than teleports. |
| `asuna scale <n>` | Clamped to 0.5–2.5, and to whatever the screen leaves room for — often the tighter of the two, so a number past the end is not an error, it lands at the ceiling. The `--scale` startup flag is the stricter one: it refuses anything outside 0.5–2.5 rather than starting her at a size you did not ask for. |
| `asuna layer <top\|bottom\|overlay\|background>` | |
| `asuna output [list\|<name>]` | Which monitor she lives on. |
| `asuna menu [open\|close\|toggle]` | For a compositor keybind. |

### Her

| | |
| --- | --- |
| `asuna say <text> [--for S]` | A line in the speech bubble. |
| `asuna say <text> --hold\|--append` | Start a line that stays up, then grow it. `--release` lets it time out; `--clear` takes it down now. |
| `asuna think [on\|off]` | An animated ellipsis, held, for the wait in front of a slow answer. |
| `asuna motion [name\|--list]` | `--list` enumerates the *model's own* inventory. |
| `asuna expression [name\|--list]` | An unknown name is refused by name, not ignored. |
| `asuna model list` | The outfit registry. |
| `asuna model use <id>` | `asuna model use 31`. |

### Extensions

A chat window, and — if you let it — the occasional remark about what is on screen. Both
are driven by an OpenAI-compatible endpoint, and both are **off** until you turn them on.

```toml
[ext]
enabled   = true
providers = ["openai"]

[ext.provider.openai]
base_url    = "https://api.openai.com/v1"
model       = "gpt-4o-mini"
api_key_env = "OPENAI_API_KEY"
```

```console
$ export OPENAI_API_KEY=sk-...        # in the session that starts her
$ asuna ext test                      # before starting anything
provider     model                  key                result
openai       gpt-4o-mini            $OPENAI_API_KEY    ok, 412 ms
$ asuna ext start
asuna: helper running (pid 21714), logging to ~/.local/state/asuna/ext.log
$ asuna chat                          # or the Chat… entry in her right-click menu
```

`asuna chat` opens a one-line prompt above her head and her answer is streamed into the
speech bubble as it arrives. To keep going, **poke her** within 90 seconds and the prompt
comes back with the conversation still in her head; the prompt never opens by itself, because
it takes the keyboard with it (see [The prompt window](#the-prompt-window)). `Escape` or a
right-click closes it. `asuna chat "何时休息"` skips the prompt, which is the form to put on a
compositor keybind.

### More than one provider

Define as many as you like. She tries them in order and moves on to the next one that
answers, so a local model can back up a paid one or the other way round:

```toml
[ext]
providers = ["local", "openai"]     # without this line, the order they appear below

[ext.provider.local]
base_url = "http://127.0.0.1:11434/v1"
model    = "qwen2.5:7b"             # no key at all, for something on this machine

[ext.provider.openai]
base_url    = "https://api.openai.com/v1"
model       = "gpt-4o-mini"
api_key_env = "OPENAI_API_KEY"
```

Failing over happens **before the first token and never after**: once a piece of an answer
is on screen, moving to another provider would write a second answer on top of the first.
A provider that fails is skipped for two minutes rather than retried on every message, so
one that is down does not slow down every question you ask.

A provider defined but left out of `providers` is parked, not ignored in silence —
`asuna ext config` says it is there and unused. `asuna ext test` probes each one with a
one-token request and reports what came back, which is the difference between "the key is
wrong", "the model name is wrong" and "nothing is listening on that port".

### Where the key goes

`api_key_env` names an **environment variable** that the helper reads out of its own
environment, so the key is never in the config file, never in the daemon and never on the
socket. `api_key = "sk-…"` in the provider section also works and is strictly worse;
`asuna config init` writes the file `0600` because of it, and `asuna config check` says so
if that has since been loosened. `asuna ext config` never prints a key and neither does its
`--json` form.

Because the key is read from the *helper's* environment, `asuna ext start` has to be run
somewhere that has it — the same place `asuna ext test` would work from.

### Her voice, in the config file

The system prompt lives in `config.toml` next to her idle timings, so tuning what she says
and tuning how often she says it are the same job in the same place:

```toml
[ext]
temperature = 0.8
max_tokens  = 220        # a speech bubble, not an essay
bubble_rows = 8          # and this is how big the bubble is

[ext.prompt]
persona = """
你是结城明日奈（亚丝娜），住在用户桌面上的小小陪伴角色。
回答要短——最多两三句话，不要列表，不要 markdown。
"""
glance = """…"""         # what she is asked when she looks at the screen
```

Triple-quoted strings run to the closing `"""` and are taken literally — a `#` inside one is
a `#`, not a comment. The newline after the opening delimiter and the one before the closing
delimiter are dropped, so the text is what you see between them.

### It runs out of process

The helper is `tools/asuna-ext.py`, a separate program that drives her through the same
control socket `asuna say` uses. Nothing about a language model exists inside the daemon:
no HTTP client, no key, no prompt, no picture.

- **Her main loop is her render loop.** A TLS handshake or a slow first token inside it is a
  visible freeze, and a pet that freezes looks broken rather than thoughtful.
- **The key lives in one small file you can read.** In the daemon it would share an address
  space with a GL context, a Live2D runtime and a socket anything on the machine can
  connect to.
- **It can be killed, edited and restarted** without her reloading 113 MB of model, so
  prompts are cheap to iterate on. If it dies she carries on exactly as she does with it
  switched off; nothing here is load-bearing for the pet.

The five things the daemon grew to make this feel native rather than bolted on:
`say --hold/--append/--release` so a reply can be written as it arrives; a bubble that grows
to `bubble_rows` rows — and a strip that reserves the height for them — before it scrolls;
`subscribe`, so the helper reacts instead of polls;
her own chatter suppressed while something is subscribed, because two voices on one
character is worse than either alone; and `think`, the waiting tell, so a slow endpoint
reads as thought.

### The prompt window

`tools/asuna-prompt.py` — a one-line entry on a layer surface, above her head, styled like
her bubble. It exists because **fuzzel has no input-method support at all**: it never binds
`zwp_text_input_v3`, so `Ctrl+Space` cannot reach fcitx5 and there is no way to type Chinese
into it. A GTK entry gets IM through the toolkit, which is the whole difference — opening
this one makes fcitx5 create an input context on the session bus, which is what a text field
is supposed to do.

It closes on `Escape` or on a right-click anywhere in it, and puts itself away after five
minutes if it is never answered. It has no context menu: GTK's own cut/copy/paste popover is
a themed slab arriving inside a surface with no theme, and a one-line prompt has nothing in
it worth a menu.

**It opens just above her hair**, centred on her. The daemon answers `ext status` with the
anchor, because it is the only process that knows where she is — and it measures it off her
*outline* rather than off her strip. The strip is not her: above her box it also carries the
lift headroom and the whole bubble band, which is sized for `ext.bubble_rows` of reply rather
than for anything that is on screen now. Anchored to that, the prompt stood a third of the
way up the screen, over whatever window was underneath, and climbed further with every scroll
up.

**Scrolled up past halfway it moves to her side** instead, level with her face, on whichever
side has more room. Above her head is only a good place while there is screen above her head;
once the top of it is more than half way up, a prompt on top of that is a prompt near the
ceiling. Her head rather than her size is the test, because her head is the thing that
actually runs out — a short screen reaches it sooner and a tall one later, and neither needs
a number of its own. She is far taller than she is wide, so sideways is where the space is by
then. The daemon sends `anchor_place`; the prompt hangs the matching part of itself on the
point, since it is the only one that knows how big it is.

**Drag the label to move it** — the `asuna` on the left is a handle, and carries the window
with it. On the label and nowhere else, because that is the one part of the prompt text never
reaches: anything overlapping the entry has to be told apart from putting the caret somewhere
and from sweeping a selection, and the only ways to do that are a hold before the drag starts
(a wait, on every move) or a race with GtkText over which gesture claims the press. A
separate widget has neither problem, so the drag begins on the press. It cannot be dragged
off the screen. The window it carries is the whole screen and always was — the prompt is a
child inside it, so dragging moves nothing the pointer is measured against, and the surface's
input region is cut to the prompt so the rest of it is not there.

**It only ever opens because you just asked for it.** `asuna chat`, the Chat entry in her
menu, or — once she has answered something — a poke, for the next 90 seconds, with the
conversation still in her head. It does not reappear by itself after an answer, because it
*cannot* appear politely: a layer surface with `on_demand` keyboard interactivity is given
focus the moment it maps (tested on niri, and setting the mode after the fact is ignored),
so a prompt that opens uninvited takes the sentence you were typing somewhere else with it.

**Typing Chinese into it** needs one fcitx5 setting, because a fresh input context starts in
fcitx5's default state rather than the one you are already using:

```
[Behavior]
ShareInputState=All      # ~/.config/fcitx5/config; No | Program | All
```

`prompt_command` under `[ext]` replaces the window with anything that prints a line and exits
0 (`"fuzzel --dmenu --prompt-only=asuna> "` for the Latin-only version).

### Memory, on purpose

She remembers inside a conversation and nothing between them. Closing the prompt drops the
history; `history_turns` caps it while it lasts. There is no transcript on disk — the log
records what the helper *did*, not what was said.

### Looking at the screen

`[ext.vision] enabled` is a separate switch, and it is the one to think about before
setting. It uses **the same providers as the chat** — an endpoint that can see a picture is
a property of the model, not a second thing to configure.

```toml
[ext.vision]
enabled  = true
interval = [900, 2700]              # random, so it is never on a metronome
deny     = ["org.keepassxc.KeePassXC", "signal"]
```

Every glance goes through four gates, and all four have to agree:

1. **The helper** checks `enabled` in the settings she gave it.
2. **The focused window** is checked against `deny` — matched as a case-insensitive
   substring, so `"firefox"` covers `org.mozilla.firefox`. niri only; on other compositors
   the app-id cannot be read, so the list matches nothing, which is why it is not the only
   gate.
3. **She looks up and says so**, and for `notice` seconds you can call it off by poking her.
4. **She checks `enabled` again herself** and refuses to hand over the geometry otherwise.
   This is the one that matters: it is the only check that does not live in the process
   holding the API key.

Then the helper takes one `grim` screenshot of her output, **paints out the rectangle she
and her speech bubble occupy** — otherwise her own last sentence comes back to her as "what
is on screen" — scales it down, and sends it. Each granted capture is logged by the daemon,
in the log you already read:

```
asuna: screen capture granted on eDP-2, masking 488x580 at (993,500)
```

The picture is sent once and never kept: not on disk, and not in the conversation history.

`asuna ext cancel` stops a reply on its way or a glance that has been announced. So does
poking her.

### The event stream

`subscribe` is the one connection that is not a request and a reply: it stays open and
receives one JSON object per line. `asuna subscribe` prints them, which is the fastest way
to see what a helper would see — bearing in mind that it *is* a subscriber, so her own
chatter stops while it is running.

```console
$ asuna subscribe
{"ok":true,"data":{"pid":14822,"subscribers":1}}
{"event":"touch","area":"head"}
{"event":"drag","phase":"begin"}
{"event":"outfit","id":"03"}
{"event":"visible","hidden":true}
{"event":"sleep","asleep":true}
{"event":"chat","text":"何时休息"}
{"event":"config"}
{"event":"bye"}
```

### Writing your own

The bundled helper is one option, not the interface. `command` under `[ext]` runs anything;
what a helper actually has to do is subscribe, react to `chat`, and write into her bubble
with `say --hold` / `--append` / `--release`. Roughly:

```python
say = lambda **a: call({"cmd": "say", "args": a})
call({"cmd": "think", "args": {"on": True}})
say(text=first_chunk, hold=True)
for chunk in rest:
    say(text=chunk, append=True)
say(release=True)
```

Two rules for a subscriber: keep the write end of the socket open (a half-close is how the
daemon is told you have gone), and open a separate connection for commands, so replies never
interleave with events. If your helper dies mid-answer she notices after 45 seconds and ends
the line herself, rather than leaving a bubble up with no countdown.

---

## Outfits

42 of them, scanned from `models/asuna_<id>/index.json` at startup. Dropping a folder into
`models/` is all it takes to add one — which is also what makes the 42-outfit sweep in
`tests/` a real regression test rather than a fixture.

```
$ asuna model list
asuna: 42 outfits in models
   01    02    03    04    05   ·06    07    08    09    12    13    14
   15    16    17    18    19    20    21    22    23    24    25    26
   27    28    29    30    31+   33+   34+   35+   36+   37+   38+   39+
   40+   41+   43+   44+   45+   46+
  · wearing now   + full body (14 of them; taller strip)
```

The `+` matters. 28 outfits declare `layout {width: 2.9, y: 1.3}` in their `index.json`,
which is a bust crop; the other 14 declare no layout at all, meaning the whole canvas.
Switching to one of those re-solves the framing and asks the compositor for a taller strip,
so it changes the shape of the whole thing rather than just the picture.

The model itself carries 10 expressions, 19 motions in two groups (`""` with 16, `idle` with
3) and 4 hit areas.

One of the ten is corrected in code. `F_SLEEP` closes her eyes but leaves
`PARAM_MOUTH_FORM` at 1 — the same smiling mouth as her resting face — so what it draws is
someone smiling with their eyes shut, not someone asleep. `kPatches` in [pet.cpp](src/pet/pet.cpp)
overrides the mouth to a relaxed, faintly open one, re-asserted every frame *after* the
runtime has run the motion, the blink and the expression over it. That window — between
`update()` and `draw()` — is where the runtime documents parameter overrides, because it
runs the deformer chain in `draw()`. The values were rendered and looked at rather than
guessed: `0` is a flat line, `-0.5` reads as unhappy, `-1` is a frown. The model files
themselves are untouched, which matters because `models/` is reproducible from
`tools/fetch_models.py` and any edit there would be overwritten by the next fetch.

---

## Measurements

All on the machine in [Requirements](#requirements), from `/proc/<pid>/stat` over 20 s and
`/proc/<pid>/statm`.

| | |
| --- | --- |
| Resident, visible | **150 MB** |
| Resident, started hidden (no model, no GL context) | **37 MB** |
| CPU, visible and idle, fps cap 30 | **7.0 %** of one core |
| CPU, hidden | **0.1 %** — i.e. the measurement floor; 0 scheduler ticks over 20 s |
| `asuna start` to the socket answering | **170 ms** |
| First frame on screen | ~1.9 s (model load + the compositor's first configure) |

CPU scales with the frame cap almost linearly, which makes `strip.fps` the lever if 7 % is
too much:

| fps cap | 10 | 15 | 20 | 30 | 60 |
| --- | --- | --- | --- | --- | --- |
| % of one core | 3.6 | 4.0 | 5.7 | 6.7 | 12.3 |

Two things worth reading off these numbers. The 37 MB says ~113 MB of the 150 is the model
and the renderer, not the toolkit — so GTK is not where the memory is. And hidden being
free took *two* changes, not one: unmapping the surface alone left her at 0.9 % of a core,
because GTK keeps the frame clock running for an off-screen window. Removing the tick
callback as well is what makes it nothing.

---

## How it is built

A full-width, bottom-anchored, transparent `wlr-layer-shell` strip with the pet drawn into
it through a `GtkGLArea`. Everything outside her input region is click-through, so the strip
does not steal input from the windows underneath.

Her box lives in **device** pixels, because that is what `glViewport` takes. GTK hands out
logical pixels, so everything crossing that boundary — the pointer, the input region, the
size request — goes through `mScaleFactor`.

`src/` is laid out by concern. `json` and `paths` sit at the root because they belong to no
layer — all four directories use them.

```
src/
  main.cpp             dispatch: hands the daemon half to app/cli.cpp
  json.{cpp,hpp}       a small read-only JSON reader
  paths.{cpp,hpp}      XDG dirs; find models/ and data/ without a cwd

  app/                 the process and its interfaces
    cli.{cpp,hpp}      every verb; the client half never touches GTK
    daemon.{cpp,hpp}   flock, log redirect + rotation, RSS, the log filter
    ipc.{cpp,hpp}      line-JSON socket server and client, and the writer
    config.{cpp,hpp}   the TOML subset reader and the settings
    state.{cpp,hpp}    state.json

  pet/                 the figure, and how she is placed and moved
    pet.{cpp,hpp}      LAppModel ownership, box placement, outfit swap
    framing.{cpp,hpp}  measure the model, solve box/scale/offset per crop
    motion.{cpp,hpp}   drag spring, lift/fall/landing, squash (no GL, no GTK)
    outfits.{cpp,hpp}  scan models/, resolve bare ids

  character/           what she does when nobody is touching her
    behaviour.{cpp,hpp} idle scheduler, chatter, sleep/wake, gaze (no GL, no GTK)
    dialogue.{cpp,hpp} dialogue.zh.json: no-repeat picking, time and season bands

  ui/                  everything that touches GTK
    shell.hpp          the one class the seven shell*.cpp files make up
    shell.cpp          window, layer-shell, monitor, layer, config, state, loop
    shell_render.cpp   GL callbacks, framing, placement, the input region
    shell_input.cpp    drag, tap, gaze, halo, wheel, right-click
    shell_speech.cpp   the Behaviour hooks, and where the bubble goes
    shell_command.cpp  what each `asuna <verb>` does on arrival
    shell_ext.cpp      the events an extension subscribes to, and the consent
                       in front of a screen capture - no API client anywhere
    shell_debug.cpp    the ASUNA_DEBUG_* hooks - synthetic input, since a
                       compositor will not give a test a real pointer
    bubble.{cpp,hpp}   the speech bubble, including the held streaming line
    menu.{cpp,hpp}     GtkPopoverMenu, built on first open
tools/asuna-ext.py       the extension helper - runs out of process, holds the
                         API key, and is the only file here that talks to a network
tools/asuna-prompt.py    the one line you type at her: a GTK entry on a layer
                         surface, because it is the only kind that takes an IME
third_party/live2d-v2/   vendored Common/ Glad/ V2/ — never V3/
```

`pet/motion` and all of `character/` deliberately know nothing about GL or GTK. That is what
makes the drag feel and the whole personality testable on a machine with no pointer and no
GPU, which this one effectively is.

### The control protocol

One JSON object per line, both directions, on an `AF_UNIX` `SOCK_STREAM` socket:
`{"cmd":"…","args":{…}}` in, `{"ok":true,"data":{…}}` or `{"ok":false,"error":"…"}` out,
then the connection closes. No session, no state on the wire — which is what lets
`asuna status` be a shell script's business rather than a protocol implementation.

Line-delimited JSON rather than D-Bus because the daemon is single-user, single-instance and
local: a D-Bus name would buy discovery and activation we do not want (a pet that respawns
when you poke it is a bug) and cost a dependency and a schema.

`subscribe` is the one exception to "no state on the wire": that connection stays open and
receives `{"event":"…",…}` lines until one end closes it. It is read-only from then on — a
subscriber that wants to ask her something opens an ordinary connection for it, so replies
never interleave with events — and it must keep its own write end open, since a half-close
is how the daemon is told the subscriber has gone. See [Extensions](#extensions).

---

## Things that were not obvious

The parts that took a second attempt, kept because each of them is a trap that is easy to
walk back into.

**The runtime is a port of a port, and its failures are silent.** `easylive2d-cpp` V2 is a
hand-port of a Python port of a deobfuscated minified JS SDK, and this model was never its
test case. Two things were wrong and neither announced itself: the texture upload
(`third_party/live2d-v2/PATCHES.md` §4) and the physics, which never ran at all (§7). The
model still rendered and still animated in both cases. Every subsystem therefore needs one
test that would *fail* if it were doing nothing — `asuna-render-test --lean` traces the
physics chain end to end for exactly that reason. Assume the untested paths (pose groups,
expression fades, motion blending) are broken until something proves otherwise.

**Never key anything off a marker's name.** `asuna_27`, `_28` and `_29` ship
`D_REF.PT_BODY` and `D_REF.PT_FOOT` swapped; `asuna_46` has no `foot` area at all. The
framing derives the waist geometrically and `Pet::normaliseHitAreas()` does the same for tap
reactions, logging when it corrects one. The third case was found only because the test
sweeps all 42 outfits and sorts the areas by height.

**A bounding rectangle is a 300×480 click dead zone.** The input region is sampled from the
alpha that was just rendered, binned into 8 px cells and merged into row runs — about 56
rectangles covering 51 % of her box, exact by construction for any outfit. Hit-testing the
model's polygons instead would have reproduced the bounding box, because it includes the
invisible `D_REF.*` markers.

**A silhouette is a shape sampled at a position.** The right-click menu used to open where
she *used* to be. The first fix — drop the sampled region whenever her geometry changes —
was only half of it. The real fix is to stamp the shape with the position it was sampled at
and carry it to wherever she is now, which removes the dependency on freshness entirely.

**Margins are part of a size request.** Positioning the speech bubble by setting margins
asks GTK to renegotiate the toplevel size, and a layer surface anchored to both side edges
has no width to offer — which is where ~300 `gdk_wayland_toplevel_compute_size` warnings per
drag came from. `GtkOverlay::get-child-position` hands back an allocation without asking
anyone to resize. The first diagnosis (caching the bubble's measurement) was wrong and
changed nothing; isolating the cost by action category — move +0, say +2, say-and-move +98 —
is what caught it.

**The viewport is a hard clip, and the box is solved around her resting pose.** Anything a
motion reaches outside it is lost — hands and elbows, on the angry motions. `side_bleed`
widens the rendered viewport symmetrically without touching the box, so framing, position
and apparent size are unchanged. 0.30 clears the worst pose across all 42 outfits (25 %) and
the worst motion they actually play (21 %), both measured with
`asuna-render-test --extent`.

**There are no feet.** Every one of the 14 full-body outfits runs past the bottom of its own
canvas, so the crop lands mid-calf and the artwork simply stops at a flat horizontal cut of
the artist's own. Lifting her turns that cut into a blunt slice hanging in mid-air. The fix
is to render 48–135 px of leg *below* the screen edge so the lift reveals more of her
instead, and the cut travels down off the edge with the body it belongs to.

**"Is one running" is answered by taking the lock, not by validating a pid.** An `flock` is
released by the kernel however the process dies. The lock *file* is deliberately never
unlinked: a departing daemon deleting a file an incoming one had already locked would put
the next start on a different inode, which is two pets.

**Ready means the socket answers, not that she is on screen.** The first frame is ~2 s away
behind a model load and the compositor's first configure. Making `asuna start` wait for it
would be making the shell wait for an animation; commands that need her say so until then.

**Link order is load-bearing.** `gtk4-layer-shell` works by interposing on
`libwayland-client`'s global registry, so it must appear before GTK4 on the link line. Get
it wrong and `gtk_layer_init_for_window()` silently does nothing and the window comes up as
an ordinary toplevel. `readelf -d build/asuna | grep NEEDED` is the check.

**GTK 4.22 defaults to the Vulkan renderer**, which costs ~22 % of a core to recomposite
this full-width surface at 30 fps. The GL renderer does the same job for ~5 %. `GSK_RENDERER`
is set but not overwritten, so the environment still wins.

**A widget's width is not its text's width.** The streaming bubble scrolls by wrapping the
full text into a Pango layout, counting the lines and dropping whatever will not fit — and
it was wrapping to the width `gtk_widget_measure` reported, which includes the CSS padding.
Pango therefore counted four rows where GTK drew five, and a `bubble_rows = 4` bubble came
out five rows tall. The padding is now measured once on the empty label and taken off.
Caught by counting rows in a screenshot; nothing about the code looked wrong.

**Nine `std::function`s in a row is a structure you cannot read.** `Menu::Actions` is
outfit, expression, motion, reset-position, reset-size, chat, hide, quit, visibility — all
but three of them `void()` — and it was filled in as a brace list. Adding Chat to the middle
of the struct silently shifted every handler after it: Chat resized her, Reset position
opened the chat, Reset size moved her. The compiler has nothing to say about it and a
screenshot of the menu looks perfect, because what a screenshot cannot show is what an item
*does*. Now assigned by name, and `ASUNA_DEBUG_ACTION=<entry>` activates one through the
same action group a click does — which is how the fix was checked.

**fuzzel cannot take an input method.** Not "needs configuring" — `strings` on the binary
finds no `text_input` at all, so it never binds `zwp_text_input_v3` and `Ctrl+Space` has
nowhere to go. Anything that wants CJK input has to be a toolkit client; her prompt is a
GTK4 entry, and opening it makes fcitx5 create an input context on the session bus, which is
the check that proves it rather than assuming it.

**Link order is load-bearing in Python too.** `gtk4-layer-shell` interposes on
libwayland-client, so it must be loaded first — but importing `gi` loads GTK and libwayland
with it, and a Python process cannot control its link order. The prompt re-execs itself once
with `LD_PRELOAD` set before it imports anything. Without it the window comes up as an
ordinary toplevel and every layer-shell call warns; with it, nothing.

**`_exit()` does not flush a `std::ofstream`.** The middle process of `ext start`'s double
fork writes the helper's pid file and then `_exit(0)`s, which runs no destructors - so the
file was created and left at zero bytes. `asuna ext status` could see the helper on the
socket but not in the process table. A scope around the stream fixes it; the lesson is that
`_exit` after a fork is a different exit from the one the rest of the program uses.

**A prompt is a place a signal cannot reach.** `subprocess.run` waits with `waitpid`, which
CPython retries across a signal, so a helper sitting at an open chat prompt ignored SIGTERM
until somebody typed something — `asuna ext stop` needed a `SIGKILL` every time. The handler
now terminates the prompt it is holding. Any blocking call in a program that has to die on
request needs the same question asked of it.

**A layer surface takes the keyboard when it maps, not when you touch it.** `on_demand` reads
like "focus me on interaction", and the protocol says as much, but a surface mapped with it
is given focus straight away — and setting the mode *after* the map is sent on the wire and
then ignored (`set_keyboard_interactivity(2)` goes out, no `keyboard.enter` follows). So
there is no such thing as a text box that appears quietly and waits to be clicked. That is
why the follow-up prompt was deleted rather than made polite: the only prompt that cannot
interrupt you is one that opens because you just asked for it.

**A surface you drag by the pointer inside it is a control loop, and you do not get to pick
its latency.** There is no window position to read and no compositor-side move to ask for on a
layer surface, so the obvious way to drag one is arithmetic on its margins — and the pointer
arrives in *surface* coordinates, so every pixel the surface goes right is reported back as
the pointer going a pixel left. That reads like a gift: the offset is self-correcting, so ask
for the move it describes and the window is where the hand is. It is really a feedback loop
closed around the compositor, and its stability depends entirely on how many frames pass
between asking for a margin and being told about it in pointer coordinates. One frame is
deadbeat — perfect, in a single step. Two is `xₙ = xₙ₋₁ − xₙ₋₂ + u`, whose roots sit exactly
on the unit circle: it oscillates and never stops. Nothing in the protocol says which you get,
and pacing the asks on the frame clock only guarantees the *asking* rate, not the answer's.
Damping it costs lag proportional to the dead time, which at a mouse's speed is worse than
the wobble.

So there is no loop: the surface is the whole output, anchored to all four edges, and the
prompt is a child moved inside it with `Gtk.Fixed`. Nothing the drag does can move the frame
the *pointer* is measured in, and placement is a layout change with no round trip in it. The
bill for that is that a screen-sized surface takes every click on the desktop, paid with
`gdk_surface_set_input_region()` cut to the prompt's own rectangle — which is what the pet has
always done to be a full-width strip you can click through, and she moves inside hers for the
same reason.

**And the frame the *gesture* is measured in is a different frame again.** A `GtkGesture`
reports its points in the coordinates of the widget its controller is on, and the handle rides
along with the thing being dragged — so a still surface is only half the answer, and the
offsets handed to `drag-update` still have every move already subtracted out of them. Same
loop, moved indoors, with the layout pass for its latency instead of the compositor: `xₙ =
2g + uₙ − xₙ₋₁`, which settles at *half the hand's speed* and steps in jerks of a whole frame's
travel, so the prompt trails the cursor and strobes into looking like two of itself. The fix
is to stop using the offsets: take the gesture's raw point and put it back through
`gtk_widget_compute_point()` into the window's coordinates, which are the screen's. The same
allocation goes in both directions, so it cancels exactly whether or not the last move has
been laid out yet.

The harness that missed this is worth more than the bug. It fed absolute offsets straight into
`drag-update`, which is the one thing a real gesture never does, and so it passed both the
broken code and the fixed code. Its replacement models a hand as a position on the *screen*
and derives every number GTK would derive, one step per frame — because the handle's origin
only changes when the window is laid out again, so a harness that steps several times inside
one callback sees a frozen origin and cannot fail. Stepping on the frame clock, the old code
ends 160 px behind a hand that moved 320 and the new code is 0 px behind at every frame.

**Not every GdkEvent survives a signal into Python.** `GtkEventControllerLegacy::event` hands
its `GdkEvent` to the handler, and PyGObject cannot convert that type as a signal argument —
the parameter is `None` however the callback is written, which reads as GTK sending empty
events. `gtk_event_controller_get_current_event()` returns the same event, correctly typed as
`Gdk.MotionEvent` and the rest. The controller is worth the workaround because it is not a
`GtkGesture`: it takes no part in claiming sequences, so it cannot lose the drag to the
entry's own selection gesture, which is a race whose winner is GtkText's business rather than
ours.

**A widget's height is not its font's line height times its rows.** The band above her head
is sized so `bubble_rows` rows actually fit, measured off the label — and measuring one row
plus seven line heights came out two pixels short of eight rows, because Pango rounds the
height of a *layout*, not of each line in it. Measured as one eight-line block instead.
Before any of that the two numbers were simply independent, so raising `bubble_rows` grew the
text through the top edge of the strip, where there is no surface to draw on.

**Typography in one place is a size limit in another.** The strip cannot be taller than the
screen, and it is her box plus the lift headroom plus the band — so the band comes straight
off how far she can be scrolled up, whether or not she is speaking. `line-height: 1.15` and
3 px of bottom padding on the bubble, added to stop CJK descenders being shaved, cost 30 px of
band and quietly took 27 px off her maximum size; nobody would look for a zoom limit in a font
rule. They were also not what fixed the descenders — naming the font was, below — so they are
gone. Eight rows of reply still reserve 200 px of a 1080 px screen, which is the real dial:
`ext.bubble_rows` sets her ceiling as much as `strip.max_height` does.

**A GtkLabel's font is not one font.** Her text is Chinese in a UI theme whose font
(Adwaita Sans) has no CJK in it at all, so every line was drawn by fontconfig's fallback
while the line box came from a mixture of the two. Naming one family that covers both scripts
(`Noto Sans CJK SC`) makes the box and the ink agree, which is the difference between a
descender that has room and one that is shaved.

**A limit you clamp against and a limit you clamp to are not the same limit.** Her scale is
bounded twice: at the bottom by a constant, at the top by whatever the screen leaves after the
band and the lift headroom. Only the constant was ever written back, so scrolling up past the
screen's limit banked scale she was not wearing — she stopped growing while the number climbed
to 2.5, and the way down had to spend all of it before she shrank by a pixel, where one notch
up from the bottom always moved her. The asymmetry was the tell, and the wasted notches
re-solved the framing and reset her idle animation each time, which is what the stutter was.
Solved rather than searched: every term in the fit is linear in the scale except the screen,
so the scale at which the screen takes over falls out of the same arithmetic, and the wheel
clamps to *that*.

---

## Tests

```sh
ctest --test-dir build          # 4 suites, no compositor, no GPU
```

| | |
| --- | --- |
| `motion` | drag feel, lift, landing and squash, asserted without a pointer |
| `behaviour` | the personality: scheduling on a timescale of minutes, and the dialogue file |
| `ipc` | the control protocol over a real socket, with a client on a second thread — including a subscription: the reply still comes first, events arrive one per line, an ordinary caller alongside it is unaffected, and the daemon is told when the subscriber goes |
| `config` | the reader, and every way of getting the file wrong — including provider groups, the priority list, and triple-quoted prompts |

The config suite also parses what `asuna config init` writes and asserts it produces a
`Config` identical to a default-constructed one — which is what stops the documented
defaults and the compiled-in defaults drifting apart. Two of those assertions earn their
keep beyond that: the file must not enable extensions or vision, and it must have nowhere
to put an API key.

Separately, `asuna-render-test` renders offscreen against a real GPU but no compositor:

```sh
./build/asuna-render-test --framing auto   # all 42 outfits
./build/asuna-render-test --hit            # hit areas, all 42
./build/asuna-render-test --extent         # how far motions reach outside the box
./build/asuna-render-test --below          # how much body is under the crop line
./build/asuna-render-test --lean           # the physics chain, end to end

# One face, held still, with a parameter overruled - how the sleeping mouth was
# picked. --param is written in the same window Pet applies its own corrections
# in, so what it shows is what she will do.
./build/asuna-render-test --model 06 --expression F_SLEEP --motion none \
    --param PARAM_MOUTH_FORM=-0.2 --param PARAM_MOUTH_OPEN_Y=0.15
```

And a handful of `ASUNA_DEBUG_*` environment variables drive the paths that need a live
compositor but cannot be reached without a pointer this machine cannot synthesise:
`ASUNA_DEBUG_HOLD`, `_TOUCH`, `_GAZE`, `_SCALE`, `_MENU`, `_ACTION`, `_STREAM`, `_QUIT`,
`_REGION`. Three of them are worth knowing about:

- `_ACTION=<entry>` activates a right-click menu item through the same action group a click
  does, which is the only way to check that an entry is wired to the handler it names.
- `_GAZE=x1,y1,x2,y2` moves the "pointer" onto her and then beside her, which is the
  sequence the gaze halo exists for — and, with `_REGION=1`, prints the rectangle it claims
  and the outline it hands back when it expires.
- `_STREAM=<text>` writes that text into her bubble a few characters at a time, the way the
  helper does over the socket, so the emergence, the wrapping and the scroll can be
  photographed without an API key in the loop. `_TOUCH=x,y,ms` repeats a poke, which is how
  "poke her to carry on the conversation" gets tested end to end.

---

## Licensing

**Read this before publishing anything.** Three layers, and only one of them is clean.

1. **This code** — ours. MIT if we like.
2. **`easylive2d-cpp` V2** — `live2d-py` is MIT (© 2024 Arkueid), but the `easylive2d-cpp`
   repository **ships no LICENSE file**, and the code is a port of a deobfuscation of the
   Cubism 2.1 Web SDK, whose original licence prohibited reverse engineering. Its legal
   standing is genuinely murky.
3. **The Asuna model** — the `weblive2d` MIT declaration covers package code, **not** the
   character artwork or the model data.

Consequences, baked into the build:

- Vendor **only** `Common/`, `Glad/` and `V2/`. **Never `V3/`**, which carries the
  proprietary prebuilt Cubism Core binaries. The vendored tree is pruned to those three by
  hand; nothing enforces it automatically.
- **Do not publish this repository with `models/` or `third_party/live2d-v2/` in it.**
  `models/` is git-ignored and re-fetchable, so it takes care of itself.
  `third_party/live2d-v2/` **is committed**, deliberately — it is not reproducible by a
  plain fetch, because it carries the local patches in `PATCHES.md`. That is fine while this
  repository stays private, and it is the thing to remove first if it ever does not.
- If it ever goes public: this code only, plus `tools/fetch_models.py` — with the vendored
  runtime stripped and rebuilt from upstream plus `PATCHES.md`.

---

## Known limits

**Gaze only works while the pointer is on her, and for a moment after.** Wayland delivers
pointer motion to your own surface and nowhere else; there is no protocol for tracking the
cursor across the desktop. The `gaze_halo` grows her input region *after* a motion event has
already landed on her, so she keeps following as the cursor moves away — but that is a
mitigation, not global tracking. The only alternative is reading `/dev/input`, which needs
group membership and sees every device on the machine. That stays a future opt-in.

The halo is *input*, not only range: while it is up, that patch of desktop belongs to her
surface and the window underneath it cannot be clicked. So it is kept alive only by a cursor
that is actually moving and expires **350 ms after one stops** — measured at `gaze_halo =
160`, the claimed rectangle is 419,000 px against her outline's 80,000, which is not
something to leave lying around under a hand that has come to rest. A press that is not on
her drops it immediately as well, so the click after that one lands where it was aimed.
`gaze_halo = 0` turns the whole thing off.

**`strip.bottom_margin` needs a restart.** It is the one config key that does not reload:
the margin is a layer-shell surface property the compositor reads when the surface is
mapped. The request does go out live — `WAYLAND_DEBUG=1` shows `set_margin(0, 0, 60, 0)`
followed by a commit — and she does not move, so it is not something another call on our
side would fix. `asuna config reload` says so rather than leaving it to be discovered.

**HiDPI is written for but untested.** Everything crossing the GTK boundary goes through the
scale factor, and `applyFraming()` re-solves when it changes under her — but the only
display here is 1920×1080 at scale 1.0, where every one of those conversions is the identity,
so the scaled paths have never actually run.

**Multi-monitor is written for and half-tested.** `asuna output <name>` switches monitors,
keeps her in the corner she was in rather than at the pixel she was at, and remembers the
choice; if the monitor she is on is unplugged she moves to the first one still there without
forgetting which she prefers. Only one display exists here, so the enumeration, the
selection and the refusal are verified and the hotplug fallback is not.

**One outfit's worth of texture stays resident while hidden.** Freeing it would make
`asuna show` a two-second wait instead of instant.

**The capture deny list only reads the focused window on niri.** It shells out to
`niri msg --json focused-window`; anywhere else the app-id comes back empty and the list
matches nothing. That is why it is one of four gates rather than the gate — see
[Extensions](#extensions) — and why `vision_enabled` is off by default. If you run something
else, keep it off until the equivalent query is wired up.

**The chat prompt is a second process.** Her own strip asks the compositor for
`KEYBOARD_MODE_NONE` and should keep doing so — a desktop pet that can take keyboard focus
is a desktop pet that can eat your keystrokes. So typing at her means a separate surface
that *can* take focus, which is `tools/asuna-prompt.py`: no `python3-gobject` or
`gtk4-layer-shell`, no prompt (though `asuna chat "…"` still works, and `prompt_command`
takes anything).

**No Cubism 4.** Designed for — it means adding the official Core against `V3/` — but not
implemented, and see [Licensing](#licensing) first.

Dropped on purpose: hover tips (meaningless off-web), texture-variant switching, the
asteroids mini-game, hitokoto quotes (needs network), X11, other platforms, packaging.

---

## What is next

**[Extensions](#extensions) are in**, off by default: a chat window and an occasional remark
about what is on screen, driven by an OpenAI-compatible endpoint from a helper that runs out
of process. What is left there is smaller than what is done. The helper reacts to `chat` and
to nothing else, though it is subscribed to touches, drags, outfits and sleep — answering a
poke, or noticing that you have been carrying her around, needs no new protocol, only a
decision about how talkative that should be. Generation parameters are global rather than
per provider, which is one line to change when a local model wants a different temperature
from a hosted one. And the deny list reads the focused window through `niri msg`, so it
matches nothing on another compositor.

**Gaze across the whole desktop**, as an opt-in that reads `/dev/input`. See
[Known limits](#known-limits) for why the Wayland-only version stops at her outline, and for
what the opt-in would cost.

**Rewriting the shell on raw `wayland-client` + EGL was considered and dropped.** It existed
as a contingency for a ≤100 MB memory target. The measurements above kill it: everything
GTK is inside the 37 MB a hidden daemon costs, and a raw client still pays ~10–12 MB for
Mesa, libwayland and EGL — so the ceiling on the whole rewrite is roughly **25 MB out of
150**. Against that: reimplementing layer-shell binding, pointer input and hit regions,
cursor handling, the menu on a second surface, text shaping for the bubble, frame scheduling
and scale handling — about the size of everything already built, putting at risk exactly the
parts that work. 17 % of memory is not worth rebuilding the parts that work.
