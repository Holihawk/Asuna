#include "pet/framing.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "LAppModel.hpp"

namespace asuna {
namespace {

// Bottom edge of the waist band, i.e. where a bust crop should end.
//
// Not simply D_REF.PT_BODY: asuna_27, _28 and _29 ship with their PT_BODY and
// PT_FOOT markers swapped, so trusting the name gives a crop at the ankles on
// those three. The markers stack down the body, so the waist is whichever of the
// two sits nearest under the chest - a geometric test that is right for all 42.
// Pet::normaliseHitAreas() applies the same principle to the tap reactions.
bool waistBottom(const live2d::LAppModel& model, float chestBottom, float* out) {
    bool found = false;
    float best = 0;
    for (const char* id : {"D_REF.PT_BODY", "D_REF.PT_FOOT"}) {
        float b[4];
        if (!model.getDrawableBounds(id, b)) continue;
        if (b[3] <= chestBottom) continue;
        if (!found || b[3] < best) {
            best = b[3];
            found = true;
        }
    }
    if (found) *out = best;
    return found;
}

// The model-space rectangle a framing wants on screen, in canvas coordinates
// (origin top-left, +y down).
bool measureTarget(const live2d::LAppModel& model, Framing framing, float out[4]) {
    float figure[4];
    if (!model.getFigureBounds(figure)) return false;

    // Artwork routinely runs outside its own canvas - a rapier, a bridal veil,
    // long hair past the feet. The widget these models come from shows the
    // canvas and lets the overflow crop, so framing the raw figure bounds would
    // zoom out to fit parts the reference renders never show. asuna_31's rapier
    // alone reaches x=2353 and y=5773 in a 2000x3000 canvas.
    const float canvasW = model.getCanvasWidth();
    const float canvasH = model.getCanvasHeight();
    out[0] = std::max(figure[0], 0.0f);
    out[1] = std::max(figure[1], 0.0f);
    out[2] = std::min(figure[2], canvasW);
    out[3] = std::min(figure[3], canvasH);

    if (framing == Framing::Bust) {
        // Cutting at the waist gives head, shoulders, chest and the top of the
        // skirt - the crop the reference art uses. Halving the figure keeps a
        // model without the standard D_REF markers usable rather than silently
        // full-body.
        float chest[4];
        const bool haveChest = model.getDrawableBounds("D_REF.PT_CHEST", chest);
        float waist;
        if (waistBottom(model, haveChest ? chest[3] : out[1], &waist))
            out[3] = waist;
        else
            out[3] = out[1] + (out[3] - out[1]) * 0.5f;

        // Long side hair keeps falling past the crop, so measuring width over
        // the whole figure would pad the box with empty columns. Clamping to
        // the head/chest span keeps the box tight around her.
        float head[4];
        if (haveChest && model.getDrawableBounds("D_REF.PT_HEAD", head)) {
            const float cx = (std::min(head[0], chest[0]) + std::max(head[2], chest[2])) * 0.5f;
            const float halfW = std::max(head[2] - head[0], chest[2] - chest[0]) * 0.95f;
            out[0] = std::max(out[0], cx - halfW);
            out[2] = std::min(out[2], cx + halfW);
        }
    }
    return out[2] > out[0] && out[3] > out[1];
}

}  // namespace

Framing parseFraming(const char* name) {
    return (name && std::strcmp(name, "full") == 0) ? Framing::Full : Framing::Bust;
}

const char* framingName(Framing f) {
    return f == Framing::Bust ? "bust" : "full";
}

Framing framingFromModelJson(const char* modelJsonPath) {
    std::ifstream f(modelJsonPath);
    if (!f) return Framing::Bust;
    std::stringstream ss;
    ss << f.rdbuf();
    // A "layout" block is the widget's crop instruction; every outfit that has
    // one asks for {width: 2.9, y: 1.3}, a bust. Outfits with no layout were
    // shown whole-canvas, i.e. full body.
    return ss.str().find("\"layout\"") == std::string::npos ? Framing::Full
                                                            : Framing::Bust;
}

bool targetSpanY(const live2d::LAppModel& model, Framing framing, float* spanY) {
    float t[4];
    if (!measureTarget(model, framing, t)) return false;
    *spanY = t[3] - t[1];
    return true;
}

bool solveFit(const live2d::LAppModel& model, Framing framing, int boxHeight,
              int pad, int bleed, float sideFraction, int drop, Fit* out) {
    if (boxHeight <= 2 * pad) return false;
    bleed = std::max(0, bleed);
    drop = std::clamp(drop, 0, bleed);

    float t[4];
    if (!measureTarget(model, framing, t)) return false;

    const double spanX = t[2] - t[0];
    const double spanY = t[3] - t[1];

    // Screen pixels per model unit, chosen so the framing fills the *visible*
    // box height. Solving from the visible height rather than the rendered one
    // is what makes the bleed invisible at rest.
    const double k = static_cast<double>(boxHeight - 2 * pad) / spanY;
    const int boxWidth = static_cast<int>(std::ceil(spanX * k)) + 2 * pad;
    const int sideBleed =
        static_cast<int>(std::ceil(boxWidth * std::max(0.0f, sideFraction)));

    // The viewport is taller than the visible box by the bleed and wider by the
    // side bleed, and everything the projection does is measured against the
    // viewport. Both are symmetric about the box, so the target's centre is
    // still the viewport's centre and none of the offsets below change.
    const double full = boxHeight + bleed;
    const double wide = boxWidth + 2.0 * sideBleed;

    // MatrixManager::getMvp normalises the canvas so one model unit becomes
    // scale * min(viewportW, viewportH) / canvasWidth pixels - it picks its
    // projection branch on exactly that comparison, and the shorter side wins
    // either way. The 0.0005 below is 1/2000, and every one of the 42 outfits is
    // 2000 units wide; a model of any other width would need this read from the
    // canvas.
    const double m = std::min(wide, full);
    const double scale = k / (0.0005 * m);

    // The same branch, as the per-axis factor the projection applies.
    const double ax = (full > wide) ? 1.0 : full / wide;
    const double ay = (full > wide) ? wide / full : 1.0;

    // Centre the target horizontally, and put its bottom edge `pad` px above the
    // visible bottom edge - which sits `bleed` px up from the viewport's own
    // bottom - so she stands on the bottom edge of the screen.
    const double centreX = (t[0] + t[2]) * 0.5;
    // `drop` slides the whole framing down inside the viewport, which trades
    // headroom above her for artwork below the screen edge one-for-one. Nothing
    // else in the solve moves: the scale is already fixed by the visible height,
    // so this changes where she sits, not how big she is.
    const double bottomNdc = 2.0 * (pad + bleed - drop) / full - 1.0;

    out->boxWidth = boxWidth;
    out->boxHeight = boxHeight;
    out->bleed = bleed;
    out->drop = drop;
    out->sideBleed = sideBleed;
    out->scale = static_cast<float>(scale);
    out->ax = static_cast<float>(ax);
    out->ay = static_cast<float>(ay);
    out->offsetX = static_cast<float>(ax * scale * (1.0 - 0.001 * centreX));
    out->offsetY = static_cast<float>(bottomNdc - ay * scale * (1.5 - 0.001 * t[3]));
    std::copy(t, t + 4, out->target);
    return true;
}

}  // namespace asuna
