#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pet/framing.hpp"
#include "pet/motion.hpp"

namespace live2d {
class LAppModel;
}

namespace asuna {

// Owns the Live2D model and knows where it sits inside the strip.
//
// The pet is drawn into a "box" - a sub-rectangle of the layer surface. Moving
// her horizontally is just moving that box, which is why dragging never needs
// to move a window (Wayland would not allow it anyway).
//
// The box is not configured by hand. fit() measures the model's own geometry
// and solves for the scale, offsets and box width that put the chosen framing
// exactly inside a box of the requested height. That is what makes a second
// outfit a drop-in: nothing about asuna_02's proportions is baked in.
class Pet {
public:
    Pet();
    ~Pet();

    // Loads a model, replacing any current one. Costume switching is just
    // this: the old model's GL resources go with its destructor, and the new
    // outfit re-solves its own framing, so a bust outfit and a full-body one
    // can swap at runtime without any caller-side bookkeeping.
    bool load(const std::string& modelJson);
    bool loaded() const { return mLoaded; }
    const std::string& modelPath() const { return mModelPath; }

    // Strip height that keeps her apparent size the same as `bustHeight` would
    // give a bust framing, capped at `maxHeight`. A full-body crop spans about
    // 1.9x a bust, so showing one in a bust-sized strip shrinks her head to
    // where the line art stops resolving.
    int preferredBoxHeight(int bustHeight, int maxHeight) const;

    // Framing::Auto is not an enum value: pass std::nullopt-like intent by
    // leaving this unset and load() will take the model's own answer from its
    // index.json.
    void setFraming(Framing f);
    // Hands the decision back to the model's index.json - the state a Pet starts
    // in, and the one `framing = "auto"` in the config file has to be able to
    // return to when a reload undoes an explicit setting.
    void setFramingAuto();
    Framing framing() const { return mFraming; }

    // Solve the framing into a visible box `boxHeight` px tall, leaving `pad` px
    // around the figure, rendering `bleed` px of extra body below the bottom
    // edge and `sideFraction` of the box width past each side. Sets the box
    // width as a side effect - read it with boxWidth(). Safe to call again after
    // the framing, height or either bleed changes.
    // `keepBelow` is how much artwork must remain below the visible bottom edge
    // once she is standing at rest, in px, so that lifting her never lifts her
    // clean off her own hem. The fit measures what the outfit actually draws
    // down there and slides her down by the shortfall, if any. 0 disables it.
    void fit(int boxHeight, int pad = 8, int bleed = 0, float sideFraction = 0.0f,
             int keepBelow = 0);

    int boxWidth() const { return mFit.boxWidth; }
    int boxHeight() const { return mFit.boxHeight; }
    // Px drawn beyond each side of the box. Callers that reason about the
    // pixels she covers - the input region, the alpha readback - need the
    // rendered width, not the box width.
    int sideBleed() const { return mFit.sideBleed; }
    const Fit& fitResult() const { return mFit; }

    // Applies one of the model's own expressions, plus any correction this build
    // makes to it - see kPatches in pet.cpp. Every route to a face goes through
    // here (the scheduler, the menu, `asuna expression`), so a correction is
    // made once and cannot be got round.
    void setExpression(const std::string& name);
    void startRandomMotion(const std::string& group, int priority = 3);
    bool motionFinished() const;

    // Play a motion by its file's base name, e.g. "I_FUN_S". The runtime only
    // indexes motions by position within a group, so the names come out of
    // index.json at load time. An outfit that does not have that motion falls
    // back to a random one from the default group rather than standing still.
    bool startMotionNamed(const std::string& name, int priority = 3);

    // Pointer position -> a point on the model canvas, and back. Screen
    // coordinates are device px with the origin at the bottom-left of the strip,
    // i.e. GL's own convention, because that is the frame the viewport is in.
    //
    // This inverts exactly the transform chain the renderer used for this
    // frame - projection branch, framing scale and offsets, lift and squash -
    // so it stays correct while she is being carried or landing. The runtime's
    // own LAppModel::hitTest cannot: it assumes an untranslated, unit-scaled
    // projection, which our framing is not.
    bool screenToModel(const Placement& p, double sx, double sy, float* mx, float* my) const;
    bool modelToScreen(const Placement& p, float mx, float my, double* sx, double* sy) const;

    // The name of the hit area under a screen point, or "" for none. Areas are
    // the model's own D_REF.PT_* rectangles, tested smallest first so the head
    // wins over anything that encloses it.
    std::string hitAreaAt(const Placement& p, double sx, double sy) const;

    // Centre of the head marker, in canvas coordinates - what the gaze aims
    // from, so looking "at the cursor" pivots on her face rather than her box.
    bool headCentre(float* mx, float* my) const;

    // What this outfit actually has, in the order its index.json lists it. The
    // menu is built from these rather than from a hard-coded list, so an outfit
    // with a different inventory shows its own.
    const std::vector<std::string>& expressionNames() const { return mExpressionNames; }
    const std::vector<std::string>& motionNames() const { return mMotionNames; }

    // How far she leans, -1..1 horizontally and vertically, as a target the
    // runtime eases toward. It drives PARAM_ANGLE_* / PARAM_BODY_ANGLE_X, and
    // because those are set before the physics pass, her hair and skirt swing
    // from it for free. Drag velocity feeds it today; the cursor will in Phase 4.
    void setLean(float x, float y);

    // deltaSec is the interval since the previous update, which is what the
    // runtime eases the lean toward its target over. Pass the render delta, not
    // the tick delta: this runs on the render.
    void update(float deltaSec);
    // Into the currently bound framebuffer, at the placement the motion model
    // asked for. Position, lift and squash are all just the viewport: no
    // re-solving, no per-frame cost beyond the draw that was happening anyway.
    void draw(const Placement& placement);

private:
    // The rectangle draw() will hand glViewport for this placement, in device
    // px. Shared with the coordinate transforms so a hit test can never be
    // measured against a viewport different from the one she was drawn into.
    void viewportFor(const Placement& p, int out[4]) const;
    void readIndexJson(const std::string& modelJson);
    void normaliseHitAreas();
    // Swaps in the corrections that belong to `expression`, putting back the
    // model's own default for anything the previous one had held.
    void repatch(const std::string& expression);
    bool parameterDefault(const std::string& id, float* out) const;

    struct HitArea {
        std::string name;       // "head"
        std::string drawable;   // "D_REF.PT_HEAD"
    };
    struct MotionRef {
        std::string group;
        int index = 0;
    };

    std::unique_ptr<live2d::LAppModel> mModel;
    Framing mFraming = Framing::Bust;
    bool mFramingExplicit = false;
    Fit mFit;
    std::string mModelPath;
    std::vector<HitArea> mHitAreas;
    std::unordered_map<std::string, MotionRef> mMotionsByName;
    std::vector<std::string> mMotionNames;       // index.json order
    std::vector<std::string> mExpressionNames;
    // Parameter values re-asserted every frame, after the runtime has run the
    // motion, the blink and the expression over them. Small by construction:
    // one face's worth.
    std::vector<std::pair<std::string, float>> mPatch;
    int mPad = 8;
    int mBleed = 0;
    int mKeepBelow = 0;

    // Push the solved fit into the runtime: viewport size, scale and offset.
    void apply();
    // Px of artwork below the visible bottom edge at rest, or -1 if unmeasurable.
    int measureBelowEdge() const;
    float mSideFraction = 0.0f;
    bool mLoaded = false;
};

}  // namespace asuna
