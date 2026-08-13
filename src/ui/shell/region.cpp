// Shell input region: retain and apply the clickable outline, and periodically
// resample it from the rendered alpha while she is stationary.
#include "ui/shell.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "GLCompat.hpp"

namespace {

// The silhouette sampler's dial. A cell is `kRegionCell` px square and counts as
// hers if any pixel in it is at least this opaque; the whole readback is rate
// limited so a drag cannot turn into a glReadPixels per frame.
constexpr int kRegionCell = 8;
constexpr int kRegionAlpha = 32;          // out of 255
constexpr gint64 kRegionRefreshUs = 1'000'000;

}  // namespace

namespace asuna {

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
