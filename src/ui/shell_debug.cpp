// Shell, the part that exists to be tested. This machine cannot synthesise a
// Wayland pointer, so a tap, a drag, a scroll and a right-click are all
// unreachable from a test - each one here is an environment variable that
// runs the same two lines the real event would. In the same spirit as the
// renderer's ASUNA_TEXFILTER: diagnostics that ship, because the questions
// they answer can only be asked of a live compositor.
#include "ui/shell.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ui/shell_internal.hpp"

namespace asuna {
gboolean Shell::onPokeTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->poke(self->mDebugPokeX, self->mDebugPokeY);
    // Repeating, if an interval was given. A poke is the only way to tell her
    // helper "I am still here" - it is what re-opens the chat prompt after an
    // answer - so testing that end of it needs a poke that arrives while a
    // conversation is running, not one fired at startup.
    return self->mDebugPokeEveryMs > 0 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

gboolean Shell::onScaleTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (self->mDebugScales.empty()) return G_SOURCE_REMOVE;
    const float next = self->mDebugScales.front();
    self->mDebugScales.erase(self->mDebugScales.begin());
    printf("asuna: debug scale -> %.2f\n", next);
    self->setUserScale(next);
    return self->mDebugScales.empty() ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

gboolean Shell::onGazeTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (self->mDebugGaze.empty()) return G_SOURCE_REMOVE;
    const auto [x, y] = self->mDebugGaze.front();
    self->mDebugGaze.erase(self->mDebugGaze.begin());
    self->mPointerX = x;
    self->mPointerY = y;
    // The same four lines onPointerMotion runs, in the same order, including
    // the one that arms the halo's expiry. A hook that only did the first two
    // would report on a halo that nothing was ever going to take away.
    const bool on = self->overHer(x, y);
    if (on) self->enterHalo();
    self->refreshHalo(on);
    self->setCursor(on ? "grab" : "default");
    self->aimGaze(x, y);
    printf("asuna: gaze aimed from (%.0f,%.0f)%s halo=%s\n", x, y,
           on ? "" : " (not on her)", self->mHalo ? "on" : "off");
    // A second point 200 ms later is how the halo's expiry gets tested: the
    // first has to be on her (nothing else can arm the halo), the second beside
    // her, and then the question is whether the region comes back on its own.
    if (!self->mDebugGaze.empty()) {
        g_timeout_add(200, onGazeTimeout, self);
        return G_SOURCE_REMOVE;
    }
    // The gaze is a target the smoothing eases toward, so reading it in this
    // same turn only ever says zero. Report it once it has arrived - and by
    // then the halo has had long enough to expire, which is the other half of
    // what this reports.
    g_timeout_add(500, onGazeReport, self);
    return G_SOURCE_REMOVE;
}

gboolean Shell::onGazeReport(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    printf("asuna: gaze now (%.2f,%.2f) halo=%s\n", self->mBehaviour.gazeX(),
           self->mBehaviour.gazeY(), self->mHalo ? "on" : "off");
    return G_SOURCE_REMOVE;
}

// ASUNA_DEBUG_STREAM's tick: a few characters at a time into the bubble, the
// way the helper's flush does. Chunked on codepoint boundaries, because half a
// UTF-8 sequence in a GtkLabel is not a partial character, it is a warning.
gboolean Shell::onStreamTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (self->mDebugStreamAt >= self->mDebugStream.size()) {
        self->mBubble.finish();
        return G_SOURCE_REMOVE;
    }
    const size_t start = self->mDebugStreamAt;
    size_t end = start;
    for (int glyphs = 0; glyphs < 4 && end < self->mDebugStream.size(); ++glyphs) {
        ++end;
        while (end < self->mDebugStream.size() &&
               (static_cast<unsigned char>(self->mDebugStream[end]) & 0xC0) == 0x80)
            ++end;
    }
    const std::string chunk = self->mDebugStream.substr(start, end - start);
    self->mDebugStreamAt = end;
    self->placeBubble();
    if (start == 0) self->mBubble.begin(chunk);
    else self->mBubble.append(chunk);
    // Re-armed rather than repeating, so the pause in front of the first piece
    // can be the long one a slow endpoint really takes.
    g_timeout_add(120, onStreamTimeout, self);
    return G_SOURCE_REMOVE;
}

gboolean Shell::onStreamStart(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->placeBubble();
    self->mBubble.waiting(true);   // the tell first, as a real answer has
    g_timeout_add(900, onStreamTimeout, self);
    return G_SOURCE_REMOVE;
}

gboolean Shell::onQuitTimeout(gpointer data) {
    static_cast<Shell*>(data)->quit();
    return G_SOURCE_REMOVE;
}

gboolean Shell::onMenuTimeout(gpointer data) {
    static_cast<Shell*>(data)->openMenu();
    return G_SOURCE_REMOVE;
}

gboolean Shell::onActionTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // The menu is built on first open, and an action group with no model behind
    // it still answers - so open it first, or "chat" would be missing for the
    // one reason that has nothing to do with the wiring.
    self->openMenu();
    const size_t colon = self->mDebugAction.find(':');
    const std::string name = self->mDebugAction.substr(0, colon);
    const std::string target =
        colon == std::string::npos ? std::string() : self->mDebugAction.substr(colon + 1);
    const bool ok = self->mMenu.activate(name, target);
    printf("asuna: menu action '%s'%s%s -> %s\n", name.c_str(), target.empty() ? "" : " ",
           target.c_str(), ok ? "activated" : "no such entry");
    self->mMenu.close();
    return G_SOURCE_REMOVE;
}

// ASUNA_DEBUG_MENU=move walks the one sequence a screenshot cannot be taken of
// by hand on this machine: open the menu, close it, move her, open it again, and
// print the rectangle each open anchors to. Whether the second open follows her
// is a question about the input region and about GTK's popup positioning, and
// only a live compositor answers it.
//
// onMenuMoveJiggle is the part that makes it a faithful reproduction rather than
// a friendly one: a hand resting on a mouse delivers motion events continuously,
// each of which re-arms the gaze halo, and the halo is what decides whether her
// silhouette gets re-read. Without it the sequence passes and the bug hides.
gboolean Shell::onMenuMoveJiggle(gpointer data) {
    static_cast<Shell*>(data)->enterHalo();
    return G_SOURCE_CONTINUE;
}

// Ticks at 100 ms. The drag has to be spread over real time like this: run
// begin/update/end in one main-loop turn and the spring never gets a frame to
// follow the target, so she does not actually move and the test proves nothing.
gboolean Shell::onMenuMoveStep(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    const int step = self->mMenuStep++;
    // How long after letting go the menu is reopened, in 100 ms steps. This is
    // the knob the whole reproduction turns on: a release throws her, the spring
    // takes about a second to settle, and the sampler stands down for all of it.
    // Reopen after the settle and the sequence passes; reopen the way a hand
    // does, while she is still coasting, and it does not.
    static const int kReopenAfter = [] {
        const char* s = getenv("ASUNA_DEBUG_REOPEN");
        return s ? atoi(s) : 19;
    }();
    // The real handlers, not glideTo: mDragging, the drag threshold and the
    // release are the whole difference between a drag and a glide, and any one
    // of them could be what leaves the region behind.
    if (step == 20) self->openMenu();
    else if (step == 30) self->mMenu.close();
    else if (step == 40) {
        const GdkRectangle b = self->silhouetteRect();
        onDragBegin(nullptr, b.x + b.width / 2.0, b.y + b.height / 2.0, self);
    } else if (step > 40 && step <= 50)
        onDragUpdate(nullptr, -50.0 * (step - 40), -6.0, self);
    else if (step == 51) onDragEnd(nullptr, -500.0, -6.0, self);
    // Its own test, not another arm of the chain, so ASUNA_DEBUG_REOPEN=0 can
    // ask the hardest question there is: the menu opening in the same main-loop
    // turn as the release, before a single frame has been drawn at the new
    // position.
    if (step == 51 + kReopenAfter) self->openMenu();
    if (step >= 71 + kReopenAfter) return G_SOURCE_REMOVE;
    return G_SOURCE_CONTINUE;
}

// Every ASUNA_DEBUG_* hook, installed once she has been placed. Called from
// placeInitial() so that the strip, the box and her position are all real
// numbers by the time any of these fire.
void Shell::installDebugHooks() {
    // Diagnostics, in the same spirit as the renderer's ASUNA_TEXFILTER: the
    // held pose cannot be reached without a pointer, so ASUNA_DEBUG_HOLD=<px>
    // grabs her at startup and pulls up by that many pixels. Pairs with
    // ASUNA_DEBUG_REGION=1, which prints the sampled silhouette's extent - the
    // two together are how the lift is checked for clipping against the strip.
    if (const char* hold = getenv("ASUNA_DEBUG_HOLD")) {
        mMotion.beginDrag();
        mMotion.hold(mMotion.placement().x, static_cast<float>(atof(hold)));
    }
    // Same idea for the menu: whether a GtkPopover really becomes an xdg_popup
    // on a layer surface is a question only a live compositor can answer, and
    // opening it needs a right-click this machine cannot synthesise.
    if (const char* menu = getenv("ASUNA_DEBUG_MENU")) {
        if (strcmp(menu, "move") == 0) {
            g_timeout_add(100, onMenuMoveStep, this);
            // Separately switchable: the jiggle is what makes the sequence
            // reproduce, and it is also a synthetic pointer that no compositor
            // ever sees, so it is worth being able to run the sequence without
            // it when the question is what the menu looks like rather than
            // where it is anchored.
            if (getenv("ASUNA_DEBUG_JIGGLE")) g_timeout_add(100, onMenuMoveJiggle, this);
        }
        else
            g_timeout_add(kGreetDelayMs * 2, onMenuTimeout, this);
    }
    // ASUNA_DEBUG_ACTION=chat, or =outfit:31, activates a menu entry the way
    // clicking it does. Added the day a menu entry was found to be wired to the
    // wrong handler: Menu::Actions is nine std::functions of nearly the same
    // shape, so inserting one in the middle silently renumbered the rest, and
    // nothing - not the compiler, not a screenshot of the menu - could tell.
    // What a screenshot cannot show is what an item *does*.
    if (const char* action = getenv("ASUNA_DEBUG_ACTION")) {
        mDebugAction = action;
        g_timeout_add(kGreetDelayMs * 5, onActionTimeout, this);
    }
    // ASUNA_DEBUG_STREAM=<text> writes that text into her bubble a few
    // characters at a time, which is what the helper does over the socket. Here
    // because the bubble's hardest questions - does the emergence read as
    // arriving, does a long reply scroll or overflow the strip, is the last row
    // clipped - are all questions about pixels, and answering them through a
    // helper means an API key, a network and a model in the loop.
    if (const char* text = getenv("ASUNA_DEBUG_STREAM")) {
        mDebugStream = text;
        g_timeout_add(kGreetDelayMs * 4, onStreamStart, this);
    }
    // And the way out: the farewell, the final state write and a clean teardown
    // all hang off the menu's Quit, which is equally unreachable from here.
    if (const char* ms = getenv("ASUNA_DEBUG_QUIT"))
        g_timeout_add(static_cast<guint>(atoi(ms)), onQuitTimeout, this);
    // ASUNA_DEBUG_TOUCH=x,y pokes her at a point in the strip's own logical
    // coordinates, running the same two lines a real tap does. The mapping from
    // a pointer to a part of her body is the one piece of Phase 4 that a unit
    // test can only check against itself; this checks it against the pixels.
    if (const char* at = getenv("ASUNA_DEBUG_TOUCH")) {
        double x = 0, y = 0;
        int every = 0;
        if (sscanf(at, "%lf,%lf,%d", &x, &y, &every) >= 2) {
            mDebugPokeX = x;
            mDebugPokeY = y;
            mDebugPokeEveryMs = std::max(0, every);
            g_timeout_add(mDebugPokeEveryMs > 0 ? mDebugPokeEveryMs : kGreetDelayMs * 3,
                          onPokeTimeout, this);
        }
    }
    // ASUNA_DEBUG_GAZE=x,y runs the four lines a real pointer motion runs. There
    // is no way to synthesise Wayland pointer motion without taking over the
    // user's actual mouse, so this is how the halo and the gaze aim are checked
    // against a live compositor; pair it with ASUNA_DEBUG_REGION=1.
    //
    // x1,y1,x2,y2 is the two-point form: a point on her, then one beside her
    // 200 ms later. That is the sequence the halo exists for and the one it used
    // to get wrong - a pointer that has wandered off her and stopped, holding a
    // rectangle of desktop that nothing was going to give back.
    if (const char* at = getenv("ASUNA_DEBUG_GAZE")) {
        double x = 0, y = 0, x2 = 0, y2 = 0;
        const int given = sscanf(at, "%lf,%lf,%lf,%lf", &x, &y, &x2, &y2);
        if (given >= 2) {
            mDebugGaze.push_back({x, y});
            if (given == 4) mDebugGaze.push_back({x2, y2});
            g_timeout_add(kGreetDelayMs * 4, onGazeTimeout, this);
        }
    }
    // ASUNA_DEBUG_SCALE=1.8,1.0,... walks her through those sizes a couple of
    // seconds apart, which is the only way to exercise scroll-to-resize without
    // a pointer - and resizing is where the strip has to renegotiate its height
    // with the compositor, the one thing here that can silently not happen.
    if (const char* list = getenv("ASUNA_DEBUG_SCALE")) {
        for (const char* p = list; *p;) {
            char* end = nullptr;
            const float v = std::strtof(p, &end);
            if (end == p) break;
            mDebugScales.push_back(v);
            p = (*end == ',') ? end + 1 : end;
        }
        if (!mDebugScales.empty()) g_timeout_add(2000, onScaleTimeout, this);
    }
}

}  // namespace asuna
