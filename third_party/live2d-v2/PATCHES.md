# Local patches to the vendored runtime

Vendored from `EasyLive2D/easylive2d-cpp`, pinned at the commit in
`PINNED-COMMIT`. Only `Common/`, `Glad/` and `V2/` are vendored — `V3/` is
deliberately excluded because it carries prebuilt proprietary Cubism Core
binaries that a Cubism 2.1 (`.moc`) model never needs.

Re-vendoring means re-applying everything below.

## 1. `V2/cmake/V2.cmake` — Linux link line

Upstream:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(${V2_TARGET} PRIVATE stdc++fs)
endif()
```

Two problems, both Linux-only, which is presumably why upstream (whose CI and
sample build instructions are Windows/MSVC) never hit them:

1. It mixes CMake's keyword signature with the plain signature used on lines 11
   and 14. CMake rejects this: *"All uses of target_link_libraries with a
   target must be either all-keyword or all-plain."* Configure fails outright.
2. `stdc++fs` is obsolete. `std::filesystem` moved into libstdc++ proper in
   GCC 9, and Fedora no longer ships a separate `libstdc++fs`.

Patched to use the plain signature and to link `stdc++fs` only for GCC < 9.

## 2. `<cstdint>` includes — 15 files

GCC 16 no longer pulls `<cstdint>` in transitively, so every file that names
`uint8_t`/`int16_t` without including it fails to compile. Upstream is
MSVC-centric and never saw this. `#include <cstdint>` was inserted after the
`#pragma once`, or before the first `#include`, in the 15 affected files.

## 3. GLES 3 backend — `V2/cmake/V2.cmake`, `V2/src/GLCompat.hpp`, `DrawParamOpenGL.cpp`, `ModelContext.cpp`

GtkGLArea cannot hand out a desktop compatibility profile, so the runtime has to
build against OpenGL ES 3. Guarded by the `LIVE2D_GLES` option (on by default).

- `V2.cmake` gains a GLES branch: defines `LIVE2D_GLES`, links `GLESv2`, and
  skips the bundled glad loader.
- New `V2/src/GLCompat.hpp` picks `<GLES3/gl3.h>` or glad's desktop header.
- `DrawParamOpenGL.cpp` emits `#version 100` instead of `#version 120`, and its
  `GL_FRAMEBUFFER_SRGB` probe is compiled out (desktop-only enum). The shaders
  themselves needed nothing: they already carried
  `#ifdef GL_ES / precision mediump float`, having been written for Android.
- `ModelContext.cpp` guards `glPushDebugGroup`/`glPopDebugGroup`, which are
  KHR_debug and desktop-only.

Verified pixel-identical to the desktop-GL baseline.

## 4. Premultiplied textures — `LAppModel.cpp`, `DrawParamOpenGL.cpp`

**This was a visible rendering bug, not a portability fix.**

Upstream uploaded straight-alpha texels and then called `glGenerateMipmap`.
Averaging straight alpha is wrong: transparent texels in a Live2D atlas carry
arbitrary RGB, so every mip level bleeds them into their opaque neighbours. On
asuna_02 that erased the eyelash and mouth line art entirely and left a visible
seam around each drawable — the mouth read as a pale patch on the face.

`LAppModel.cpp` now premultiplies RGB by alpha before `glTexImage2D`, and uses
`GL_LINEAR_MIPMAP_LINEAR`. To match, `DrawParamOpenGL.cpp` prepends
`#define PREMULTIPLIED_TEXTURES 1` to every shader and `#ifndef`s out the two
`rgb *= a` steps that would otherwise multiply a second time. The version line
moved out of the shader string literals into `compileShader` so the define can
precede the body.

`ASUNA_TEXFILTER` selects the strategy at runtime, for comparing against the
original: `raw` (upstream behaviour, straight alpha + shader premultiply),
`linear` (premultiplied, no mipmaps), `mipnear`, `miplinear` (default).

## 5. Geometry read-back — `LAppModel.hpp/.cpp`

Added `refreshGeometry()`, `getDrawableBounds()` and `getFigureBounds()`. These
let the host measure the model's own silhouette and its `D_REF.PT_*` hit-area
rectangles, which is what `src/framing.cpp` solves the on-screen framing from.
Without them every outfit would need hand-tuned scale constants.

`refreshGeometry()` exists because the deformer chain runs inside `draw()`, so
bounds queried before the first frame would otherwise all read zero.

## 6. Drawable dump — `Model/ModelContext.cpp`

`ASUNA_DRAWLIST=1` prints one line per drawable in draw order (id, part,
texture, composition mode, clip, opacity). Added while chasing patch 4; kept
because it is the fastest way to see what a new outfit is actually drawing.
The pre-existing `V2CPP_DUMP` per-draw framebuffer dump gained
`V2CPP_DUMP_FROM`/`V2CPP_DUMP_TO` to select a range instead of only the first 25.

## 7. Physics never ran — `LAppModel.cpp`, `Framework/L2DPhysics.cpp`

Two independent upstream faults, either of which alone is enough to leave every
model's hair and skirt chains completely inert. Nothing looks broken until
something actually swings the body, which is why it survived Phase 0's gate: the
drag lean reached `PARAM_ANGLE_X` correctly, but `PARAM_HAIR_*` never moved off
zero.

**7a. `index.json`'s `"physics"` key was never read.** `loadModelJson()` parses
`model`, `textures`, `init_parts_visible`, `init_params`, `pose`, `motions` and
`expressions` — but not `physics`. `L2DBaseModel::loadPhysics()` existed and was
never called from anywhere, so `mPhysics` stayed the empty instance the
constructor default-builds and `updateParam()` walked an empty list. Now parsed
in the same shape as the adjacent `"pose"` block.

**7b. `ptype` always parsed as `":"`.** The src/target readers in
`L2DPhysics::load` located a value with `find('"', keyPos + 5)` — an offset
hard-coded for the 4-character key `"id"`. For `"ptype"` (7 characters) it lands
on the key's own closing quote, so the "value" came back as the colon between
key and value. Neither `"x"`, `"y"` nor `"angle"` matched, and every src and
target hit the `else { continue; }` branch: each chain was built with no inputs
and no outputs. Replaced with `jsonString`/`jsonNumber` helpers that find the
colon after the key first, so any key length works.

Verified with `asuna-render-test --lean 1.0`, which holds the drag lean at a
fixed value and traces the chain `setLean` → `PARAM_ANGLE_X` /
`PARAM_BODY_ANGLE_X` → physics → `PARAM_HAIR_FRONT/SIDE/BACK`. Before: the hair
column is flat zero. After: it swings its full ±1 range.

## 8. Wall-clock animation quantised to a staircase — `Util/UtSystem.cpp`, `LAppModel.cpp`, `Framework/L2DMotionManager.{hpp,cpp}`, `Framework/L2DEyeBlink.{hpp,cpp}`, `Framework/L2DPose.{hpp,cpp}`

**Another visible rendering bug, and the reason her idle motion stuttered while
her gaze did not.**

`UtSystem::getUserTimeMSec()` returned `steady_clock`'s epoch, which on Linux is
`CLOCK_MONOTONIC` — time since the machine booted. Every caller then stored it
in a `float`. A 24-bit mantissa cannot resolve a frame step out at that
magnitude: at nine days of uptime the nearest representable values are 64 ms
apart, so `now` only changed every ninth frame at 144 Hz, and changed by 64 ms
when it did.

Everything sampled from that clock inherited the staircase — the breath sway and
nod, the idle `.mtn` motions, eye blink, pose fades. Rendering faster bought
nothing, because the *phase* was quantised, not the frame delivery. Measured on
the shipped breath expression: `PARAM_ANGLE_X` took 17 distinct values per
second instead of 144.

It hid for two reasons. It scales with the host's uptime, not with anything the
app does, so it is absent on a freshly booted machine and worst on the
long-uptime desktop a pet actually lives on. And the one idle motion that looked
fine — the gaze — is the one that never reads this clock: `LAppModel::update()`
advances it a fixed `0.016f` per rendered frame.

Two fixes, both kept:

- `UtSystem` now measures from its own first call, so the magnitude is bounded by
  asuna's runtime rather than the machine's. This alone is not sufficient — a
  session that runs a day is back to 8 ms — but it keeps any float-typed caller
  re-vendored from upstream well conditioned for a plausible session.
- Absolute timestamps in the four consumers were widened to `double`
  (`MotionQueueEntry::m{Start,FadeInStart,End}TimeMs`, `L2DEyeBlink::m{CurrentTime,
  NextBlinkTime,StateStartTime}`, `L2DPose::mLastTime`, and the breath's `t`).
  Differences are still narrowed to `float` immediately after subtraction, since
  each is small and bounded; only the absolute values needed the range.

Note `L2DPhysics` was already correct — it holds its timestamps in `long long`.

## 9. Gaze easing ran per frame, not per second — `LAppModel.{hpp,cpp}`

`LAppModel::update()` passed a hard-coded `0.016f` to `mDragMgr.update()`.
`L2DTargetPoint::update` scales its lerp by the interval it is given, so it was
already written to be time-based; feeding it a constant made it converge per
*frame* instead. At 144 Hz her head snapped to the cursor about five times
faster than at 30, and any frame-rate cap silently changed the feel of the drag.

`update()` now takes `float deltaSec`, defaulted to `1.0f/60.0f` so upstream's
own call sites still compile. The host passes the interval between renders -
which is the right clock, because this is advanced on the render, not on the
tick, and the two diverge as soon as a cap is in force.
