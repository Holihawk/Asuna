// Shell, the part that produces pixels: the GL callbacks and the frame
// clock. Layout and input-region sampling have their own focused units; see
// ui/shell.hpp for the class this is one piece of.
#include "ui/shell.hpp"

#include <gtk4-layer-shell.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "GLCompat.hpp"
#include "ui/shell/internal.hpp"

namespace {

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

}  // namespace asuna
