// Shell, the part the pointer talks to: the drag that carries her along the
// bottom edge, the tap that does not become one, the gaze, the halo that
// widens her input region while she is being touched, and the two gestures
// that are neither - the wheel and the right-click menu.
#include "ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Pointer travel that turns a press into a drag rather than a tap.
constexpr double kDragThreshold = 4.0;

// One scroll notch, as a multiplier on her scale.
constexpr float kScaleStep = 1.06f;

// How far from her centre the cursor has to be for a full-strength look, as a
// fraction of her box. Smaller than her box on purpose: a gaze that only reaches
// its extreme at the very edge of her never looks like it is following anything.
constexpr double kGazeSpan = 0.5;

// How long the halo outlives the last pointer motion. Re-armed by every one, so
// this is a stillness timer and not a lifetime: a cursor crossing the halo keeps
// being followed for as long as it keeps moving, and one that stops has 350 ms
// before the desktop underneath it is handed back.
//
// The number is the whole compromise. Wayland gives a surface one region for
// both "deliver me motion" and "clicks here are mine", so every millisecond of
// gaze range past her outline is a millisecond of dead zone; a hand that stops
// moving is about to click something, and 350 ms is comfortably shorter than
// the gap between a hand arriving somewhere and pressing the button.
constexpr guint kHaloLingerMs = 350;

}  // namespace

namespace asuna {
void Shell::aimGaze(double x, double y) {
    if (!mPlaced || !mPet.loaded()) return;

    // Her head, not her box: the gaze should pivot on her face, and on a bust
    // crop the two are nowhere near each other.
    float hx = 0, hy = 0;
    if (!mPet.headCentre(&hx, &hy)) return;
    double headX = 0, headY = 0;
    if (!mPet.modelToScreen(mMotion.placement(), hx, hy, &headX, &headY)) return;

    // GL's frame: device px, origin at the bottom-left of the strip.
    const double px = x * mScaleFactor;
    const double py = mStripHeight - y * mScaleFactor;

    const double spanX = std::max(1.0, mPet.boxWidth() * kGazeSpan);
    const double spanY = std::max(1.0, mPet.boxHeight() * kGazeSpan);
    mBehaviour.lookAt(std::clamp((px - headX) / spanX, -1.0, 1.0),
                      std::clamp((py - headY) / spanY, -1.0, 1.0));
}

// Deferred to the first right-click: 42 outfits, 10 expressions and 19 motions
// become seventy-odd menu items across six popover surfaces, and most sessions
// never open the menu at all. Worth about 2 MB of RSS - measured, and less than
// expected, but it also keeps the outfit list from going stale between opens.
void Shell::rebuildMenu() { mMenuStale = true; }

void Shell::openMenu() {
    if (mMenuStale) {
        mMenuStale = false;
        mMenu.rebuild(mOutfits, outfitId(mPet.modelPath()), mPet.expressionNames(),
                      mPet.motionNames(), listening());
    }
    mMenu.popupBeside(silhouetteRect());
}

// GTK's frame: origin top-left of the strip, logical px. Everything else in
// here works in GL's - origin bottom-left, device px - so this is the one place

bool Shell::overHer(double x, double y) const {
    // While she is moving the sampler stands down, so our copy of the region is
    // one position out of date - but so is the compositor's, and it is the
    // compositor's that decided this event reaches us at all. Trust it: the halo
    // is dropped for the duration of any movement, so nothing but her outline
    // can be delivering events right now.
    if (mDragging || mMotion.animating()) return true;
    if (!mBodyRegion) return false;
    return cairo_region_contains_point(mBodyRegion, static_cast<int>(x),
                                       static_cast<int>(y));
}

// --- dragging -------------------------------------------------------------
//
// She only moves along the bottom edge, so a drag is one axis: the vertical
// offset is ignored. No window is moved - Wayland would not allow it - the box
// simply gets a new x inside the strip, which is also why dragging costs
// nothing beyond the frame that was going to be drawn anyway.

void Shell::onDragBegin(GtkGestureDrag*, double x, double y, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // Inside the gaze halo but not on her: the user is clicking the desktop, not
    // picking her up. Drop the halo so the click they are about to repeat lands
    // where they meant it to.
    if (!self->overHer(x, y)) {
        self->leaveHalo();
        return;
    }
    self->mDragging = true;
    self->mDragMoved = false;
    self->mDragOriginX = self->mMotion.placement().x;
    // Kept for the tap case: a press that never passes the drag threshold is a
    // poke, and this is where it landed.
    self->mPressX = x;
    self->mPressY = y;
}

void Shell::onDragUpdate(GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (!self->mDragging) return;   // the press was not on her

    if (!self->mDragMoved) {
        if (std::fabs(dx) < kDragThreshold && std::fabs(dy) < kDragThreshold) return;
        self->mDragMoved = true;
        self->setCursor("grabbing");
        // Only now does she leave the ground: a press that never became a drag
        // must not bob her.
        self->mMotion.beginDrag();
        // Behaviour owns her face and her voice, here as everywhere else, so a
        // grab and a poke cannot end up disagreeing about what she is doing.
        self->mBehaviour.dragBegin();
        self->cancelGlance("picked up");
        self->publish("drag", ipc::Out().str("phase", "begin").done());
    }

    // She is not pinned to the pointer - this only moves the target the spring
    // is chasing. The gap between the two is what her velocity, her lean and
    // her hair sway all come out of.
    self->mMotion.hold(self->mDragOriginX + dx * self->mScaleFactor,
                       static_cast<float>(-dy * self->mScaleFactor));

    // No input-region juggling here: Wayland's implicit pointer grab keeps
    // delivering motion to us until the button comes up, however far she moves.
    self->scheduleSave();
}

void Shell::onDragEnd(GtkGestureDrag*, double, double, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->endDrag();
}

void Shell::onDragCancel(GtkGesture*, GdkEventSequence*, gpointer data) {
    static_cast<Shell*>(data)->endDrag();
}

void Shell::endDrag() {
    if (!mDragging) return;
    mDragging = false;
    setCursor("grab");

    if (!mDragMoved) {
        poke(mPressX, mPressY);
        return;
    }

    // Lets go: she keeps her horizontal momentum and falls the last few px back
    // onto the screen edge, landing with a squash proportional to the drop.
    mMotion.release();
    mBehaviour.dragEnd();
    mRegionDirty = true;
    scheduleSave();
    publish("drag", ipc::Out().str("phase", "end").done());
}

// A tap that never became a drag. Which part of her it landed on decides the
// reaction, so the press point goes back through the exact transform she was
// drawn with this frame. The press was already checked against her outline in
// onDragBegin, which is the only non-debug way in here.
void Shell::poke(double x, double y) {
    const double px = x * mScaleFactor;
    const double py = mStripHeight - y * mScaleFactor;
    const std::string area = mPet.hitAreaAt(mMotion.placement(), px, py);
    static const bool debug = getenv("ASUNA_DEBUG_TOUCH") != nullptr;
    if (debug)
        printf("asuna: poke at (%.0f,%.0f) logical -> '%s'\n", x, y,
               area.empty() ? "(no area)" : area.c_str());
    // Touching her is how a glance she has announced is called off. Before the
    // reaction, so the poke that stops it is not also a poke that makes her
    // giggle about it.
    cancelGlance("poked");
    mBehaviour.touch(area);
    publish("touch", ipc::Out().str("area", area).done());
}

void Shell::onPointerMotion(GtkEventControllerMotion*, double x, double y,
                            gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->mPointerX = x;
    self->mPointerY = y;
    // Entered on her outline, kept alive by a cursor that keeps moving. Motion
    // only arrives here at all because the region already covers this point, so
    // "on her" is the entry condition and the halo is what extends it.
    const bool onHer = self->overHer(x, y);
    if (onHer) self->enterHalo();
    self->refreshHalo(onHer);
    self->setCursor(onHer ? "grab" : "default");
    self->aimGaze(x, y);
}

void Shell::onPointerLeave(GtkEventControllerMotion*, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->leaveHalo();
    self->mBehaviour.lookAway();
}

// The halo is the answer to "follow the cursor further out without making a dead
// zone of the desktop around her". Wayland delivers pointer motion only inside
// our input region, so range and dead zone are the same knob - unless the region
// is only large while she is already being touched, which is what this does.
//
// The cost is bounded on three sides: it can only ever be entered by a pointer
// that is on her; it expires kHaloLingerMs after the pointer stops moving; and a
// press that is not on her drops it immediately, so a click meant for the window
// underneath goes through on the second try rather than being swallowed
// indefinitely.
//
// The middle one is the one that was missing. Without it the halo lasted as long
// as the pointer stayed inside it - which, since the halo is a 160 px band drawn
// around wherever the pointer already is, is "until the user moves the mouse
// somewhere else entirely". A cursor left at rest beside her held a rectangle of
// desktop hostage for as long as it sat there, hand cursor and all.
void Shell::enterHalo() {
    if (mHalo || mOpt.gazeHalo <= 0 || mMenuOpen) return;
    mHalo = true;
    // The region only reaches the compositor on the next commit, so ask for a
    // frame now: a pointer that leaves her outline before then simply does not
    // get the halo this time round.
    if (mArea) gtk_gl_area_queue_render(GTK_GL_AREA(mArea));
    // Measured from her box, not from bodyRect(): the side bleed is room for
    // her arms to swing into, mostly empty, and counting it would silently make
    // the halo half as wide again as the number the user asked for.
    GdkRectangle b = bodyRect();
    const int side = mPet.sideBleed() / mScaleFactor;
    b.x += side;
    b.width = std::max(1, b.width - 2 * side);
    const int halo = mOpt.gazeHalo;
    const int stripW = mStripWidth / mScaleFactor;
    const int stripH = mStripHeight / mScaleFactor;
    const int x = std::max(0, b.x - halo);
    const int y = std::max(0, b.y - halo);
    const cairo_rectangle_int_t rect = {x, y,
                                        std::min(stripW - x, b.width + 2 * halo),
                                        std::min(stripH - y, b.height + 2 * halo)};
    applyRegion(&rect, 1);

    static const bool debug = getenv("ASUNA_DEBUG_REGION") != nullptr;
    if (debug)
        printf("asuna: halo on, region %dx%d at (%d,%d) (body %dx%d at (%d,%d))\n",
               rect.width, rect.height, rect.x, rect.y, b.width, b.height, b.x, b.y);
}

void Shell::leaveHalo() {
    if (mHaloTimer) {
        g_source_remove(mHaloTimer);
        mHaloTimer = 0;
    }
    if (!mHalo) return;
    mHalo = false;
    mRegionDirty = true;   // the next frame reads her silhouette back
    static const bool debug = getenv("ASUNA_DEBUG_REGION") != nullptr;
    if (debug) printf("asuna: halo off\n");
}

void Shell::refreshHalo(bool onHer) {
    if (!mHalo) return;
    // On her, the region is hers by right and there is nothing to expire: the
    // pointer is over her outline, which is the region she would have anyway.
    if (mHaloTimer) g_source_remove(mHaloTimer);
    mHaloTimer = onHer ? 0 : g_timeout_add(kHaloLingerMs, onHaloTimeout, this);
}

gboolean Shell::onHaloTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->mHaloTimer = 0;
    // Still on her - the pointer has been resting on her shoulder rather than
    // beside her, and taking the halo away would be taking away her own outline.
    if (self->overHer(self->mPointerX, self->mPointerY)) return G_SOURCE_REMOVE;
    self->leaveHalo();
    return G_SOURCE_REMOVE;
}

// Only touches the widget when the answer changes: set_cursor_from_name builds a
// GdkCursor per call, and this is on the motion path.
void Shell::setCursor(const char* name) {
    if (!mArea || strcmp(name, mCursor) == 0) return;
    mCursor = name;
    gtk_widget_set_cursor_from_name(mArea, name);
}

void Shell::onSecondaryPress(GtkGestureClick* gesture, int, double x, double y,
                             gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // Claim the sequence so the press does not also reach the drag gesture.
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    if (!self->overHer(x, y)) {
        self->leaveHalo();   // right-clicked the desktop beside her, not her
        return;
    }
    // A second right-click puts it away. In practice the popover's own grab
    // usually swallows this press and closes itself, but that only covers a
    // click *outside* the menu - and the menu stands beside her, so the obvious
    // place to click again is her, which is inside our surface, not the popup's.
    if (self->mMenu.isOpen())
        self->mMenu.close();
    else
        self->openMenu();
}

gboolean Shell::onScroll(GtkEventControllerScroll*, double, double dy,
                         gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (dy == 0) return FALSE;
    // Scrolling over the desktop beside her, inside the halo, is the window
    // underneath being scrolled - not a request to resize her. Only worth
    // asking while the halo is up: with it down, the region *is* her outline,
    // so an event reaching us came from her by definition.
    if (self->mHalo && !self->overHer(self->mPointerX, self->mPointerY)) return FALSE;
    // dy is one unit per wheel notch but a small fraction per touchpad frame,
    // so the step has to follow it rather than be applied per event - otherwise
    // a touchpad flick runs the whole range in a few hundred milliseconds.
    self->setUserScale(self->mUserScale * std::pow(kScaleStep, -dy));
    return TRUE;
}

}  // namespace asuna
