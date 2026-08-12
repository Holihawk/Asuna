// Shell: the layer-shell window she lives in, and the lifecycle around it -
// which outfit, which monitor, which layer, what the config file said, and
// what gets written back to the state file. The rest of the class is split
// across shell_render, shell_input, shell_speech, shell_command and
// shell_debug; ui/shell.hpp says which is which.
#include "ui/shell.hpp"

#include <glib-unix.h>
#include <gtk4-layer-shell.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "app/config.hpp"
#include "app/daemon.hpp"
#include "paths.hpp"
#include "ui/shell_internal.hpp"

namespace {

// Worn if nothing else has an opinion: neither the command line, nor the state
// file, nor the config file.
constexpr const char* kDefaultOutfit = "02";

// How long the goodbye stays up before the loop stops.
constexpr guint kFarewellMs = 1400;
// Long enough to read "asuna show" off the bubble before she takes it with her.
constexpr guint kHideDelayMs = 2200;

// The speech bubble is a plain GTK widget over the GL area, so it is styled the
// way any GTK widget is. It sits over the user's desktop rather than over an
// application window, so it cannot inherit a background that is guaranteed to
// contrast with what is behind it - it brings its own, and it is the same dark
// wash the menu uses, because the two are both her speaking and should look it.
constexpr const char* kCss =
    "#asuna-window, #asuna-window > * { background: none; background-color: transparent; }"
    ".asuna-bubble {"
    // Translucent enough to belong to the desktop rather than to sit on top of
    // it, dark enough that pale text on it reads over anything. Light-on-dark
    // also holds up better here than the pale wash it replaces: her artwork is
    // bright, and a bright bubble against bright hair had nothing to separate
    // the two but a border.
    "  background-color: rgba(26, 24, 36, 0.78);"
    "  color: #f4f1fa;"
    "  border: 1px solid rgba(255, 255, 255, 0.14);"
    "  border-radius: 14px;"
    // Named rather than inherited from the theme, and the same family for both
    // scripts. The theme font here is Adwaita Sans, which has no CJK at all, so
    // every Chinese line was already being drawn in a fallback font while the
    // line box was sized from a mixture of the two - which is what a tail
    // shaved off the bottom of 了 or 好 actually was.
    "  font-family: 'Noto Sans CJK SC', 'Noto Sans CJK JP', 'Noto Sans', sans-serif;"
    "  font-size: 14px;"
    // Even padding, and no line-height. Both were once inflated to keep the tail
    // of 了 or 好 off the bottom edge - and neither was what fixed it; naming the
    // font above was, because the shaved tail was a line box sized from Adwaita
    // Sans while the ink came from a fallback. Room bought against a problem that
    // no longer exists is not free: the band the strip reserves for this bubble
    // is measured off these rules, and every px of band is a px she can no longer
    // be scrolled up to. The 1.15 and the extra 3 px cost 30 px of band and 27 px
    // of her ceiling.
    "  padding: 8px 14px;"
    "  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.38);"
    "}"
    // The menu stands beside her over the user's desktop, so it is translucent
    // and dark: it has to read as belonging to her rather than to whatever
    // application happens to be underneath, and it must not hide the part of
    // her the user is about to point at. GTK's own popover node paints the
    // background on `contents`, so that is where the colour goes - styling
    // `popover` itself only produces a second, opaque rectangle behind it.
    ".asuna-menu { background: none; }"
    ".asuna-menu > contents {"
    "  background-color: rgba(26, 24, 36, 0.80);"
    "  color: #f1edf8;"
    "  border: 1px solid rgba(255, 255, 255, 0.14);"
    "  border-radius: 12px;"
    "  padding: 4px;"
    "  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);"
    "}"
    ".asuna-menu modelbutton { border-radius: 8px; }"
    // The keyboard-selection highlight, turned off on purpose.
    //
    // An autohide popover moves focus into itself when it pops up, and it lands
    // on the first item; a focused GtkModelButton draws itself selected. So the
    // menu came up with Outfit already lit, and the light went out again as soon
    // as the pointer did anything - a flicker on the top item, every open.
    //
    // Here it can never mean anything. The strip asks the compositor for
    // keyboard mode NONE (see Shell::buildWindow), so no key event ever reaches
    // this menu and nothing but the pointer can move that selection - and the
    // pointer already has :hover to say where it is. If a later phase asks for
    // keyboard interactivity, this rule is the one to take back out.
    //
    // Above the :hover rule rather than below it: the two selectors have equal
    // specificity, so on the item under the pointer - focused and hovered at
    // once - the later one has to be the one that paints.
    ".asuna-menu modelbutton:selected,"
    ".asuna-menu modelbutton:focus { background-color: transparent; }"
    ".asuna-menu modelbutton:hover { background-color: rgba(255, 255, 255, 0.16); }"
    ".asuna-menu separator { background-color: rgba(255, 255, 255, 0.16); }";

bool knownLayer(const std::string& name) {
    return name == "top" || name == "bottom" || name == "overlay" || name == "background";
}

GtkLayerShellLayer parseLayer(const std::string& name) {
    if (name == "bottom") return GTK_LAYER_SHELL_LAYER_BOTTOM;
    if (name == "overlay") return GTK_LAYER_SHELL_LAYER_OVERLAY;
    if (name == "background") return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    return GTK_LAYER_SHELL_LAYER_TOP;
}

}  // namespace

namespace asuna {
// Here rather than in config.cpp so that file needs nothing from shell.hpp, and
// the config parser's test can link a parser rather than a toolkit.
void Config::applyTo(ShellOptions* o) const {
    // The five the state file also owns go to the seed tier, not to the fields
    // the command line writes - see ShellOptions::Seed.
    o->seed.model = model;
    o->seed.scale = scale;
    o->seed.layer = layer;
    o->seed.hidden = hidden;
    o->seed.output = output;

    o->configSource = source;
    o->anchor = anchor;
    o->framing = framing;
    o->language = language;
    o->greet = greet;
    o->stripHeight = height;
    o->maxHeight = maxHeight;
    o->margin = margin;
    o->bottomMargin = bottomMargin;
    o->pad = pad;
    o->bubbleBand = bubbleBand;
    o->sideBleed = sideBleed;
    o->fps = fps;
    o->gazeHalo = gazeHalo;
    o->bubble.base = bubbleBase;
    o->bubble.perGlyph = bubblePerGlyph;
    o->bubble.max = bubbleMax;
    o->behaviour = behaviour;
    o->ext = ext;
}

Shell::Shell(ShellOptions opt) : mOpt(std::move(opt)) {
    // "auto" (the default) lets each outfit's index.json decide; anything else
    // is the user overriding it from the command line.
    if (mOpt.framing != "auto") mPet.setFraming(parseFraming(mOpt.framing.c_str()));

    if (mOpt.persist) mState.load();

    // Precedence, per README "Which setting wins": CLI flag > state > config >
    // built-in default. mOpt's own fields carry the flag, mOpt.seed carries the
    // config file, and each of these five reads down that list until something
    // answers. The outfit is only restored if it is still on disk - models/ is a
    // fetch away, so a remembered outfit can legitimately vanish.
    const auto findModel = [](const std::string& remembered) -> std::string {
        if (remembered.empty()) return "";
        if (std::filesystem::exists(remembered)) return remembered;
        // The remembered path was relative to whatever directory she was started
        // in last time, and a daemon launched by the session has no way to
        // reproduce that. The outfit id survives it, and the registry knows
        // where models are.
        const std::string byId = resolveModelArg(outfitId(remembered));
        return (!byId.empty() && std::filesystem::exists(byId)) ? byId : std::string();
    };
    if (mOpt.model.empty()) mOpt.model = findModel(mState.model);
    if (mOpt.model.empty()) mOpt.model = findModel(resolveModelArg(mOpt.seed.model));
    if (mOpt.model.empty()) mOpt.model = resolveModelArg(kDefaultOutfit);

    const float scale = mOpt.scale >= 0      ? mOpt.scale
                        : mState.scale >= 0  ? mState.scale
                        : mOpt.seed.scale >= 0 ? mOpt.seed.scale
                                               : 1.0f;
    mUserScale = std::clamp(scale, kMinUserScale, kMaxUserScale);

    mHidden = mOpt.hidden >= 0     ? mOpt.hidden != 0
              : mState.hidden >= 0 ? mState.hidden != 0
                                   : mOpt.seed.hidden > 0;

    if (mOpt.layer.empty())
        mOpt.layer = knownLayer(mState.layer)        ? mState.layer
                     : knownLayer(mOpt.seed.layer)   ? mOpt.seed.layer
                                                     : std::string("top");
    if (mOpt.output.empty())
        mOpt.output = !mState.output.empty() ? mState.output : mOpt.seed.output;
}

Shell::~Shell() {
    flushSave();
    if (mBubbleIdle) g_source_remove(mBubbleIdle);
    if (mHaloTimer) g_source_remove(mHaloTimer);
    if (mBodyRegion) cairo_region_destroy(mBodyRegion);
}

// Finds a monitor by connector name; "" means "the first one, whatever it is".
// Returns nullptr if the name was given and no such monitor is plugged in - the
// caller decides whether that is worth complaining about, because at startup it
// is a stale config and at hotplug time it is a cable somebody just pulled.
//
// g_list_model_get_item() returns a reference. The monitors belong to the
// display and outlive us either way, but holding one per call over a session of
// hotplug events is still a leak, so each is dropped as we go.
GdkMonitor* Shell::findMonitor(const std::string& connector, GdkMonitor** first) {
    if (first) *first = nullptr;
    GdkDisplay* display = gdk_display_get_default();
    if (!display) return nullptr;
    GListModel* monitors = gdk_display_get_monitors(display);
    const guint n = g_list_model_get_n_items(monitors);
    GdkMonitor* found = nullptr;

    for (guint i = 0; i < n; ++i) {
        auto* m = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
        if (first && !*first) *first = m;
        const char* name = gdk_monitor_get_connector(m);
        if (!found && !connector.empty() && name && connector == name) found = m;
        g_object_unref(m);
    }
    return connector.empty() ? (first ? *first : nullptr) : found;
}

GdkMonitor* Shell::pickMonitor() const {
    GdkMonitor* first = nullptr;
    if (GdkMonitor* m = findMonitor(mOpt.output, &first)) return m;
    if (!mOpt.output.empty() && first) {
        fprintf(stderr, "asuna: output '%s' is not plugged in, using %s\n",
                mOpt.output.c_str(), gdk_monitor_get_connector(first));
    }
    return first;
}

void Shell::buildWindow() {
    mWindow = gtk_window_new();
    gtk_window_set_decorated(GTK_WINDOW(mWindow), FALSE);
    gtk_widget_set_name(mWindow, "asuna-window");

    // Hint her text against the pixel grid, which since GTK 4.16 is something
    // an application has to ask for.
    //
    // That release made `gtk-font-rendering: automatic` the default, and
    // automatic means unhinted outlines placed at subpixel offsets. It is the
    // right call on a HiDPI screen, where there are enough pixels for a stroke
    // to survive landing between two rows. At 1x it is not: a 14px CJK glyph
    // draws its strokes about one pixel wide, and the ones that miss the grid
    // are rendered as two half-lit rows instead of one lit row.
    //
    // Which is invisible for most of a glyph and fatal for one stroke in
    // particular. 色, 吃, 也, 把, 包 - every character ending in 竖弯钩 - closes
    // on a long horizontal at the very bottom of the em box, and that is the
    // stroke the grid misses. Half-lit twice reads as *lighter*, and pale text
    // on this bubble's dark wash has nowhere to go from there: the stroke
    // washes out to nothing and she says 巴 for 色. Dark-on-light hides this,
    // because there the same two rows read as heavier rather than absent -
    // which is why it shows up here and in any other dark-themed window, and
    // not in a terminal, where the glyphs are hinted and start on the grid.
    //
    // MANUAL does not mean we choose the hinting; it means fontconfig's answer
    // is used rather than overridden, so this follows the user's desktop
    // setting the way every other application on it does.
#if GTK_CHECK_VERSION(4, 16, 0)
    if (GtkSettings* settings = gtk_settings_get_default())
        g_object_set(settings, "gtk-font-rendering", GTK_FONT_RENDERING_MANUAL,
                     nullptr);
#endif

    // The strip must not paint anything of its own, or the "transparent"
    // background becomes an opaque GTK theme colour.
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, kCss);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // --- wlr-layer-shell -----------------------------------------------
    gtk_layer_init_for_window(GTK_WINDOW(mWindow));
    gtk_layer_set_namespace(GTK_WINDOW(mWindow), "asuna");
    gtk_layer_set_layer(GTK_WINDOW(mWindow), parseLayer(mOpt.layer));
    // Anchoring left+right+bottom makes the surface a full-width strip glued
    // to the bottom edge; its height is whatever we request.
    gtk_layer_set_anchor(GTK_WINDOW(mWindow), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(mWindow), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(mWindow), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    // -1: never reserve space, never push tiled windows around.
    gtk_layer_set_exclusive_zone(GTK_WINDOW(mWindow), -1);
    // A desktop pet must never take keyboard focus.
    gtk_layer_set_keyboard_mode(GTK_WINDOW(mWindow),
                                GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_margin(GTK_WINDOW(mWindow), GTK_LAYER_SHELL_EDGE_BOTTOM,
                         mOpt.bottomMargin);
    mMonitor = pickMonitor();
    if (mMonitor) gtk_layer_set_monitor(GTK_WINDOW(mWindow), mMonitor);

    // Hotplug. The monitor she is standing on can be unplugged, and a layer
    // surface whose output has gone is a surface the compositor destroys.
    if (GdkDisplay* display = gdk_display_get_default())
        g_signal_connect(gdk_display_get_monitors(display), "items-changed",
                         G_CALLBACK(onMonitorsChanged), this);

    mRequestedHeight = mOpt.stripHeight;
    gtk_widget_set_size_request(mWindow, -1, mRequestedHeight);

    // --- GL area ---------------------------------------------------------
    mArea = gtk_gl_area_new();
    // GLES, because the renderer's shaders are GLSL ES 1.00 and GTK will not
    // hand out a desktop compatibility profile.
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(mArea), GDK_GL_API_GLES);
    // GTK4's GL area is always RGBA (unlike GTK3, which needed set_has_alpha),
    // so transparency comes from the clear colour plus the window background.
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(mArea), FALSE);
    gtk_gl_area_set_auto_render(GTK_GL_AREA(mArea), FALSE);
    gtk_widget_set_hexpand(mArea, TRUE);
    gtk_widget_set_vexpand(mArea, TRUE);

    g_signal_connect(mArea, "realize", G_CALLBACK(onRealize), this);
    g_signal_connect(mArea, "unrealize", G_CALLBACK(onUnrealize), this);
    g_signal_connect(mArea, "render", G_CALLBACK(onRender), this);

    // The bubble is a GTK widget laid over the GL area, not something drawn into
    // the scene. It therefore never appears in the alpha the input region is
    // sampled from, which is what keeps a line of dialogue from swallowing
    // clicks over a strip of empty desktop.
    mOverlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(mOverlay), mArea);
    mBubble.attach(mOverlay);
    gtk_window_set_child(GTK_WINDOW(mWindow), mOverlay);

    // --- input -----------------------------------------------------------
    // Only the input region reaches these at all, so the controllers cover the
    // whole strip but fire only over her.
    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(onDragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(onDragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(onDragEnd), this);
    // A cancelled sequence (the compositor breaking the pointer grab, the
    // surface being unmapped mid-drag) does not necessarily produce a drag-end,
    // and a stuck mDragging would hold off every state write from then on.
    g_signal_connect(drag, "cancel", G_CALLBACK(onDragCancel), this);
    gtk_widget_add_controller(mArea, GTK_EVENT_CONTROLLER(drag));

    GtkEventController* scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(onScroll), this);
    gtk_widget_add_controller(mArea, scroll);

    // Right-click anywhere on her opens the menu. Its own gesture rather than a
    // branch inside the drag: GtkGestureDrag is bound to the primary button, and
    // the two must not fight over a sequence.
    GtkGesture* secondary = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(onSecondaryPress), this);
    gtk_widget_add_controller(mArea, GTK_EVENT_CONTROLLER(secondary));

    // Gaze. This only ever fires while the cursor is inside her input region -
    // Wayland delivers motion nowhere else - so "follows the cursor" really
    // means "follows it while you are touching her" (see the README, "Known limits"). Outside that,
    // Behaviour drifts her back to an idle look-around.
    GtkEventController* pointer = gtk_event_controller_motion_new();
    // "enter" carries coordinates and the same signature, and it is the only
    // event a pointer that arrives and then scrolls without moving produces.
    g_signal_connect(pointer, "enter", G_CALLBACK(onPointerMotion), this);
    g_signal_connect(pointer, "motion", G_CALLBACK(onPointerMotion), this);
    g_signal_connect(pointer, "leave", G_CALLBACK(onPointerLeave), this);
    gtk_widget_add_controller(mArea, pointer);

    // The cursor is the only affordance saying she can be picked up, and it can
    // only ever be seen over her, since the rest of the strip takes no input.
    gtk_widget_set_cursor_from_name(mArea, "grab");

    // Assigned by name, not as a brace list. It was a brace list until Chat was
    // added to the middle of Actions and every handler after it quietly moved
    // up one: Chat resized her, Reset position opened the chat, Reset size
    // moved her. Nine same-shaped std::functions in a row is a structure that
    // cannot be checked by reading it, and the compiler has nothing to say
    // about it either.
    Menu::Actions actions;
    actions.outfit = [this](const std::string& id) { setOutfit(id); };
    // Through Behaviour rather than straight at the Pet: it has to count as
    // attention so she does not doze off while being played with, and it is
    // Behaviour that owns which face she is wearing.
    actions.expression = [this](const std::string& name) { mBehaviour.wearExpression(name); };
    actions.motion = [this](const std::string& name) {
        mBehaviour.notice();
        mPet.startMotionNamed(name, 3);
    };
    actions.resetPosition = [this] { mMotion.glideTo(clampX(anchorX())); scheduleSave(); };
    actions.resetSize = [this] { setUserScale(1.0f); };
    actions.chat = [this] { openChat(""); };
    actions.hide = [this] {
        // The line first, then she goes. The menu closes with her, so the
        // bubble is the last thing on screen that can say how to undo this.
        say("hide");
        g_timeout_add(kHideDelayMs, onHideTimeout, this);
    };
    actions.quit = [this] { quit(); };
    actions.visibility = [this](bool open) {
        // The popover is its own Wayland surface with its own input region,
        // but if the compositor ever declines that and GTK falls back to
        // drawing it inside ours, a silhouette-shaped region would make the
        // menu unclickable. Opening it up costs nothing while the menu is
        // the thing the user is looking at.
        mMenuOpen = open;
        if (open) {
            leaveHalo();   // the menu is taking the region over
            const cairo_rectangle_int_t all = {0, 0, mStripWidth / mScaleFactor,
                                               mStripHeight / mScaleFactor};
            applyRegion(&all, 1);
        } else {
            mRegionDirty = true;
        }
    };
    mMenu.attach(mArea, std::move(actions));

    g_signal_connect(mWindow, "map", G_CALLBACK(onMap), this);
    mTickId = gtk_widget_add_tick_callback(mArea, onTick, this, nullptr);
}


bool Shell::setOutfit(const std::string& id) {
    if (id == outfitId(mPet.modelPath())) return true;   // already wearing it
    const std::string path = outfitsRoot(mPet.modelPath()) + "/asuna_" + id + "/index.json";
    if (!std::filesystem::exists(path)) return false;
    // Loading uploads textures and solves a fit, and the fit renders her to
    // measure herself. Both need this context, not whichever one GTK happens to
    // have left current.
    if (mArea && gtk_widget_get_realized(mArea))
        gtk_gl_area_make_current(GTK_GL_AREA(mArea));
    if (!mPet.load(path)) return false;

    // A new outfit is a new figure: it re-solves its own framing, which can
    // change the box width and the strip height (a full-body costume needs a
    // taller one), so her position has to be re-clamped into the new bounds.
    applyFraming();
    mMotion.reset(clampX(mMotion.placement().x));
    schedulePlaceBubble();
    mBehaviour.setInventory(mPet.expressionNames(), mPet.motionNames());
    rebuildMenu();
    say("outfit");
    scheduleSave();
    publish("outfit", ipc::Out().str("id", id).done());
    printf("asuna: outfit=%s\n", path.c_str());
    return true;
}

void Shell::quit() {
    flushSave();
    // While the socket is still up: the helper is a separate process with no
    // other way of knowing, and a subscriber that finds out by having its
    // connection dropped cannot tell "she left" from "she crashed".
    publish("bye");
    // Nobody is watching, so there is nothing to wait for. Going straight out
    // also keeps `asuna hide && asuna exit` from taking a second and a half.
    if (mHidden || !mPlaced) {
        g_timeout_add(1, onFarewellTimeout, this);
        return;
    }
    // Let the goodbye actually be read. The bubble is a GTK widget on its own
    // timer, so it keeps animating while the main loop winds down.
    mBubble.clear();
    placeBubble();
    mBubble.say(mDialogue.pick("farewell"));
    g_timeout_add(kFarewellMs, onFarewellTimeout, this);
}

gboolean Shell::onFarewellTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (self->mLoop) g_main_loop_quit(self->mLoop);
    return G_SOURCE_REMOVE;
}

// --- the control socket -----------------------------------------------------

void Shell::signalReady(const std::string& status) {
    if (mOpt.readyFd < 0) return;
    const std::string line = status + "\n";
    // Nothing useful to do if this fails: the parent has gone, which only means
    // nobody is waiting for the answer.
    [[maybe_unused]] const ssize_t n = write(mOpt.readyFd, line.data(), line.size());
    close(mOpt.readyFd);
    mOpt.readyFd = -1;
}

// Unmapping the layer surface is the whole of it. GTK stops the frame clock for
// a window that is not on screen, and every one of her clocks hangs off that
// tick - the motion spring, the idle scheduler, the sleep countdown, the render
// throttle - so she does not need pausing separately. It also means the
// compositor stops recompositing a full-width strip, which is where the CPU
// actually goes.
void Shell::setHidden(bool hidden) {
    if (hidden == mHidden) return;
    mHidden = hidden;
    if (hidden) {
        mMenu.close();
        mBubble.clear();
        leaveHalo();
        gtk_widget_set_visible(mWindow, FALSE);
        // Measured, not assumed. Unmapping alone left her costing 0.9% of a
        // core, because GTK keeps the frame clock running for a window that is
        // off screen and our callback with it. Taking the callback off as well
        // is what makes it nothing: 0 scheduler ticks over 20 s, against 7.1%
        // of a core visible.
        if (mTickId) {
            gtk_widget_remove_tick_callback(mArea, mTickId);
            mTickId = 0;
        }
    } else {
        if (!mTickId) mTickId = gtk_widget_add_tick_callback(mArea, onTick, this, nullptr);
        // The clock was stopped, not slowed: without this the first tick back
        // hands the motion model however long she was away as a single step.
        mLastTickUs = 0;
        mLastFrameUs = 0;
        mLastRenderUs = 0;
        mRegionDirty = true;
        gtk_window_present(GTK_WINDOW(mWindow));
    }
    scheduleSave();
    publish("visible", ipc::Out().boolean("hidden", mHidden).done());
    printf("asuna: %s\n", hidden ? "hidden" : "shown");
}

bool Shell::setLayer(const std::string& name) {
    if (!knownLayer(name)) return false;
    mOpt.layer = name;
    if (mWindow) gtk_layer_set_layer(GTK_WINDOW(mWindow), parseLayer(name));
    scheduleSave();
    printf("asuna: layer=%s\n", name.c_str());
    return true;
}

bool Shell::setOutput(const std::string& connector) {
    GdkMonitor* first = nullptr;
    GdkMonitor* target = findMonitor(connector, &first);
    if (!target) return false;   // a name, but nothing plugged in answering to it

    const char* name = gdk_monitor_get_connector(target);
    mOpt.output = connector.empty() ? std::string() : std::string(name ? name : "");
    if (!mWindow || target == mMonitor) {
        scheduleSave();
        return true;
    }

    // Keep her in the corner she was in, not at the pixel she was at: a 1920
    // laptop panel and a 2560 desktop screen have almost no x in common, and a
    // pet that was tucked into the right-hand corner should arrive in the
    // right-hand corner. Measured from whichever edge she was nearer, so a
    // left-corner pet stays left and a roughly-central one stays roughly
    // central.
    double gap = -1;
    bool fromRight = false;
    if (mPlaced && mStripWidth > 0) {
        const double x = mMotion.placement().x;
        const double right = mStripWidth - (x + mPet.boxWidth());
        fromRight = right < x;
        gap = std::max(0.0, fromRight ? right : x);
    }

    mMonitor = target;
    gtk_layer_set_monitor(GTK_WINDOW(mWindow), target);

    // The compositor's configure for the new output has not arrived yet, so take
    // the width from the monitor itself rather than waiting for it. onRender
    // corrects both this and the scale factor if the guess was wrong, which is
    // the same path a mode switch already goes through.
    GdkRectangle geom;
    gdk_monitor_get_geometry(target, &geom);
    mStripWidth = geom.width * mScaleFactor;
    applyFraming();   // the new screen's height caps how tall the strip may be
    if (gap >= 0) {
        mMotion.setBounds(0, std::max(0, mStripWidth - mPet.boxWidth()),
                          mPet.boxHeight());
        mMotion.reset(clampX(fromRight ? mStripWidth - mPet.boxWidth() - gap : gap));
        schedulePlaceBubble();
    }
    mRegionDirty = true;
    scheduleSave();
    printf("asuna: output=%s %dx%d\n", name ? name : "?", geom.width, geom.height);
    return true;
}

void Shell::onMonitorsChanged(GListModel*, guint, guint removed, guint, gpointer data) {
    auto* self = static_cast<Shell*>(data);
    if (!removed || !self->mWindow) return;
    // Is the one she is standing on still there? Comparing the pointer would be
    // comparing against an object GDK may already have dropped, so ask by name -
    // which is also the identity the user gave us and the one in state.json.
    GdkMonitor* first = nullptr;
    if (findMonitor(self->mOpt.output, &first)) return;
    if (!first) return;   // every monitor gone: nothing to move to, and nothing to draw on

    const char* name = gdk_monitor_get_connector(first);
    fprintf(stderr, "asuna: output '%s' went away, moving to %s\n",
            self->mOpt.output.empty() ? "(default)" : self->mOpt.output.c_str(),
            name ? name : "?");
    // Deliberately does *not* rewrite mOpt.output to the fallback: unplugging a
    // dock should not make her forget which screen she belongs on. setOutput's
    // empty-string form takes the first monitor without recording it, and the
    // preference survives in state.json for when the cable comes back.
    const std::string preferred = self->mOpt.output;
    self->setOutput("");
    self->mOpt.output = preferred;
}

void Shell::applyTunables() {
    mBehaviour.setTuning(mOpt.behaviour);
    mBubble.setTiming(mOpt.bubble);
    mBubble.setRows(mOpt.ext.bubbleRows);
    // Set here as well as in buildWindow so it is right for the next map, but it
    // does not move a surface that is already up - see reloadConfig.
    if (mWindow)
        gtk_layer_set_margin(GTK_WINDOW(mWindow), GTK_LAYER_SHELL_EDGE_BOTTOM,
                             mOpt.bottomMargin);
}

std::string Shell::reloadConfig(std::vector<std::string>* problems, std::string* note) {
    Config cfg;
    cfg.load(Config::path());
    if (problems) *problems = cfg.problems;
    if (!cfg.problems.empty()) return "";

    // Everything except the five settings state.json owns. Re-seeding those from
    // the config would be the config quietly overruling what the user last did
    // by hand, which is the precedence rule backwards - `asuna scale`, `move`,
    // `layer`, `output` and `model use` are how those change.
    const std::string language = mOpt.language;
    const int margin = mOpt.bottomMargin;
    const ShellOptions::Seed keep = mOpt.seed;
    cfg.applyTo(&mOpt);
    mOpt.seed = keep;

    // The one setting a reload cannot deliver. Everything else here is ours to
    // change; the bottom margin is the compositor's, and it reads it when the
    // layer surface is mapped. The request does go out live - WAYLAND_DEBUG
    // shows `set_margin(0, 0, 60, 0)` followed by a commit - and she does not
    // move, so it is not something another call on our side would fix. Said out
    // loud rather than left to be discovered, because a config change that
    // appears to do nothing is the exact failure this whole file is written to
    // avoid.
    if (note && mOpt.bottomMargin != margin)
        *note = "strip.bottom_margin only takes effect when the surface is mapped -"
                " `asuna restart` applies it";

    applyTunables();
    if (mOpt.framing == "auto") mPet.setFramingAuto();
    else mPet.setFraming(parseFraming(mOpt.framing.c_str()));
    if (mOpt.language != language && mPet.loaded()) {
        mDialogue.load(Dialogue::defaultPath(mOpt.language));
    }
    // side_bleed, the strip heights, the pad and the band all feed the framing
    // solver, so one re-solve picks up every one of them - and it is the same
    // call an outfit switch makes, so it is a path that is already exercised.
    if (mPet.loaded()) applyFraming();
    // The helper reads its endpoint, its model and both vision switches from
    // here, so a reload has to reach it too - otherwise `asuna config reload`
    // would be a setting that only half applies, which is the exact failure
    // this file is written to avoid.
    publish("config");
    return cfg.source.empty() ? std::string("no config file; back to the defaults")
                              : cfg.source;
}

gboolean Shell::onTerminate(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // A signal is "stop now", not "say goodbye" - but the state file still has
    // to be written, which is the whole reason this is not the default handler.
    printf("asuna: terminating\n");
    self->flushSave();
    if (self->mLoop) g_main_loop_quit(self->mLoop);
    return G_SOURCE_REMOVE;
}

// --- persistence ----------------------------------------------------------

void Shell::scheduleSave() {
    if (!mOpt.persist) return;
    mDirty = true;
    if (!mSaveTimer) mSaveTimer = g_timeout_add_seconds(1, onSaveTimeout, this);
}

gboolean Shell::onSaveTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    // Never write mid-drag: the position is still moving and the file would be
    // rewritten a second later anyway. The timer simply waits out the drag.
    if (self->mDragging) return G_SOURCE_CONTINUE;
    self->mSaveTimer = 0;
    self->flushSave();
    return G_SOURCE_REMOVE;
}

void Shell::flushSave() {
    if (mSaveTimer) {
        g_source_remove(mSaveTimer);
        mSaveTimer = 0;
    }
    // Only ever write what the user changed. Persisting the anchored start
    // position too would pin her there forever and make --anchor look broken on
    // the second run.
    if (!mOpt.persist || !mDirty || !mPlaced) return;
    mDirty = false;
    mState.x = mMotion.placement().x;
    mState.scale = mUserScale;
    mState.model = mPet.modelPath();
    mState.hidden = mHidden ? 1 : 0;
    mState.layer = mOpt.layer;
    mState.output = mOpt.output;
    if (!mState.save())
        fprintf(stderr, "asuna: could not write %s\n", State::path().c_str());
}

int Shell::run() {
    mStartedUs = g_get_monotonic_time();

    if (!gtk_layer_is_supported()) {
        const char* why = "this compositor does not support wlr-layer-shell";
        fprintf(stderr, "asuna: %s.\n", why);
        signalReady(std::string("err: ") + why);
        return 1;
    }

    // The lock is taken here rather than by whoever spawned us, because here is
    // the only place that cannot lose a race: a second daemon that gets this far
    // is a second daemon, however carefully its parent checked first.
    if (mOpt.control) {
        pid_t holder = 0;
        if (!mLock.acquire(paths::lockPath(), &holder)) {
            std::string why = "already running";
            if (holder > 0) why += " (pid " + std::to_string(holder) + ")";
            fprintf(stderr, "asuna: %s\n", why.c_str());
            signalReady("err: " + why);
            return 1;
        }
    }

    buildWindow();

    if (mOpt.control) {
        std::string error;
        const std::string socket = paths::socketPath();
        if (!mServer.start(socket, [this](const Json& r) { return handleCommand(r); },
                           &error)) {
            fprintf(stderr, "asuna: control socket: %s\n", error.c_str());
            signalReady("err: " + error);
            return 1;
        }
        mServer.onSubscribersChanged([this] { updateChatter(); });
        printf("asuna: listening on %s\n", socket.c_str());
    }

    if (mHidden)
        printf("asuna: starting hidden - `asuna show` brings her back\n");
    else
        gtk_window_present(GTK_WINDOW(mWindow));

    // Ready means "the socket answers", not "she is on screen": the first frame
    // is about two seconds away behind a model load and the compositor's first
    // configure, and making `asuna start` wait for it would be making the shell
    // wait for an animation. Commands that need her say so until then.
    signalReady("ok");

    mLoop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGTERM, onTerminate, this);
    g_unix_signal_add(SIGINT, onTerminate, this);
    g_signal_connect_swapped(mWindow, "destroy", G_CALLBACK(g_main_loop_quit),
                             mLoop);
    g_main_loop_run(mLoop);
    g_main_loop_unref(mLoop);
    mLoop = nullptr;
    mServer.stop();
    return mGlFailed ? 1 : 0;
}

}  // namespace asuna
