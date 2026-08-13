// Shell layout: solve the model framing and strip height, place her within it,
// and apply user scale without changing the render or input-region mechanics.
#include "ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "ui/shell/internal.hpp"

namespace asuna {

void Shell::applyFraming() {
    // Solving a fit is not pure arithmetic any more: it renders her once to
    // measure how far her artwork reaches below the crop (see Pet::fit). Most
    // callers here are outside the render pass - a costume change, a monitor
    // change, a config reload - and outside it the GL area's context is only
    // current by luck. Ask for it explicitly rather than inherit whatever GTK
    // last left bound, or the probe draws into nothing and reads back zero.
    if (mArea && gtk_widget_get_realized(mArea))
        gtk_gl_area_make_current(GTK_GL_AREA(mArea));

    // The model's own geometry decides both how wide her box has to be and how
    // tall the strip needs to be for her to stay the same apparent size; nothing
    // here is tuned to a particular outfit. The user's scale multiplies both,
    // and re-solving (rather than scaling the drawn result) is what keeps her
    // sharp at any size.
    //
    // The band is whichever is larger: what the user asked for, or what the
    // bubble needs for the rows they asked for. Left to `strip.bubble_band`
    // alone, `ext.bubble_rows` was a promise the strip could not keep - the
    // extra rows were drawn above the top edge, where there is no surface.
    mBand = std::max(mOpt.bubbleBand, mBubble.bandFor(mOpt.ext.bubbleRows)) * mScaleFactor;

    const int base = static_cast<int>(std::lround(mOpt.stripHeight * mUserScale)) * mScaleFactor;
    const int asked = static_cast<int>(std::lround(mOpt.maxHeight * mUserScale)) * mScaleFactor;
    int screen = 0;
    if (mMonitor) {
        // A strip taller than the screen would be anchored off the top edge and
        // cost a full-screen recomposite every frame. So it has to fit in what
        // is between the bottom margin and the top of the screen, less the band,
        // which comes off whole because it is reserved permanently.
        //
        // The 2% is only so it never quite reaches the top edge; the bottom
        // margin is subtracted for the same reason the band is. Nothing else
        // here decides her ceiling - the band does, and `ext.bubble_rows` sets
        // the band: eight rows of reply reserve 200 px of screen whether or not
        // she is speaking, and that is 200 px she cannot be scrolled up into.
        GdkRectangle geom;
        gdk_monitor_get_geometry(mMonitor, &geom);
        const int usable = std::max(0, geom.height - mOpt.bottomMargin);
        screen = static_cast<int>(usable * 0.98) * mScaleFactor - mBand;
    }
    int cap = screen > 0 ? std::min(asked, screen) : asked;
    cap = std::max(cap, mScaleFactor);
    // The strip carries her box plus the headroom the lift and the stretch need,
    // so the cap has to be shared out between them.
    const double withHeadroom =
        1.0 + mMotion.tuning().maxLift + mMotion.tuning().liftStretch;
    const int budget = std::min(base, static_cast<int>(cap / withHeadroom));

    // ...and the scale at which the screen, rather than the request, is what
    // decides her size. Past it nothing moves, so the wheel has to stop there.
    //
    // Letting `mUserScale` run on above it looks harmless and is not: it banks
    // scale she is not wearing. Scrolled up to the ceiling and then further, she
    // stops growing but the number keeps climbing to 2.5, and the way back down
    // has to spend every notch of that before she shrinks by a pixel - while at
    // the bottom, where the clamp is a plain constant, one notch up always moves
    // her. That asymmetry is the whole bug; the wasted notches also re-solved the
    // framing and reset the idle animation each time, which is the stutter that
    // came with them.
    //
    // Solved rather than searched, because everything it needs is linear in the
    // scale and already in hand here. Her box is
    //   min(base x r, screen / headroom, asked / headroom)
    // where r is what preferredBoxHeight() makes of a full-body outfit (1 for a
    // bust). `base` and `asked` are proportional to the scale and `screen` is
    // not, so each of the two screen-side terms gives a scale at which it takes
    // over, and the ceiling is the later of them. Asking Pet for the height it
    // would prefer with no ceiling at all is what keeps r out of here: it is the
    // same call the fit below makes, handed infinity instead of a limit.
    mScaleCeiling = kMaxUserScale;
    if (screen > 0 && mUserScale > 0) {
        const double wanted =
            mPet.preferredBoxHeight(base, std::numeric_limits<int>::max());
        double ceiling = 0.0;
        if (wanted > 0) ceiling = mUserScale * (screen / withHeadroom) / wanted;
        if (asked > 0)
            ceiling = std::max(ceiling, mUserScale * screen / static_cast<double>(asked));
        mScaleCeiling = std::clamp(static_cast<float>(ceiling), kMinUserScale, kMaxUserScale);
    }
    // Nothing is re-solved for the clamped scale: at and above the ceiling the
    // arithmetic above lands on the same box, which is what makes it the ceiling.
    if (mUserScale > mScaleCeiling) mUserScale = mScaleCeiling;

    // How much body to keep ready below the screen edge. A crop cuts her
    // somewhere, and lifting her turns that cut into a blunt slice hanging in
    // mid-air; rendering past it means the lift reveals more of her instead, and
    // the cut travels down off the screen edge with the body it belongs to.
    //
    // Both framings need this, not just the bust. A full-body crop is not her
    // feet: measureTarget() clamps the target to the canvas, and every one of
    // the 14 full-body outfits has its figure running past the canvas bottom, so
    // the crop lands mid-calf. There are no feet in the artwork to preserve -
    // the legs stop at a flat horizontal cut of the artist's own, because the
    // widget these models come from always framed them against the canvas edge -
    // but there is 48-135 px of leg below the crop on an 800 px box, measured
    // with asuna-render-test --below. The lift is 72 px at that size, so for 11
    // of the 14 the cut stays at or below the screen edge for the whole lift and
    // is simply never seen. The three yukata outfits (asuna_36/37/38) have only
    // 49 px and show the last 23 px of a hem that is drawn flat anyway.
    const auto bleedFor = [&](int height) {
        return static_cast<int>(std::ceil(
            height * (mMotion.tuning().maxLift + mMotion.tuning().liftStretch)));
    };

    // ...and how much of it has to be real artwork rather than reserved room.
    //
    // At full lift her bottom-most pixel sits at `lift - bleed + (bleed - below)
    // * stretch` relative to the screen edge, where `below` is what the outfit
    // actually draws under that edge. Setting that to zero and solving for
    // `below` is the least artwork that keeps her hem on the edge for the whole
    // drag. Pet::fit measures the real figure and makes up any shortfall by
    // sitting her lower - so the yukata outfits, which are 16 px short of this,
    // stop lifting off into a transparent gap, and asuna_39 and 43, which are
    // already over it, are left exactly where they were.
    //
    // Plus a margin, because the exact answer does not look like the right one.
    // Solved bare, her hem arrives at the screen edge at the top of the lift and
    // stops there - technically no gap, but the eye reads a line landing exactly
    // on an edge as having only just made it. A few px of overlap reads as her
    // continuing past the edge, which is the thing being suggested. A fraction
    // rather than px so it survives a resize: 0.6% is 4 px at the 681 px box a
    // full-body outfit gets by default.
    constexpr double kLiftMargin = 0.006;
    const auto keepBelowFor = [&](int height) {
        const double bleed = bleedFor(height);
        const double lift = height * mMotion.tuning().maxLift;
        const double stretch = 1.0 + mMotion.tuning().liftStretch;
        return static_cast<int>(
            std::ceil(bleed - (bleed - lift) / stretch + kLiftMargin * height));
    };

    const int pad = mOpt.pad * mScaleFactor;
    mPet.fit(budget, pad, bleedFor(budget), mOpt.sideBleed, keepBelowFor(budget));
    const int want = mPet.preferredBoxHeight(budget, static_cast<int>(cap / withHeadroom));
    if (want != mPet.boxHeight())
        mPet.fit(want, pad, bleedFor(want), mOpt.sideBleed, keepBelowFor(want));

    mMotion.setBounds(0, std::max(0, mStripWidth - mPet.boxWidth()), mPet.boxHeight());

    // Layer surfaces resize by asking: the compositor answers with a configure
    // and the GL area follows. Only ask when it actually changed - a redundant
    // request still costs a round trip and a relayout.
    //
    // The band for the speech bubble is part of the strip permanently rather
    // than something the window grows into when she speaks: a layer-surface
    // resize is a compositor round trip that can drop the input region on the
    // way through (R4), and she is anchored to the bottom edge, so carrying the
    // band changes nothing about where she stands.
    const int height = (mPet.boxHeight() + mMotion.headroom() + mBand) / mScaleFactor;
    if (height != mRequestedHeight) {
        mRequestedHeight = height;
        // Height only. A surface anchored to both side edges takes its width
        // from the compositor, and asking for one anyway makes GTK renegotiate
        // the toplevel size far more often - measurably worse, not better.
        gtk_widget_set_size_request(mWindow, -1, height);
        gtk_window_set_default_size(GTK_WINDOW(mWindow), -1, height);
    }
    mRegionDirty = true;
    // A resize moves and reshapes her under a halo that was measured around her
    // old box, and scroll-to-resize happens with the pointer on her, so the halo
    // is always up for it. Same expiry as the settle edge above.
    leaveHalo();

    // Logged here rather than in Pet::fit: this is the point at which the
    // framing is final, so there is one line per outfit or size change.
    const Fit& f = mPet.fitResult();
    printf("asuna: framing=%s target=(%.0f,%.0f)-(%.0f,%.0f) box=%dx%d bleed=%d+%d "
           "drop=%d zoom=%.3f strip=%d(want %d) ceiling=%.2f\n",
           framingName(mPet.framing()), f.target[0], f.target[1], f.target[2],
           f.target[3], f.boxWidth, f.boxHeight, f.bleed, f.sideBleed, f.drop, f.scale,
           mStripHeight / mScaleFactor, mRequestedHeight, mScaleCeiling);
}

double Shell::anchorXFor(const std::string& where) const {
    const int box = mPet.boxWidth();
    if (where == "left") return mOpt.margin * mScaleFactor;
    if (where == "centre" || where == "center") return (mStripWidth - box) / 2.0;
    return mStripWidth - box - mOpt.margin * mScaleFactor;   // bottom-right
}

double Shell::anchorX() const { return anchorXFor(mOpt.anchor); }

void Shell::placeInitial() {
    // Needs both halves of the puzzle: the strip width from the compositor and
    // the box width from the framing solver. Whichever arrives second calls in.
    if (mPlaced || mStripWidth <= 0 || !mPet.loaded()) return;
    mPlaced = true;

    const int box = mPet.boxWidth();
    double x;
    if (mOpt.x >= 0)
        x = mOpt.x;
    else if (mState.x >= 0)
        x = mState.x;              // where she was left last time
    else
        x = anchorX();
    mMotion.setBounds(0, std::max(0, mStripWidth - box), mPet.boxHeight());
    mMotion.reset(clampX(x));
    applyBoxRegion();
    placeBubble();
    // Synthetic input, an environment variable at a time - see
    // ui/shell/debug.cpp, which is where all of it lives.
    installDebugHooks();
    // The greeting is the shell's, not Behaviour's: a one-off tied to the wall
    // clock rather than anything the state machine schedules.
    //
    // Deferred by a moment rather than said here. This runs inside the first
    // frame, which on this compositor arrives ~1.9 s after launch with the
    // initial configure - and GTK has not laid out the overlay yet at that
    // point, so a bubble asked for now is snapshotted before it has an
    // allocation. Waiting a beat also reads better: she appears, then speaks.
    if (mOpt.greet) g_timeout_add(kGreetDelayMs, onGreetTimeout, this);
    printf("asuna: strip=%dpx x=%.0f box=%dx%d headroom=%d scale=%.2f%s\n",
           mStripWidth, mMotion.placement().x, mPet.boxWidth(), mPet.boxHeight(),
           mMotion.headroom(), mUserScale,
           (mOpt.x < 0 && mState.x >= 0) ? " (restored)" : "");
}

double Shell::clampX(double x) const {
    const double limit = std::max(0.0, static_cast<double>(mStripWidth - mPet.boxWidth()));
    return std::clamp(x, 0.0, limit);
}

void Shell::setUserScale(float scale) {
    // The ceiling is the screen's, worked out by the last framing (see there);
    // it is the smaller of the two limits whenever the screen runs out before
    // 2.5x does, which on a 1080p screen is most of the way through the range.
    scale = std::clamp(scale, kMinUserScale, std::min(kMaxUserScale, mScaleCeiling));
    if (std::fabs(scale - mUserScale) < 0.001f) return;
    // Scrolling mid-drag re-solves the box under the gesture, which would leave
    // the grab anchored to a position that no longer exists. Put her down first;
    // the pointer keeps its grab and the next press picks her up again.
    endDrag();
    mUserScale = scale;

    // Resize about her centre, so she grows in place instead of sliding out
    // from under the pointer.
    const double centre = mMotion.placement().x + mPet.boxWidth() / 2.0;
    applyFraming();
    mMotion.reset(clampX(centre - mPet.boxWidth() / 2.0));
    schedulePlaceBubble();
    scheduleSave();
    printf("asuna: scale=%.2f box=%dx%d\n", mUserScale, mPet.boxWidth(),
           mPet.boxHeight());
}

}  // namespace asuna
