// Shell construction: resolve startup state, build the layer-shell window, and
// load the outfit. Runtime state and the remaining concerns live in the other
// shell/*.cpp units listed in ui/shell.hpp.
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
#include "ui/shell/internal.hpp"
#include "ui/shell/layer.hpp"

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

}  // namespace

namespace asuna {
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

}  // namespace asuna
