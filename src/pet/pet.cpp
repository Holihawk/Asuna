#include "pet/pet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "GLCompat.hpp"
#include "LAppModel.hpp"
#include "json.hpp"

namespace asuna {
namespace {

// "mtn/I_FUN_S.mtn" -> "I_FUN_S".
std::string motionName(const std::string& file) {
    return std::filesystem::path(file).stem().string();
}

// Corrections to the model's own expressions, applied on top of them.
//
// F_SLEEP is the only one that needs any. It shuts her eyes and sets
// PARAM_MOUTH_FORM to 1 - the same smiling mouth as her resting face - so what
// it actually draws is someone smiling with their eyes closed, which is not what
// being asleep looks like. Overriding the mouth to a relaxed, faintly open one
// is the whole fix; the eyes, the brows and the downcast head are already right.
//
// The values are not guesses. They were rendered and looked at: 0 is a flat
// closed line, -0.5 reads as unhappy rather than asleep, and -1 is a frown. -0.2
// with a little PARAM_MOUTH_OPEN_Y is the one that reads as breathing.
// `asuna-render-test --expression F_SLEEP --param PARAM_MOUTH_FORM=<v>` is how
// to look at another.
struct Patch {
    const char* expression;
    const char* param;
    float value;
};
constexpr Patch kPatches[] = {
    {"F_SLEEP", "PARAM_MOUTH_FORM", -0.2f},
    {"F_SLEEP", "PARAM_MOUTH_OPEN_Y", 0.15f},
};

}  // namespace

Pet::Pet() : mModel(std::make_unique<live2d::LAppModel>()) {
    mFit.boxWidth = 300;
    mFit.boxHeight = 520;
}
Pet::~Pet() = default;

bool Pet::load(const std::string& modelJson) {
    // Build into a fresh model and only swap on success, so a bad path or a
    // corrupt .moc leaves the current outfit on screen instead of a blank strip.
    auto next = std::make_unique<live2d::LAppModel>();
    if (!next->loadModelJson(modelJson)) {
        fprintf(stderr, "asuna: failed to load model: %s\n", modelJson.c_str());
        return false;
    }
    mModel = std::move(next);
    mModelPath = modelJson;
    mLoaded = true;
    mPatch.clear();   // the old outfit's face went with the old model
    readIndexJson(modelJson);
    // Unless the caller insisted, take the framing the outfit asks for. The
    // full-body costumes are exactly the ones with no layout block.
    if (!mFramingExplicit) mFraming = framingFromModelJson(modelJson.c_str());
    fit(mFit.boxHeight, mPad, mBleed, mSideFraction, mKeepBelow);
    normaliseHitAreas();   // needs the geometry fit() just refreshed
    return true;
}

// Some outfits ship their hit-area markers under the wrong names: on asuna_27,
// _28 and _29, D_REF.PT_BODY carries the geometry that is PT_FOOT everywhere
// else, and vice versa. The framing solver already ignores the names for exactly
// this reason (see waistBottom in framing.cpp, which finds the waist
// geometrically), and the reactions have to as well - otherwise poking her waist
// on those three gets the "don't step on my skirt" line.
//
// The markers stack down the body, so their vertical order *is* their identity.
// Only applied when an outfit declares precisely the four standard areas: for
// anything else the declared names are all we have to go on.
void Pet::normaliseHitAreas() {
    static const char* kOrder[] = {"head", "chest", "body", "foot"};
    constexpr size_t kCount = 4;
    if (mHitAreas.size() != kCount) return;

    struct Marker {
        std::string drawable;
        float centreY;
    };
    std::vector<Marker> markers;
    for (const auto& area : mHitAreas) {
        if (std::find(std::begin(kOrder), std::end(kOrder), area.name) == std::end(kOrder))
            return;   // not the standard set - leave it alone
        float b[4];
        if (!mModel->getDrawableBounds(area.drawable, b)) return;
        markers.push_back({area.drawable, (b[1] + b[3]) * 0.5f});
    }
    std::sort(markers.begin(), markers.end(),
              [](const Marker& a, const Marker& b) { return a.centreY < b.centreY; });

    // Only a drawable that ends up under a *different* name than it was declared
    // with counts as a correction. index.json happens to list the areas in the
    // order chest, head, body, foot on every outfit, so comparing positions
    // rather than names would report all 42 as mislabelled.
    bool changed = false;
    for (size_t i = 0; i < kCount; ++i) {
        auto declared = std::find_if(mHitAreas.begin(), mHitAreas.end(),
                                     [&](const HitArea& a) { return a.name == kOrder[i]; });
        if (declared == mHitAreas.end() || declared->drawable != markers[i].drawable)
            changed = true;
    }
    for (size_t i = 0; i < kCount; ++i) mHitAreas[i] = {kOrder[i], markers[i].drawable};
    // Loud, not silent: a name that had to be corrected is worth knowing about,
    // and a future model where this fires wrongly should be easy to spot.
    if (changed)
        printf("asuna: hit areas re-ordered by geometry (this outfit's markers are mislabelled)\n");
}

// Two things the runtime loads but does not expose: which drawable belongs to
// which named hit area, and what each motion is called. The runtime indexes
// motions by position within a group, so without this the only way to ask for
// "I_FUN_S" would be a hard-coded number that differs per outfit.
void Pet::readIndexJson(const std::string& modelJson) {
    mHitAreas.clear();
    mMotionsByName.clear();
    mMotionNames.clear();
    mExpressionNames.clear();

    std::string error;
    const Json root = Json::parseFile(modelJson, &error);
    if (!root.isObject()) {
        // Not fatal: the model itself already loaded. She simply has no hit
        // areas and no motions by name.
        fprintf(stderr, "asuna: %s\n",
                error.empty() ? "index.json is not an object" : error.c_str());
        return;
    }

    const Json& areas = root["hit_areas"];
    for (size_t i = 0; i < areas.size(); ++i) {
        const std::string& name = areas[i]["name"].asString();
        const std::string& id = areas[i]["id"].asString();
        if (!name.empty() && !id.empty()) mHitAreas.push_back({name, id});
    }

    const Json& motions = root["motions"];
    for (size_t g = 0; g < motions.size(); ++g) {
        const std::string group = motions.keyAt(g);
        const Json& list = motions.valueAt(g);
        for (size_t i = 0; i < list.size(); ++i) {
            const std::string& file = list[i]["file"].asString();
            if (file.empty()) continue;
            const std::string name = motionName(file);
            mMotionsByName[name] = {group, static_cast<int>(i)};
            mMotionNames.push_back(name);
        }
    }

    const Json& expressions = root["expressions"];
    for (size_t i = 0; i < expressions.size(); ++i) {
        const std::string& name = expressions[i]["name"].asString();
        if (!name.empty()) mExpressionNames.push_back(name);
    }
}

void Pet::setFraming(Framing f) {
    mFramingExplicit = true;
    if (f == mFraming) return;
    mFraming = f;
    if (mLoaded) fit(mFit.boxHeight, mPad, mBleed, mSideFraction, mKeepBelow);
}

void Pet::setFramingAuto() {
    mFramingExplicit = false;
    if (!mLoaded) return;
    const Framing f = framingFromModelJson(mModelPath.c_str());
    if (f == mFraming) return;
    mFraming = f;
    fit(mFit.boxHeight, mPad, mBleed, mSideFraction, mKeepBelow);
}

int Pet::preferredBoxHeight(int bustHeight, int maxHeight) const {
    if (!mLoaded || mFraming == Framing::Bust) return bustHeight;
    float bust = 0, full = 0;
    if (!targetSpanY(*mModel, Framing::Bust, &bust) ||
        !targetSpanY(*mModel, Framing::Full, &full) || bust <= 0)
        return bustHeight;
    const int want = static_cast<int>(bustHeight * (full / bust) + 0.5f);
    return std::max(bustHeight, std::min(want, maxHeight));
}

void Pet::fit(int boxHeight, int pad, int bleed, float sideFraction,
              int keepBelow) {
    if (boxHeight <= 0) return;
    mFit.boxHeight = boxHeight;
    mPad = pad;
    mBleed = bleed;
    mSideFraction = sideFraction;
    mKeepBelow = keepBelow;
    if (!mLoaded) return;

    // Bounds come from the deformed vertex arrays, which draw() populates.
    // Nothing has been drawn yet at load time, so run the chain explicitly.
    mModel->refreshGeometry();

    const auto solve = [&](int drop, Fit* out) {
        return solveFit(*mModel, mFraming, boxHeight, pad, bleed, sideFraction,
                        drop, out);
    };

    Fit solved;
    if (!solve(0, &solved)) {
        fprintf(stderr, "asuna: could not measure model bounds, using defaults\n");
        mModel->resize(mFit.boxWidth, boxHeight);
        mModel->setScale(1.0f);
        mModel->setOffset(0.0f, 0.0f);
        return;
    }
    mFit = solved;
    apply();

    // How far she can be lifted before the artwork runs out.
    //
    // Every crop cuts her somewhere and `bleed` reserves room below the screen
    // edge for the body that continues past it - but reserving room does not
    // create artwork. The full-body costumes end at a flat horizontal cut of the
    // artist's own at the canvas edge, and how much leg is drawn past that cut
    // varies by outfit: measured at a 364 px box, asuna_43 has 56 px of it and
    // asuna_36/37/38 have 17. The lift is 33 px. So the same drag that reveals
    // more leg on one outfit lifts another clean off its own hem and leaves a
    // transparent gap standing on the desktop.
    //
    // Which of the two happens cannot be read off the geometry: asuna_31's
    // figure bounds run 2778 units past the canvas and still only 28 px of it
    // draws anything. So it is measured, once per fit, by rendering her at rest
    // and looking. That keeps the promise the rest of this file makes - solved
    // from what the model actually is, never a table of per-outfit constants -
    // and a fit happens on an outfit change or a resize, not on a frame.
    if (keepBelow > 0 && bleed > 0) {
        const int below = measureBelowEdge();
        const int drop = std::clamp(keepBelow - below, 0, bleed);
        if (below >= 0 && drop > 0 && solve(drop, &solved)) {
            mFit = solved;
            apply();
        }
    }
}

// Renders her at rest into an offscreen target and reports how many px of
// artwork sit below the visible bottom edge, or -1 if it could not be measured.
//
// Offscreen rather than into whatever is currently bound: a fit can be solved
// from inside the render pass, and the probe must not touch the frame being
// composed. The previous binding and viewport are restored.
int Pet::measureBelowEdge() const {
    const int w = mFit.boxWidth + 2 * mFit.sideBleed;
    const int h = mFit.boxHeight + mFit.bleed;
    if (w <= 0 || h <= 0 || mFit.bleed <= 0) return -1;

    GLint prevFbo = 0, prevVp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    // GTK leaves a scissor box behind once it has drawn a frame, and it is in
    // window coordinates that have nothing to do with this probe's target: left
    // on, it clips the whole probe away and the measurement silently reads zero.
    // That is why this only ever needed disabling on the costume-change path -
    // at startup nothing has been rendered yet for it to be left over from.
    const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);

    int result = -1;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        mModel->draw();

        std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        // Row 0 is the bottom of the viewport, which hangs `bleed` px below the
        // visible bottom edge; row `bleed` is the edge itself. The lowest row
        // carrying ink is how far her artwork actually reaches.
        result = 0;
        for (int y = 0; y < mFit.bleed; ++y) {
            const unsigned char* row = px.data() + static_cast<size_t>(y) * w * 4;
            bool ink = false;
            for (int x = 0; x < w && !ink; ++x) ink = row[x * 4 + 3] > 32;
            if (ink) { result = mFit.bleed - y; break; }
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    if (prevScissor) glEnable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return result;
}

void Pet::apply() {
    // The renderer is told the viewport it will actually get, both bleeds
    // included.
    mModel->resize(mFit.boxWidth + 2 * mFit.sideBleed, mFit.boxHeight + mFit.bleed);
    mModel->setScale(mFit.scale);
    mModel->setOffset(mFit.offsetX, mFit.offsetY);
}

void Pet::setExpression(const std::string& name) {
    if (!mLoaded) return;
    mModel->setExpression(name);
    repatch(name);
}

bool Pet::parameterDefault(const std::string& id, float* out) const {
    for (int i = 0; i < mModel->getParameterCount(); ++i) {
        if (mModel->getParameterId(i) != id) continue;
        *out = mModel->getParameterDefault(i);
        return true;
    }
    return false;
}

void Pet::repatch(const std::string& expression) {
    // Put back what the last face was holding. Without this, a parameter no
    // expression happens to mention - PARAM_MOUTH_OPEN_Y is one - would keep the
    // value the patch left until some motion got round to animating it, so she
    // would wake up still half-asleep about the mouth.
    for (const auto& [id, value] : mPatch) {
        float def = 0;
        if (parameterDefault(id, &def)) mModel->setParameterValue(id, def);
    }
    mPatch.clear();

    for (const Patch& p : kPatches)
        if (expression == p.expression) mPatch.emplace_back(p.param, p.value);
}

void Pet::startRandomMotion(const std::string& group, int priority) {
    if (mLoaded) mModel->startRandomMotion(group, priority);
}

bool Pet::motionFinished() const {
    return mLoaded ? mModel->isMotionFinished() : true;
}

bool Pet::startMotionNamed(const std::string& name, int priority) {
    if (!mLoaded) return false;
    auto it = mMotionsByName.find(name);
    if (it == mMotionsByName.end()) {
        // The default group is the one every outfit has; a costume with a
        // different motion inventory reacts with something rather than nothing.
        mModel->startRandomMotion("", priority);
        return false;
    }
    mModel->startMotion(it->second.group, it->second.index, priority);
    return true;
}

void Pet::setLean(float x, float y) {
    // Straight into the target point rather than through LAppModel::drag(),
    // which expects screen pixels and converts them: the lean is already
    // normalised, and going through the pixel path would tie it to the box size.
    if (mLoaded) mModel->mDragMgr.set(x, y);
}

void Pet::update(float deltaSec) {
    if (!mLoaded) return;
    mModel->update(deltaSec);
    // After update() and before draw() on purpose: the runtime runs the deformer
    // chain in draw(), so this window is the documented place to overrule a
    // parameter. The motion, the blink and the expression have all had their say
    // by now, which is exactly what a correction to an expression has to beat.
    for (const auto& [id, value] : mPatch) mModel->setParameterValue(id, value);
}

// The box is positioned by the viewport, so the model itself never needs to know
// about the strip. GL's viewport origin is bottom-left, which is exactly where
// we want her: standing on the bottom edge of the screen, and `lift` px above it
// while she is being carried.
//
// Squash and stretch are the same viewport: growing it vertically while keeping
// its bottom edge pins the effect to her feet, and the horizontal half-difference
// keeps her centre line where it was.
//
// The viewport hangs `bleed` px below the screen edge, so the body that continues
// past the crop - torso under a bust, calf under a full body - is rendered but
// clipped away until a lift brings it up, and
// overhangs the box by `sideBleed` px on each side so a pose that reaches outward
// keeps its hands. At rest this is exactly the framing solved for the visible
// box; nothing about her resting position depends on either.
void Pet::viewportFor(const Placement& p, int out[4]) const {
    const int wide = mFit.boxWidth + 2 * mFit.sideBleed;
    out[2] = static_cast<int>(std::lround(wide * p.scaleX));
    out[3] = static_cast<int>(std::lround((mFit.boxHeight + mFit.bleed) * p.scaleY));
    out[0] = static_cast<int>(
        std::lround(p.x - mFit.sideBleed + (wide - out[2]) * 0.5));
    out[1] = static_cast<int>(std::lround(p.lift)) - mFit.bleed;
}

void Pet::draw(const Placement& p) {
    if (!mLoaded) return;
    int vp[4];
    viewportFor(p, vp);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    mModel->draw();
}

// --- pointer <-> model ------------------------------------------------------
//
// The runtime builds its MVP as projection * modelMatrix, where the model
// matrix maps canvas coordinates (origin top-left, +y down) into a square
// centred on the origin:
//
//     mx -> 2*mx/canvasW - 1                (a plain remap of the width)
//     my -> (canvasH - 2*my)/canvasW        (note: canvasW, so aspect is kept)
//
// and the projection is the framing's scale and offset, times the per-axis
// factor `ax`/`ay` that depends on which side of the viewport is shorter. What
// comes out is NDC; the viewport turns that into device pixels. Both directions
// below are that chain, and nothing else - which is why they keep working
// through a lift, a squash and a resize without any of them being special-cased.

bool Pet::modelToScreen(const Placement& p, float mx, float my, double* sx,
                        double* sy) const {
    if (!mLoaded) return false;
    const double canvasW = mModel->getCanvasWidth();
    const double canvasH = mModel->getCanvasHeight();
    if (canvasW <= 0 || canvasH <= 0) return false;

    const double ndcX = mFit.ax * mFit.scale * (2.0 * mx / canvasW - 1.0) + mFit.offsetX;
    const double ndcY =
        mFit.ay * mFit.scale * ((canvasH - 2.0 * my) / canvasW) + mFit.offsetY;

    int vp[4];
    viewportFor(p, vp);
    *sx = vp[0] + (ndcX + 1.0) * 0.5 * vp[2];
    *sy = vp[1] + (ndcY + 1.0) * 0.5 * vp[3];
    return true;
}

bool Pet::screenToModel(const Placement& p, double sx, double sy, float* mx,
                        float* my) const {
    if (!mLoaded) return false;
    const double canvasW = mModel->getCanvasWidth();
    const double canvasH = mModel->getCanvasHeight();
    if (canvasW <= 0 || canvasH <= 0) return false;

    int vp[4];
    viewportFor(p, vp);
    if (vp[2] <= 0 || vp[3] <= 0) return false;

    const double kx = mFit.ax * mFit.scale;
    const double ky = mFit.ay * mFit.scale;
    if (kx == 0 || ky == 0) return false;

    const double ndcX = 2.0 * (sx - vp[0]) / vp[2] - 1.0;
    const double ndcY = 2.0 * (sy - vp[1]) / vp[3] - 1.0;

    *mx = static_cast<float>(((ndcX - mFit.offsetX) / kx + 1.0) * canvasW * 0.5);
    *my = static_cast<float>(
        (canvasH - ((ndcY - mFit.offsetY) / ky) * canvasW) * 0.5);
    return true;
}

std::string Pet::hitAreaAt(const Placement& p, double sx, double sy) const {
    float mx = 0, my = 0;
    if (!screenToModel(p, sx, sy, &mx, &my)) return "";

    // Smallest first, so a point inside both the head marker and something that
    // encloses it answers "head". The alternative - trusting index.json's order -
    // is exactly the assumption that made asuna_27 and asuna_28 crop at the
    // ankles back in Phase 2b.
    const std::string* best = nullptr;
    float bestArea = 0;
    for (const auto& area : mHitAreas) {
        float b[4];
        if (!mModel->getDrawableBounds(area.drawable, b)) continue;
        if (mx < b[0] || mx > b[2] || my < b[1] || my > b[3]) continue;
        const float size = (b[2] - b[0]) * (b[3] - b[1]);
        if (!best || size < bestArea) {
            best = &area.name;
            bestArea = size;
        }
    }
    return best ? *best : std::string();
}

bool Pet::headCentre(float* mx, float* my) const {
    if (!mLoaded) return false;
    float b[4];
    if (!mModel->getDrawableBounds("D_REF.PT_HEAD", b)) return false;
    *mx = (b[0] + b[2]) * 0.5f;
    *my = (b[1] + b[3]) * 0.5f;
    return true;
}

}  // namespace asuna
