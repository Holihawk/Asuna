#pragma once

#include <gtk/gtk.h>

#include <string>
#include <vector>

#include "character/behaviour.hpp"
#include "ui/bubble.hpp"
#include "app/config.hpp"
#include "app/daemon.hpp"
#include "character/dialogue.hpp"
#include "app/ipc.hpp"
#include "ui/menu.hpp"
#include "pet/motion.hpp"
#include "pet/outfits.hpp"
#include "pet/pet.hpp"
#include "app/state.hpp"

namespace asuna {

struct ShellOptions {
    std::string model;               // empty => state's last outfit, else asuna_02
    std::string layer;               // empty => state's last layer, else "top"
    std::string output;              // empty => compositor's default monitor
    std::string framing = "auto";    // auto | bust | full
    std::string anchor = "right";    // right | left | centre - where she starts
    std::string language = "zh";     // picks data/dialogue.<language>.json

    // What the config file said about the five settings state.json also
    // remembers. Kept apart from the fields above, because those mean "the user
    // said so on the command line just now" and these mean "this is where to
    // start if nothing has been done by hand yet".
    //
    // The precedence, in one place: CLI flag > state > config > built-in
    // default. A config `scale` is the size she is on a machine she has never
    // run on; it must not overrule a size the user scrolled to last week, or
    // writing the file down once would freeze her that way forever.
    struct Seed {
        std::string model;
        std::string layer;
        std::string output;
        float scale = -1.0f;
        int hidden = -1;
    } seed;

    int stripHeight = 460;           // strip height for a bust framing at scale 1
    int maxHeight = 760;             // ceiling when a full-body outfit asks for more
    int margin = 24;                 // gap from the anchored screen edge, px
    // Gap between the strip and the bottom of the screen. 0 - flush against the
    // edge - is the default and what she is designed around: she stands *on*
    // the screen edge. Non-zero floats her, which also lifts her feet off the
    // crop line the framing solves against, so it is a look, not a placement.
    int bottomMargin = 0;
    int pad = 8;                     // transparent margin around her in the box
    int bubbleBand = 92;             // strip height reserved above her for speech
    // Extra width rendered past each side of her box, as a fraction of it. A
    // crossed-arms pose reaches well outside the outline she stands in, and the
    // viewport is a hard clip - without this her hands are cut off mid-motion.
    // asuna-render-test --extent sweeps the whole parameter space of all 42
    // outfits: the worst reaches 25% of the box width (asuna_18), and the
    // motions they actually play reach 16% (I_ANGRY). 0.30 clears both.
    float sideBleed = 0.30f;
    // How far outside her the pointer can be and still be followed, logical px.
    // Costs nothing while she is untouched: the region only grows once the
    // pointer is already on her, and shrinks again when it leaves. 0 disables.
    int gazeHalo = 160;
    bool greet = true;               // say hello on launch
    float scale = -1.0f;             // < 0 => take it from state, else 1.0
    double x = -1.0;                 // >= 0 => explicit pixel position
    int fps = 30;                    // render cap; the strip is wide, so every
                                     // frame costs a full-width recomposite
    bool persist = true;             // remember position/size/outfit between runs
    // Start put away. -1 takes it from state, which is what makes `asuna hide`
    // survive a logout; 0/1 is --show / --hidden overriding that.
    int hidden = -1;
    // The single-instance lock and the control socket. Off is for running a
    // second copy alongside a live one, which is only ever a debugging thing -
    // two pets on one desktop is a bug, not a feature.
    bool control = true;
    // Written "ok" (or "err: …") once the socket is listening, then closed, so
    // `asuna start` can stop waiting. -1 when nobody is waiting.
    int readyFd = -1;

    // From the config file, which is the only thing that sets any of them - they
    // have no command-line flags because nobody wants to type nine numbers to
    // make her slightly quieter. `asuna config reload` re-reads all three live.
    BehaviourTuning behaviour;
    BubbleTiming bubble;
    // The daemon barely uses these: it holds them so `asuna ext config` can
    // hand them to the helper, and enforces exactly one of them itself
    // (vision_enabled, in handleExt). See ExtConfig.
    ExtConfig ext;
    // The config file actually read, empty if there was none. Carried so
    // `asuna status` can answer "which file are you using", which is the first
    // question anyone whose config appears to be ignored actually has.
    std::string configSource;
};

// A bottom-anchored, full-width, transparent wlr-layer-shell surface with the
// pet drawn into it via a GtkGLArea.
//
// Everything outside her input region is click-through, so the strip does not
// steal input from the windows underneath.
//
// Coordinates: her box lives in *device* pixels, because that is what glViewport
// takes. GTK hands out logical pixels, so anything crossing that boundary - the
// pointer, the input region, the size request - goes through mScaleFactor.
//
// One class, seven files, split by what each part of it is for:
//
//   ui/shell.cpp          the window, the monitor, the layer, the config file,
//                         the state file, and the main loop around them
//   ui/shell_render.cpp   the GL callbacks, the framing, where she stands, and
//                         the input region sampled out of her own alpha
//   ui/shell_input.cpp    the drag, the tap, the gaze, the halo, the wheel and
//                         the right-click menu
//   ui/shell_speech.cpp   the hooks Behaviour reaches her through, and the
//                         speech bubble
//   ui/shell_command.cpp  what each `asuna <verb>` does when it arrives
//   ui/shell_ext.cpp      the events an out-of-process helper subscribes to,
//                         and the consent in front of a screen capture
//   ui/shell_debug.cpp    the ASUNA_DEBUG_* hooks, which are the only way to
//                         drive a pointer on a compositor that has none to give
class Shell {
public:
    explicit Shell(ShellOptions opt);
    ~Shell();
    int run();

private:
    void buildWindow();
    void placeInitial();   // apply the remembered position, or the anchor
    void applyFraming();   // re-fit and resize the strip for outfit + user scale
    double clampX(double x) const;
    double anchorX() const;   // where --anchor puts her, in device px
    void setUserScale(float scale);
    GdkMonitor* pickMonitor() const;
    // By connector name; "" takes the first. `first` gets whichever that is,
    // for the caller's fallback message.
    static GdkMonitor* findMonitor(const std::string& connector, GdkMonitor** first);
    void endDrag();

    // --- control socket ---------------------------------------------------
    // One request in, one response line out. Runs on the main loop, so it can
    // touch anything the pointer handlers can.
    ipc::Reply handleCommand(const Json& request);

    // --- extensions -------------------------------------------------------
    // ui/shell_ext.cpp. Everything here exists for the out-of-process helper
    // (see the README, "Extensions"): the daemon does not talk to an API, hold
    // a key or take a screenshot, it only makes her reachable and legible from
    // a program that does.

    // One event to whoever is subscribed, or nothing at all if nobody is. Cheap
    // in the usual case: `data` is not built unless someone is listening, which
    // is why callers ask `listening()` first.
    void publish(const std::string& event, const std::string& data = "");
    bool listening() const { return mServer.subscribers() > 0; }
    // Her own voice follows the subscriber count: with a helper listening, it
    // answers her triggers itself and two of them would talk over each other.
    // Also marks the menu stale, because Chat only belongs in it while there is
    // something to chat to.
    void updateChatter();
    // Someone wants to talk to her: the menu's Chat entry, or `asuna chat`.
    // Empty text means "open the prompt"; anything else is the first thing
    // said, so a compositor keybind can hand her a line directly.
    void openChat(const std::string& text);
    // `asuna ext …`: the settings the helper runs on, and the two-step consent
    // in front of a screen capture.
    std::string handleExt(const Json& args);
    // The pause between her looking up and the shutter. Poking her during it is
    // what calls the capture off - see handleExt.
    static gboolean onGlanceTimeout(gpointer self);
    void cancelGlance(const char* why);
    // Unmaps the layer surface, which also stops the frame clock and with it
    // every timer that hangs off it. She is not paused so much as absent.
    void setHidden(bool hidden);
    bool setLayer(const std::string& name);
    // Moves her to another monitor. "" means the compositor's first. Fails only
    // if the connector is not one of the ones plugged in right now.
    bool setOutput(const std::string& connector);
    // Re-reads the config file and applies everything state.json does not own.
    // Returns the file it read, empty if it refused; `note` gets anything the
    // user needs to hear about a setting that could not be applied live.
    //
    // `problems` refuses the reload, `warnings` does not - a setting that does
    // nothing because another one overrides it still gets applied, and is
    // reported so it is not a silent nothing. Both are filled either way, so a
    // caller that refuses still has the warnings to show.
    std::string reloadConfig(std::vector<std::string>* problems,
                             std::vector<std::string>* warnings, std::string* note);
    // The settings the config supplies that can change under a running pet:
    // called by reloadConfig, and by nothing else.
    void applyTunables();
    void sayText(const std::string& text, double seconds);
    // Where --anchor's three names put her, in device px. anchorX() is this
    // applied to the option; `asuna move left` is the same thing on demand.
    double anchorXFor(const std::string& where) const;
    // Tells whoever launched us that the socket is up, then closes the pipe:
    // the parent's read() reaching EOF without a line is how a daemon that died
    // on the way up is told apart from a slow one.
    void signalReady(const std::string& status);

    // --- personality -----------------------------------------------------
    void wireBehaviour();
    void say(const std::string& dialogueKey);
    void placeBubble();
    // placeBubble() on the next idle. Anything that moves the bubble from
    // inside a render or a resize has to go through this.
    void schedulePlaceBubble();
    bool setOutfit(const std::string& id);
    void rebuildMenu();      // marks it stale; the build happens on first open
    void openMenu();
    void quit();
    // Pointer position (logical px, widget coordinates) -> a gaze direction in
    // her own frame, and on to Behaviour.
    void aimGaze(double x, double y);
    // A tap at a point in the strip's logical coordinates.
    void poke(double x, double y);
    // The rectangle she covers on screen, in logical px with the origin at the
    // top-left of the strip - GTK's frame, not GL's. Side bleed included: with
    // her arms out she really is that wide.
    GdkRectangle bodyRect() const;
    // The same, tightened to her rendered outline. What the menu points at.
    GdkRectangle silhouetteRect() const;
    // Is a point in the parent's logical coordinates actually on her? The gaze
    // halo means an event can arrive from empty desktop beside her, so having
    // received it is no longer proof that she was touched.
    bool overHer(double x, double y) const;

    // Input region. The silhouette version is sampled from the alpha we just
    // rendered, so it tracks her actual outline including hair and ribbons;
    // applyBoxRegion() is the fallback for before the first frame.
    void applyBoxRegion();
    void sampleSilhouette();
    void applyRegion(const cairo_rectangle_int_t* rects, int count);
    void setBodyRegion(const cairo_rectangle_int_t* rects, int count);
    // Grows the region to her box plus mOpt.gazeHalo on every side, so pointer
    // motion keeps arriving after the cursor has left her outline. Only ever
    // entered from a pointer already on her, and dropped the moment it leaves
    // or clicks on something that is not her - the dead zone therefore exists
    // only while she is being touched.
    void enterHalo();
    void leaveHalo();
    // Every pointer motion, with whether it landed on her. The halo is *input*,
    // not just range: while it is up, the patch of desktop around her belongs to
    // us and the window underneath cannot be clicked. So it is kept alive only
    // by a cursor that is actually moving, and expires shortly after one stops -
    // a hand travelling past her is followed; a hand that has come to rest
    // beside her has stopped being about her, and gets its desktop back.
    void refreshHalo(bool onHer);
    // "grab" over her, "grabbing" while she is being carried, the ordinary arrow
    // anywhere else. Inside the halo the cursor is ours to draw either way, and a
    // hand over empty desktop is a promise that clicking will pick her up, which
    // it will not.
    void setCursor(const char* name);

    // Position and size are persisted, but a drag produces motion events far
    // faster than anyone wants to touch the disk, so writes are debounced.
    void scheduleSave();
    void flushSave();

    // GTK trampolines.
    static void onRealize(GtkWidget* area, gpointer self);
    static void onUnrealize(GtkWidget* area, gpointer self);
    static gboolean onRender(GtkGLArea* area, GdkGLContext* ctx, gpointer self);
    static void onMap(GtkWidget* widget, gpointer self);
    static gboolean onTick(GtkWidget* widget, GdkFrameClock* clock, gpointer self);
    static void onDragBegin(GtkGestureDrag* g, double x, double y, gpointer self);
    static void onDragUpdate(GtkGestureDrag* g, double dx, double dy, gpointer self);
    static void onDragEnd(GtkGestureDrag* g, double dx, double dy, gpointer self);
    static void onDragCancel(GtkGesture* g, GdkEventSequence* seq, gpointer self);
    static gboolean onScroll(GtkEventControllerScroll* c, double dx, double dy,
                             gpointer self);
    static gboolean onSaveTimeout(gpointer self);
    static void onPointerMotion(GtkEventControllerMotion* c, double x, double y,
                                gpointer self);
    static void onPointerLeave(GtkEventControllerMotion* c, gpointer self);
    static void onSecondaryPress(GtkGestureClick* g, int n, double x, double y,
                                 gpointer self);
    static gboolean onFarewellTimeout(gpointer self);
    static gboolean onHideTimeout(gpointer self);
    static gboolean onGreetTimeout(gpointer self);
    static gboolean onMenuTimeout(gpointer self);
    static gboolean onActionTimeout(gpointer self);
    static gboolean onMenuMoveStep(gpointer self);
    static gboolean onMenuMoveJiggle(gpointer self);
    static gboolean onQuitTimeout(gpointer self);
    static gboolean onGazeTimeout(gpointer self);
    static gboolean onGazeReport(gpointer self);
    static gboolean onScaleTimeout(gpointer self);
    static gboolean onPlaceBubbleIdle(gpointer self);
    static gboolean onPokeTimeout(gpointer self);
    static gboolean onHaloTimeout(gpointer self);
    static gboolean onStreamStart(gpointer self);
    static gboolean onStreamTimeout(gpointer self);
    // Reads the ASUNA_DEBUG_* environment and arms whatever it asks for. Called
    // from placeInitial(), so everything they poke at is a real number by then.
    void installDebugHooks();
    // SIGTERM/SIGINT, delivered on the main loop by glib-unix rather than in a
    // signal handler, so it can write the state file like any other shutdown.
    static gboolean onTerminate(gpointer self);
    // A monitor was plugged in or unplugged. The one case that matters is the
    // one she is standing on going away (see the README, "Known limits").
    static void onMonitorsChanged(GListModel* monitors, guint position,
                                  guint removed, guint added, gpointer self);

    ShellOptions mOpt;
    Pet mPet;
    Motion mMotion;
    Behaviour mBehaviour;
    Dialogue mDialogue;
    Bubble mBubble;
    Menu mMenu;
    State mState;
    ipc::Server mServer;
    daemon::Lock mLock;
    std::vector<Outfit> mOutfits;
    GtkWidget* mWindow = nullptr;
    GtkWidget* mOverlay = nullptr;
    GtkWidget* mArea = nullptr;
    GdkMonitor* mMonitor = nullptr;
    GMainLoop* mLoop = nullptr;
    int mStripWidth = 0;        // device px
    int mStripHeight = 0;       // device px, as last configured by the compositor
    int mRequestedHeight = 0;   // logical px last asked of the compositor
    int mBand = 0;              // device px reserved at the top for the bubble
    int mScaleFactor = 1;
    float mUserScale = 1.0f;
    // The largest scale the screen leaves any room for, re-solved by every
    // framing. Starts out of the way: until one has run there is no screen to
    // ask, and setUserScale takes the smaller of this and kMaxUserScale anyway.
    float mScaleCeiling = 1e9f;
    gint64 mLastFrameUs = 0;
    gint64 mStartedUs = 0;   // for `asuna status`
    bool mHidden = false;
    bool mPlaced = false;
    bool mGlFailed = false;
    bool mMenuOpen = false;
    bool mMenuStale = true;
    int mMenuStep = 0;        // ASUNA_DEBUG_MENU=move only
    bool mHalo = false;         // the gaze halo is currently claiming input
    guint mHaloTimer = 0;       // its expiry, re-armed by every motion event
    const char* mCursor = "grab";   // what mArea is currently showing
    guint mBubbleIdle = 0;      // pending deferred placeBubble()
    guint mTickId = 0;          // the frame-clock callback, dropped while hidden

    // Extensions.
    bool mWasAsleep = false;    // last published sleep state, for the edge
    bool mGlancing = false;     // she has looked up and the shutter has not fired
    bool mGlanceCancelled = false;
    guint mGlanceTimer = 0;
    gint64 mGlanceStartedUs = 0;   // so a grant can go stale rather than sit open

    // Drag state.
    bool mDragging = false;
    bool mDragMoved = false;   // has it passed the threshold, i.e. not a tap
    double mDragOriginX = 0;   // her box position when the drag began
    double mPressX = 0;        // where the press landed, logical px, for a tap
    double mPressY = 0;
    double mPointerX = 0;      // last seen pointer position, logical px
    double mPointerY = 0;
    double mDebugPokeX = 0, mDebugPokeY = 0;   // ASUNA_DEBUG_TOUCH
    int mDebugPokeEveryMs = 0;                 // ... and its optional repeat
    std::vector<std::pair<double, double>> mDebugGaze;   // ASUNA_DEBUG_GAZE
    std::vector<float> mDebugScales;           // ASUNA_DEBUG_SCALE
    std::string mDebugAction;                  // ASUNA_DEBUG_ACTION
    std::string mDebugStream;                  // ASUNA_DEBUG_STREAM
    size_t mDebugStreamAt = 0;                 // how much of it has been sent
    gint64 mLastTickUs = 0;    // for the motion model's frame delta
    gint64 mLastRenderUs = 0;  // for the gaze easing, which advances on renders

    // Input region.
    bool mRegionDirty = true;
    bool mWasMoving = false;
    gint64 mRegionSampledUs = 0;
    // Where she was standing when the region was read, device px. A silhouette
    // is a shape sampled at a position; keeping the position with it is what
    // lets silhouetteRect() carry the shape to wherever she is now.
    double mRegionSampledX = 0.0;
    float mRegionSampledLift = 0.0f;
    std::vector<unsigned char> mPixels;   // readback scratch, kept to avoid churn
    cairo_region_t* mBodyRegion = nullptr;   // her outline, without halo or menu

    guint mSaveTimer = 0;
    bool mDirty = false;   // something worth remembering has changed
};

}  // namespace asuna
