// Shell runtime state: control-socket lifecycle, monitor/layer changes, live
// configuration, persistence, signal handling, and the main loop around them.
#include "ui/shell.hpp"

#include <glib-unix.h>
#include <gtk4-layer-shell.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "paths.hpp"
#include "ui/shell/layer.hpp"

namespace asuna {

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

std::string Shell::reloadConfig(std::vector<std::string>* problems,
                                std::vector<std::string>* warnings, std::string* note) {
    Config cfg;
    cfg.load(Config::path());
    if (problems) *problems = cfg.problems;
    if (warnings) *warnings = cfg.warnings;
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
