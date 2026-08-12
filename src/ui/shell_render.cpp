// Shell, the part that produces pixels: the GL callbacks and the frame
// clock, the framing solver's results turned into a box and a strip height,
// where she stands, and the input region sampled back out of the alpha she
// was drawn with. See ui/shell.hpp for the class this is one piece of.
#include "ui/shell.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "GLCompat.hpp"
#include "ui/shell_internal.hpp"

namespace {

// The silhouette sampler's dial. A cell is `kRegionCell` px square and counts as
// hers if any pixel in it is at least this opaque; the whole readback is rate
// limited so a drag cannot turn into a glReadPixels per frame.
constexpr int kRegionCell = 8;
constexpr int kRegionAlpha = 32;          // out of 255
constexpr gint64 kRegionRefreshUs = 1'000'000;

// Drag speed that produces a full lean, and the most she will lean.
constexpr double kLeanFullSpeed = 600.0;
constexpr float kLeanMax = 0.9f;

}  // namespace

namespace asuna {
// GTK's frame: origin top-left of the strip, logical px. Everything else in
// here works in GL's - origin bottom-left, device px - so this is the one place
// the two are converted, and the widget API is the only consumer.
GdkRectangle Shell::bodyRect() const {
    const int side = mPet.sideBleed();
    int height = mPet.boxHeight() + mMotion.headroom();
    // Her box is glued to the bottom edge, so its top is the strip height less
    // its own. Before the compositor's first configure there is no strip height
    // yet, and the band is where the box starts by construction.
    int top = mBand;
    if (mStripHeight > 0) {
        height = std::min(mStripHeight, height);
        top = mStripHeight - height;
    }
    return {
        static_cast<int>(std::lround(mMotion.placement().x - side)) / mScaleFactor,
        top / mScaleFactor,
        (mPet.boxWidth() + 2 * side) / mScaleFactor,
        height / mScaleFactor,
    };
}

// Her actual outline, not her box. With the gaze halo up, an event can arrive
// from a good way outside her, and the empty corners around her hair are inside
// the box but are not her - a click there should reach the desktop, not make her
// giggle. The silhouette the compositor was last given is exactly the right
// answer, and it is already sitting in memory.
// The tight box around her actual outline, which is a good deal narrower than
// bodyRect(): that one carries the side bleed, ~30% of her box width on each
// side, which is room for her arms to swing into rather than anywhere she
// usually is. Anchoring the menu to it stood the menu a hundred px off her
// shoulder. Falls back to the box before there is a silhouette to read.
GdkRectangle Shell::silhouetteRect() const {
    if (!mBodyRegion || cairo_region_is_empty(mBodyRegion)) return bodyRect();
    cairo_rectangle_int_t ext;
    cairo_region_get_extents(mBodyRegion, &ext);
    // Carried to where she is now. The sampler stands down for the whole of any
    // movement, so a region read before a drag is still the one in memory while
    // she is coasting to a stop after it - and that is exactly when a hand
    // right-clicks her, because letting go and clicking is one gesture. She is
    // then delivered the click by a halo measured from her live box, and
    // overHer() waves it through unconditionally while she is animating, so the
    // menu opens against the one thing in the chain that had not caught up and
    // stands where she used to be.
    //
    // The shape does not need re-reading to fix that: it is the same silhouette,
    // just somewhere else, and where she is is known exactly every frame. So
    // translate it by how far she has travelled since it was read. Squash is
    // ignored deliberately - it peaks at a couple of percent of her height, and
    // paying a frame of latency to re-read the region for that would be a worse
    // menu than one anchored a pixel or two off.
    const Placement p = mMotion.placement();
    ext.x += static_cast<int>(std::lround(p.x - mRegionSampledX)) / mScaleFactor;
    // Up the screen is down in y, and the lift is px above the bottom edge.
    ext.y -= static_cast<int>(std::lround(p.lift - mRegionSampledLift)) / mScaleFactor;
    return {ext.x, ext.y, ext.width, ext.height};
}


void Shell::onRealize(GtkWidget* area, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    gtk_gl_area_make_current(GTK_GL_AREA(area));
    if (GError* err = gtk_gl_area_get_error(GTK_GL_AREA(area))) {
        fprintf(stderr, "asuna: GL area error: %s\n", err->message);
        self->mGlFailed = true;
        return;
    }
    printf("asuna: GL %s | GLSL %s | %s\n", glGetString(GL_VERSION),
           glGetString(GL_SHADING_LANGUAGE_VERSION), glGetString(GL_RENDERER));

    self->mScaleFactor = std::max(1, gtk_widget_get_scale_factor(area));

    // Realized again after a hide and a show: hiding the window destroys its
    // surface and with it the GL context she was uploaded into, so everything
    // has to go up again. It is her *current* outfit that has to come back, not
    // the one the process started in - hide/show is not a way to undo a costume
    // change. The path is copied because load() overwrites the member it lives
    // in before it is finished with it.
    const bool first = !self->mPet.loaded();
    const std::string path = first ? self->mOpt.model : std::string(self->mPet.modelPath());
    if (!self->mPet.load(path)) {
        self->mGlFailed = true;
        return;
    }
    if (first) {
        self->mDialogue.load(Dialogue::defaultPath(self->mOpt.language));
        self->mOutfits = scanOutfits(outfitsRoot(self->mPet.modelPath()));
        self->wireBehaviour();
        self->applyTunables();
    }
    // What this outfit can pull faces with. Re-read on every load, next to the
    // menu that is built from the same two lists, because a costume change can
    // change both.
    self->mBehaviour.setInventory(self->mPet.expressionNames(), self->mPet.motionNames());
    self->rebuildMenu();
    self->applyFraming();
    self->placeInitial();
    self->mPet.startRandomMotion("idle");
}

void Shell::onUnrealize(GtkWidget* area, gpointer) {
    gtk_gl_area_make_current(GTK_GL_AREA(area));
}

gboolean Shell::onRender(GtkGLArea* area, GdkGLContext*, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (self->mGlFailed) return TRUE;

    const int scale = std::max(1, gtk_widget_get_scale_factor(GTK_WIDGET(area)));
    const int w = gtk_widget_get_width(GTK_WIDGET(area)) * scale;
    const int h = gtk_widget_get_height(GTK_WIDGET(area)) * scale;
    if (scale != self->mScaleFactor) {
        // Dragged onto a monitor with a different scale: re-solve at the new
        // device resolution, or she renders at half size (or blurred).
        self->mScaleFactor = scale;
        self->applyFraming();
    }
    if (h != self->mStripHeight) {
        // A resize we asked for has landed. GTK can drop the input region
        // across one (R4), and it was computed against the old height anyway.
        static const bool debug = getenv("ASUNA_DEBUG_SCALE") != nullptr;
        if (debug)
            printf("asuna: strip height %d -> %d (asked for %d)\n",
                   self->mStripHeight / scale, h / scale, self->mRequestedHeight);
        self->mStripHeight = h;
        self->mRegionDirty = true;
        // Deferred, never inline: this runs inside the render pass, and moving
        // a GTK widget there invalidates an allocation GTK is in the middle of
        // snapshotting ("Trying to snapshot GtkLabel without a current
        // allocation"). An idle runs below GDK_PRIORITY_REDRAW, i.e. after the
        // frame that just resized us has been laid out.
        self->schedulePlaceBubble();
    }
    if (w != self->mStripWidth) {
        self->mStripWidth = w;
        if (self->mPlaced) {
            // The strip got narrower - a monitor change or a mode switch. Keep
            // her on screen rather than clipped off the right edge.
            self->mMotion.setBounds(0, w - self->mPet.boxWidth(), self->mPet.boxHeight());
            self->mRegionDirty = true;
        } else {
            self->placeInitial();
        }
    }

    // GTK renders into its own framebuffer; the renderer saves and restores
    // the current binding around its mask passes, so it composes correctly.
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Two things aim her head, and they simply add. Her own velocity is the
    // lean - no decay of our own, because the spring's velocity already falls to
    // zero as she settles - and on top of it sits where she is looking. Summing
    // rather than switching is what keeps the handover smooth: as a throw
    // settles the velocity term fades out from under a gaze that was already
    // there, with nothing to snap.
    const double leanX = self->mMotion.velocityX() / kLeanFullSpeed + self->mBehaviour.gazeX();
    const double leanY = self->mMotion.liftVelocity() / kLeanFullSpeed + self->mBehaviour.gazeY();
    self->mPet.setLean(
        static_cast<float>(std::clamp(leanX, -(double)kLeanMax, (double)kLeanMax)),
        static_cast<float>(std::clamp(leanY, -(double)kLeanMax, (double)kLeanMax)));

    // The gaze eases on the render clock, not the tick clock, because that is
    // where it is advanced. The two differ whenever a cap is in force, and a
    // cap is exactly the case that used to make her head chase the cursor at a
    // different speed. Clamped because a hide, a monitor change or a stalled
    // compositor can leave an arbitrarily long gap, and the easing should
    // resume from where she was rather than teleport.
    const gint64 renderNow =
        gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(GTK_WIDGET(area)));
    const float renderDt =
        self->mLastRenderUs
            ? std::min(0.1f, (float)(renderNow - self->mLastRenderUs) / 1e6f)
            : 1.0f / 60.0f;
    self->mLastRenderUs = renderNow;

    self->mPet.update(renderDt);
    self->mPet.draw(self->mMotion.placement());

    // Must follow the draw: the silhouette is read back out of it.
    self->sampleSilhouette();
    return TRUE;
}

void Shell::onMap(GtkWidget*, gpointer data) {
    static_cast<Shell*>(data)->applyBoxRegion();
}

gboolean Shell::onTick(GtkWidget* widget, GdkFrameClock* clock, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // The callback is taken off entirely while she is hidden - unmapping the
    // window does not stop the frame clock, which is the whole reason that is
    // done. This is the guard for the frame already in flight when it happens.
    if (self->mHidden) return G_SOURCE_CONTINUE;
    const gint64 now = gdk_frame_clock_get_frame_time(clock);

    // The motion model advances on wall-clock time at its own fixed rate,
    // whether or not this frame gets drawn, so throttling never changes where
    // she ends up - only how often you see it.
    const float dt = self->mLastTickUs ? (now - self->mLastTickUs) / 1e6f : 0.0f;
    self->mLastTickUs = now;
    self->mMotion.advance(dt);
    // The idle scheduler, the chatter timer, the sleep countdown and the gaze
    // smoothing all live here, on the same clock and for the same reason: they
    // must not speed up or slow down with the frame rate.
    self->mBehaviour.advance(dt, self->mPet.motionFinished());
    // Dozing off is the one thing she does entirely on her own that something
    // else might want to know about - a helper has no business chatting to
    // someone who is asleep. Published on the edge, not the level: this runs
    // thirty times a second.
    if (self->mBehaviour.asleep() != self->mWasAsleep) {
        self->mWasAsleep = self->mBehaviour.asleep();
        self->publish("sleep", ipc::Out().boolean("asleep", self->mWasAsleep).done());
    }
    if (self->mDragging || self->mMotion.animating()) {
        self->placeBubble();
        // A halo computed where she used to be is worse than none: it would
        // keep claiming the old patch of desktop and never be corrected, since
        // the silhouette sampler stands down while the halo is up. Wayland's
        // implicit grab carries a drag regardless of the region, so nothing is
        // lost by dropping it - the next motion over her puts it back.
        self->leaveHalo();
    }

    // The tick fires at the display refresh rate. Recompositing a full-width
    // surface 144 times a second for an idle animation nobody is looking that
    // hard at is waste, so throttle - but never while she is actually moving. A
    // drag updated at 30 fps is exactly the stutter it looks like.
    //
    // Throttling costs more than the frames it drops, and it has to be worth it.
    //
    // On Wayland the frame clock is paced by the compositor's frame callbacks,
    // and a callback only comes back for a surface that actually committed a
    // buffer. Skip a paint and the clock has nothing pacing it, so it falls off
    // vblank onto its own timer - not just slower but *uneven*. Measured on a
    // 144 Hz panel, idle: uncapped, ticks arrive every 6.95 ms with a standard
    // deviation of 0.27 ms; capped, the same clock wanders to 8.2 ms +/- 3.0.
    // The renders inherit it, and so does everything sampled per render.
    //
    // Which is why her gaze looked fine while her breathing did not. The gaze
    // is advanced a fixed step per rendered frame, so uneven frames are
    // invisible in it - every frame moves her head the same amount. Her breath
    // and her idle motions are sampled from the wall clock, so an uneven frame
    // is an uneven step, and that is the hitch.
    //
    // So: a cap in the top half of the refresh rate is refused. It cannot be
    // paced evenly up there - 90 on this panel measured 43.8 fps with 8 ms of
    // jitter, and even an exact half, 72, came out at 45 and uneven - and it is
    // saving at most half the frames of something already cheap enough to leave
    // running. Below half it is left alone: the default 30 measures 24 fps at
    // 0.5 ms of jitter, which is the even cadence that reads as calm.
    if (self->mOpt.fps > 0 && !self->mMotion.animating()) {
        gint64 refresh = 0;
        gdk_frame_clock_get_refresh_info(clock, now, &refresh, nullptr);
        if (refresh <= 0) refresh = G_USEC_PER_SEC / 60;
        const gint64 interval = G_USEC_PER_SEC / self->mOpt.fps;
        if (interval >= 2 * refresh && now - self->mLastFrameUs < interval)
            return G_SOURCE_CONTINUE;
    }
    self->mLastFrameUs = now;
    gtk_gl_area_queue_render(GTK_GL_AREA(widget));
    return G_SOURCE_CONTINUE;
}

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
    // ui/shell_debug.cpp, which is where all of it lives.
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

// The region as it would be with nothing claiming it - her outline, or her box
// before there is an outline to read. Kept because it is also the answer to "is
// the pointer on her", which the halo makes a real question, and because the
// halo and the menu hand the region back to it when they are done.
void Shell::setBodyRegion(const cairo_rectangle_int_t* rects, int count) {
    if (mBodyRegion) cairo_region_destroy(mBodyRegion);
    mBodyRegion = cairo_region_create_rectangles(rects, count);
    // Stamped here rather than at the call sites so the shape and the position
    // it was taken at can never be recorded apart.
    mRegionSampledX = mMotion.placement().x;
    mRegionSampledLift = mMotion.placement().lift;
    applyRegion(rects, count);
}

void Shell::applyRegion(const cairo_rectangle_int_t* rects, int count) {
    if (!mWindow) return;
    GtkNative* native = gtk_widget_get_native(mWindow);
    if (!native) return;
    GdkSurface* surface = gtk_native_get_surface(native);
    if (!surface) return;

    cairo_region_t* region = cairo_region_create_rectangles(rects, count);
    gdk_surface_set_input_region(surface, region);
    cairo_region_destroy(region);
}

void Shell::applyBoxRegion() {
    // Her whole bounding box, which starts below the bubble band. Only used
    // before the first frame has been drawn, since until then there is no alpha
    // to read a silhouette out of.
    const GdkRectangle b = bodyRect();
    const cairo_rectangle_int_t rect = {b.x, b.y, b.width, b.height};
    setBodyRegion(&rect, 1);
}

// Reads back the alpha we just rendered and hands the compositor the cells she
// actually covers.
//
// The bounding box was a ~300x460 dead zone on the top layer: her outline fills
// barely half of it, so clicks and scrolls died in the empty corners around her
// hair. Sampling the rendered alpha is exact by construction - it includes hair,
// ribbons and whatever a new outfit adds - and it costs one readback per second
// rather than per frame.
//
// The bias is deliberately outward: a whole cell counts as hers if any pixel in
// it is even faintly opaque, so an antialiased hair edge stays grabbable.
void Shell::sampleSilhouette() {
    if (!mPlaced || mStripHeight <= 0) return;

    // Never while she is moving. Wayland's implicit grab keeps the pointer
    // coming to us throughout a drag regardless, and any region sampled in
    // flight is wrong by the next frame - including during the fall and landing
    // after release. Instead, take one sample the moment she comes to rest.
    const bool moving = mDragging || mMotion.animating();
    // Three separate things can hold this sampler off, and a region that goes
    // stale is invisible until something reads it and answers about where she
    // used to be. ASUNA_DEBUG_SAMPLER=1 prints which one is holding it and how
    // old the region already is.
    static const bool debugSampler = getenv("ASUNA_DEBUG_SAMPLER") != nullptr;
    if (debugSampler) {
        static gint64 last = 0;
        const gint64 t = g_get_monotonic_time();
        if (t - last > 500000) {
            last = t;
            printf("asuna: sampler moving=%d halo=%d menu=%d dirty=%d age=%.2fs\n",
                   moving, mHalo, mMenuOpen, mRegionDirty,
                   (t - mRegionSampledUs) / 1e6);
        }
    }
    if (mWasMoving && !moving) {
        mRegionDirty = true;
        // And the halo's claim on the region expires here, because it was
        // entered against an outline she has since walked out of. Nothing else
        // would ever drop it: onTick only does so while she is moving, and once
        // she is still the halo stays up until the pointer leaves - which it
        // does not, because the hand that just dragged her is still on her. The
        // sampler stands down the whole time, so our copy of her silhouette
        // stays frozen at her old position, and everything that reads it
        // (the menu's anchor, overHer()) answers about where she used to be.
        // The next motion event puts the halo back, measured from where she is.
        leaveHalo();
    }
    mWasMoving = moving;
    if (moving) return;
    // The menu and the gaze halo each own the region while they are up;
    // re-reading it now would take their claim away from underneath them.
    if (mMenuOpen || mHalo) return;

    const gint64 now = g_get_monotonic_time();
    if (!mRegionDirty && now - mRegionSampledUs < kRegionRefreshUs) return;
    mRegionDirty = false;
    mRegionSampledUs = now;

    // Everything she is drawn into - box plus side bleed - widened by one cell
    // so a stretch or a swaying sleeve cannot fall outside the window we read.
    const int side = mPet.sideBleed() + kRegionCell;
    const int x0 = std::max(0, static_cast<int>(mMotion.placement().x) - side);
    const int x1 = std::min(mStripWidth,
                            static_cast<int>(mMotion.placement().x) + mPet.boxWidth() + side);
    const int w = x1 - x0;
    // Only the part of the strip she can ever occupy. The bubble band above it
    // is GTK's, not GL's, so there is nothing in those rows to find - and at
    // 1920 px wide they are not free to scan.
    const int h = std::min(mStripHeight, mPet.boxHeight() + mMotion.headroom());
    if (w <= 0 || h <= 0) return;

    mPixels.resize(static_cast<size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, mPixels.data());
    const gint64 readDone = g_get_monotonic_time();

    // Bin into cells and merge along each row, so the compositor gets a handful
    // of rectangles to walk per pointer event instead of thousands.
    std::vector<cairo_rectangle_int_t> rects;
    const int cell = kRegionCell;
    for (int y = 0; y < h; y += cell) {
        const int rows = std::min(cell, h - y);
        int runStart = -1;
        for (int x = 0; x <= w; x += cell) {
            bool occupied = false;
            if (x < w) {
                const int cols = std::min(cell, w - x);
                for (int py = y; py < y + rows && !occupied; ++py) {
                    const unsigned char* row = mPixels.data() + (static_cast<size_t>(py) * w + x) * 4;
                    for (int px = 0; px < cols; ++px) {
                        if (row[px * 4 + 3] > kRegionAlpha) { occupied = true; break; }
                    }
                }
            }
            if (occupied) {
                if (runStart < 0) runStart = x;
            } else if (runStart >= 0) {
                // glReadPixels rows start at the bottom of the strip; surface
                // rows at the top of it.
                const int top = mStripHeight - y - rows;
                rects.push_back({(x0 + runStart) / mScaleFactor, top / mScaleFactor,
                                 (x - runStart) / mScaleFactor, rows / mScaleFactor});
                runStart = -1;
            }
        }
    }

    // Nothing opaque at all - she is mid-load or fully faded. Keep whatever
    // region is already set rather than making her unclickable.
    if (rects.empty()) return;
    setBodyRegion(rects.data(), static_cast<int>(rects.size()));

    static const bool debug = getenv("ASUNA_DEBUG_REGION") != nullptr;
    if (debug) {
        long area = 0;
        int top = mStripHeight, left = mStripWidth, right = 0;
        for (const auto& r : rects) {
            area += static_cast<long>(r.width) * r.height;
            top = std::min(top, r.y);
            left = std::min(left, r.x);
            right = std::max(right, r.x + r.width);
        }
        printf("asuna: silhouette top=%d left=%d right=%d lift=%.1f scaleY=%.3f\n",
               top, left, right, mMotion.placement().lift, mMotion.placement().scaleY);
        const long box = static_cast<long>((mPet.boxWidth() + 2 * mPet.sideBleed()) /
                                           mScaleFactor) *
                         ((mPet.boxHeight() + mMotion.headroom()) / mScaleFactor);
        printf("asuna: region %zu rects, %ld px = %.1f%% of her bounding box "
               "(%.2f ms: %.2f read, %.2f bin)\n",
               rects.size(), area, box ? 100.0 * area / box : 0.0,
               (g_get_monotonic_time() - now) / 1000.0,
               (readDone - now) / 1000.0,
               (g_get_monotonic_time() - readDone) / 1000.0);
    }
}

}  // namespace asuna
