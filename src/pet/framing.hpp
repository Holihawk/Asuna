#pragma once

namespace live2d {
class LAppModel;
}

namespace asuna {

// How much of the character to show.
//
// Bust is the default because these models are authored for it: 28 of the 42
// Asuna outfits declare layout {width: 2.9, y: 1.3} in their index.json, which
// is a bust crop. The other 14 declare no layout at all, meaning the whole
// canvas - those are the full-body costumes.
enum class Framing {
    Bust,   // head down to the bottom of the D_REF.PT_BODY hit area
    Full,   // whatever of the figure the artwork actually contains
};

Framing parseFraming(const char* name);
const char* framingName(Framing f);

// Framing that a model's own index.json asks for: no layout block means the
// widget showed the whole canvas, a layout block means it cropped to a bust.
Framing framingFromModelJson(const char* modelJsonPath);

// Everything the renderer needs to put a chosen crop of a model exactly inside
// a box. Solved from the model's measured geometry, never hand-tuned, so a new
// outfit needs no per-model constants.
struct Fit {
    int boxWidth = 0;
    int boxHeight = 0;   // the *visible* box: what the strip shows
    // Extra body rendered below the visible bottom edge, in px. Every crop cuts
    // her somewhere - the bust at the waist, the full body at the canvas edge,
    // which lands mid-calf - so lifting her off the screen edge would otherwise
    // expose that cut as a blunt slice. Rendering past it means a lift reveals
    // more of her instead, and the cut travels down off the screen edge with
    // her. The artwork continues below both crop lines (measurably: see
    // asuna-render-test --below), so this costs nothing but a taller viewport,
    // and the rows below the framebuffer are clipped before they are shaded.
    int bleed = 0;
    // Extra width rendered to the left and right of the visible box, in px.
    //
    // Unlike `bleed`, this is not hidden: it is there precisely so it can be
    // seen. The box is solved around her *resting* silhouette, but a motion
    // moves her - a crossed-arms pose puts her elbows and hands well outside
    // the resting outline - and the viewport is a hard clip, so anything past
    // the box edge is simply not drawn. Widening the viewport without widening
    // the box keeps her apparent size, her position and her framing exactly as
    // solved; it only stops the sides being cut off.
    int sideBleed = 0;
    // How far the framing was slid down inside the viewport, in px, to keep
    // enough artwork below the screen edge to survive a full lift. Zero for
    // every bust framing and for the full-body outfits whose legs already run
    // far enough past the canvas edge. See Pet::fit.
    int drop = 0;
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    // The per-axis factor the runtime's projection applies, which depends on
    // which side of the viewport is shorter. Solved here rather than recomputed
    // by every caller: mapping a pointer back to a point on her body needs the
    // exact same branch the renderer took, and two copies of it would drift.
    float ax = 1.0f;
    float ay = 1.0f;
    // The model-space rectangle that was framed, for logging: left, top,
    // right, bottom in canvas coordinates (origin top-left, +y down).
    float target[4] = {0, 0, 0, 0};
};

// Measure `model` and solve for a box `boxHeight` px tall with `pad` px of
// transparent margin around the figure, plus `bleed` px of extra body rendered
// below it. The model must have had its deformer chain run at least once
// (LAppModel::refreshGeometry or a draw).
//
// `bleed` never changes her apparent size or where she sits at rest: the scale
// is solved from the visible height, and the crop line lands in exactly the same
// place either way. It only decides how much is waiting below the screen edge.
//
// `sideFraction` is the side bleed as a fraction of the solved box width - a
// fraction rather than pixels because only the solver knows that width, and
// because it has to hold across every user scale. Neither bleed moves her: both
// only widen the viewport the framing is drawn into.
//
// `drop` slides the solved framing down inside the viewport by that many px,
// spending headroom above her head to put more of her below the visible bottom
// edge. It is clamped to `bleed`, and it changes neither her size nor the box.
//
// Returns false if the model exposes no measurable geometry, leaving `out`
// untouched.
bool solveFit(const live2d::LAppModel& model, Framing framing, int boxHeight,
              int pad, int bleed, float sideFraction, int drop, Fit* out);

// Vertical extent of a crop in model units, without solving a whole fit.
//
// This is what keeps her apparent size constant across framings: a full-body
// crop spans roughly 1.9x what a bust does, so it needs a proportionally taller
// strip or her head shrinks to the point the line art stops resolving.
bool targetSpanY(const live2d::LAppModel& model, Framing framing, float* spanY);

}  // namespace asuna
