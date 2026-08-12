// Shell, the part an extension can see.
//
// The whole of Phase 7 on this side of the line is three small things: events
// worth reacting to, settings worth reading, and permission to take one
// screenshot. What is deliberately *not* here is anything that talks to an API
// - no HTTP client, no API key, no prompt, no picture. That runs out of process
// (tools/asuna-ext.py) and drives her through the same control socket `asuna
// say` uses, for the reasons in the README: her main loop is also her render
// loop, so a TLS handshake or a slow first token inside it is a visible freeze,
// and a key in this address space would be a key beside a GL context and a
// socket anything on the machine can connect to.
//
// The gate in front of screen capture is the one piece of policy the daemon
// does own, and it is here rather than in the helper on purpose: a check that
// lives in the process holding the API key is a check that process could
// simply not perform.
#include "ui/shell.hpp"

#include <cstdio>

#include "paths.hpp"

namespace asuna {
namespace {

// How long a granted capture stays granted. The helper asks, she looks up, and
// it takes the picture a moment later; if it does not, permission lapses rather
// than sitting open. Generously longer than any vision_notice the config will
// accept, because the helper adds its own round trips to the wait.
constexpr double kGrantWindowS = 90.0;

// How far above her hair a prompt floats, logical px. The same gap the speech
// bubble keeps off the same head (kGap in ui/bubble.cpp), because they are the
// same gesture wearing two different windows.
constexpr int kPromptGap = 6;

}  // namespace

void Shell::publish(const std::string& event, const std::string& data) {
    if (!listening()) return;
    ipc::Out out;
    out.str("event", event);
    // The event's own fields, already encoded, hoisted into the same object -
    // so a subscriber reads `{"event":"touch","area":"head"}` rather than
    // having to look inside a nested one for a single string.
    std::string line = out.done();
    if (!data.empty() && data.size() > 2)
        line = line.substr(0, line.size() - 1) + "," + data.substr(1);
    mServer.publish(line);
}

// Called wherever the subscriber count can have changed - a helper connecting,
// a helper dying, the socket shutting down.
void Shell::updateChatter() {
    const bool listening = this->listening();
    if (listening == !mBehaviour.chatter()) return;   // already in that state
    mBehaviour.setChatter(!listening);
    // Chat belongs in the menu exactly while there is something to chat to.
    rebuildMenu();
    printf("asuna: extension %s\n", listening ? "connected" : "gone");
}

void Shell::openChat(const std::string& text) {
    if (!listening()) return;
    mBehaviour.notice();
    publish("chat", ipc::Out().str("text", text).done());
    // Only when there is nothing to answer yet: a line handed straight to
    // `asuna chat "…"` is answered in a moment, and "I'm listening" landing on
    // top of that answer would be her talking over herself.
    if (text.empty()) say("chat.open");
}

void Shell::cancelGlance(const char* why) {
    if (!mGlancing) return;
    mGlanceCancelled = true;
    if (mGlanceTimer) g_source_remove(mGlanceTimer);
    mGlanceTimer = 0;
    mGlancing = false;
    mBubble.clear();
    printf("asuna: glance called off (%s)\n", why);
    publish("glance", ipc::Out().str("state", "cancelled").done());
}

gboolean Shell::onGlanceTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->mGlanceTimer = 0;
    // Not cleared: mGlancing staying true is what makes the grant valid for the
    // next little while. The bubble goes, because the pause it was announcing
    // is over.
    self->mBubble.clear();
    return G_SOURCE_REMOVE;
}

std::string Shell::handleExt(const Json& args) {
    const ExtConfig& ext = mOpt.ext;

    // What the helper runs on: everything the config file says about it.
    //
    // `secrets` decides whether a literal `api_key` from the config file goes
    // out with it. The helper asks for them because it has to make the request;
    // `asuna ext config` does not, so neither its summary nor its --json form
    // can print a key by accident. A key given as `api_key_env` never travels
    // either way - the helper reads that variable out of its own environment.
    if (args["config"].asBool(false)) {
        const bool secrets = args["secrets"].asBool(false);
        std::vector<std::string> providers;
        for (const ExtProvider& p : ext.providers) {
            ipc::Out o;
            o.str("name", p.name);
            o.str("base_url", p.baseUrl);
            o.str("model", p.model);
            o.str("api_key_env", p.apiKeyEnv);
            o.boolean("api_key_here", !p.apiKey.empty());
            if (secrets) o.str("api_key", p.apiKey);
            providers.push_back(o.done());
        }
        ipc::Out d;
        d.boolean("enabled", ext.enabled);
        d.raw("providers", ipc::Out::rawArray(providers));
        d.raw("idle_providers", ipc::Out::array(ext.idleProviders));
        d.str("prompt_command", ext.promptCommand);
        d.integer("history_turns", ext.historyTurns);
        d.integer("bubble_rows", ext.bubbleRows);
        d.num("temperature", ext.temperature);
        d.integer("max_tokens", ext.maxTokens);
        d.str("persona", ext.persona);
        d.str("glance_prompt", ext.glancePrompt);
        d.boolean("vision_enabled", ext.visionEnabled);
        d.num("vision_min", ext.visionMin);
        d.num("vision_max", ext.visionMax);
        d.num("vision_notice", ext.visionNotice);
        d.raw("vision_deny", ipc::Out::array(ext.visionDeny));
        d.str("config", mOpt.configSource);
        return ipc::ok(d.done());
    }

    if (args["status"].asBool(false)) {
        ipc::Out d;
        d.integer("subscribers", mServer.subscribers());
        d.boolean("enabled", ext.enabled);
        d.boolean("vision_enabled", ext.visionEnabled);
        d.boolean("glancing", mGlancing);
        d.boolean("ready", mPet.loaded() && mPlaced && !mHidden);
        // Where to put a prompt so it belongs to her rather than to the screen:
        // centred on her, floating just above her hair. Logical px, which is
        // what a GTK window is placed in. The daemon answers this because it is
        // the only process that knows where she is.
        //
        // Her hair, not the top of the strip. The strip is not her: above her
        // box it also carries the lift headroom and the whole bubble band, which
        // is sized for `ext.bubble_rows` of reply rather than for anything that
        // is on screen now - 280 px of empty surface at the default size, and
        // proportionally more the larger she is scaled. Anchored to that, the
        // prompt stood a third of the way up the screen, nowhere near her, over
        // whatever window was underneath, and climbed further with every scroll
        // up. The silhouette is the same rectangle the right-click menu is
        // anchored to, for the same reason: her box is where she is allowed to
        // be, her outline is where she is.
        // The screen the prompt has to stay inside. Three things need it: the
        // first placement, so a prompt centred on a pet standing in the corner
        // is not half off the edge; the drag, so it cannot be thrown off one;
        // and the choice below of where to put it at all. Answered even when
        // she is not placed - the screen is there either way, and a prompt with
        // no anchor still has to land somewhere legal.
        GdkRectangle geom = {0, 0, 0, 0};
        if (mMonitor) gdk_monitor_get_geometry(mMonitor, &geom);
        d.integer("screen_width", geom.width);
        d.integer("screen_height", geom.height);

        if (mPlaced) {
            const GdkRectangle box = bodyRect();
            const GdkRectangle her = silhouetteRect();
            // The top of her head, in px above the bottom of the screen. Every
            // placement below is measured off it.
            const int head = mStripHeight / mScaleFactor + mOpt.bottomMargin - her.y;

            // Above her head while there is screen above her head. Scrolled up
            // far enough she stands most of the way up it, and a prompt on top
            // of that is a prompt somewhere near the ceiling - so past halfway
            // it goes beside her instead, level with her face, on whichever side
            // has more room. She is much taller than she is wide, so sideways is
            // where the space is by then.
            //
            // Her head rather than her size is the test because it is the thing
            // that actually runs out. A short screen reaches it sooner, a tall
            // one later, and neither needs a number of its own.
            const bool beside = geom.height > 0 && head > geom.height / 2;
            const int room = geom.width - (her.x + her.width);
            d.str("anchor_place", !beside ? "above" : (room >= her.x ? "right" : "left"));
            if (!beside) {
                // Centred on her box, not on her outline: the framing centres
                // her in her box, so the box centre is her centre and stays put
                // while she breathes, where the outline's centre wanders with an
                // arm. Vertically the box top is meaningless - it is headroom -
                // and the outline top is exactly the top of her head.
                d.integer("anchor_x", box.x + box.width / 2);
                d.integer("anchor_bottom", head + kPromptGap);
            } else {
                // The near edge of her outline, and her head to hang it level
                // with. The prompt puts its own far edge on the first and its
                // middle on the second - it is the only one that knows how big
                // it is.
                d.integer("anchor_x", room >= her.x ? her.x + her.width + kPromptGap
                                                    : her.x - kPromptGap);
                d.integer("anchor_bottom", head);
            }
        }
        return ipc::ok(d.done());
    }

    if (args["cancel"].asBool(false)) {
        cancelGlance("asked to");
        // Not only the glance: `asuna ext cancel` is also the way to stop a
        // reply that is on its way, and the helper is the only one that can do
        // that. It hears about it here.
        mBubble.clear();
        publish("cancel");
        return ipc::ok();
    }

    // --- the two steps in front of a screenshot ---------------------------
    //
    // Split in two so that the pause between them is a real one, on screen, in
    // front of the user, rather than a number in a config file that nobody can
    // see the effect of. `glance` makes her look up and say so; `capture` is
    // the shutter, and it refuses if anything happened in between.
    if (args["glance"].asBool(false)) {
        if (!ext.enabled) return ipc::fail("extensions are off - `ext.enabled` in the config");
        if (!ext.visionEnabled)
            return ipc::fail("she is not allowed to look at the screen -"
                             " `ext.vision_enabled` in the config");
        if (const char* why = mHidden ? "she is hidden" : nullptr) return ipc::fail(why);
        if (!mPet.loaded() || !mPlaced) return ipc::fail("she is still starting up");

        mGlancing = true;
        mGlanceCancelled = false;
        mGlanceStartedUs = g_get_monotonic_time();
        // Awake, looking up and out of the way of her own bubble. This is the
        // whole of the notice: it has to be something the user can catch out of
        // the corner of an eye and stop, which is what the touch handler does.
        mBehaviour.notice();
        mBehaviour.lookAt(0.0, 1.0);
        say("glance");
        if (mGlanceTimer) g_source_remove(mGlanceTimer);
        mGlanceTimer = g_timeout_add(static_cast<guint>(ext.visionNotice * 1000),
                                     onGlanceTimeout, this);
        publish("glance", ipc::Out().str("state", "open").done());
        return ipc::ok(ipc::Out().num("wait_ms", ext.visionNotice * 1000).done());
    }

    if (args["capture"].asBool(false)) {
        // Both switches again, not only at `glance`: a config reload can turn
        // vision off between the two, and the answer to "may I look" has to be
        // the one that was true at the moment of looking.
        if (!ext.enabled || !ext.visionEnabled)
            return ipc::fail("she is not allowed to look at the screen");
        if (mGlanceCancelled) return ipc::fail("the glance was called off");
        if (!mGlancing) return ipc::fail("no glance was announced - `ext glance` comes first");
        if (mGlanceTimer) return ipc::fail("the notice is not over yet");
        const double openS = (g_get_monotonic_time() - mGlanceStartedUs) / 1e6;
        if (openS > kGrantWindowS) {
            mGlancing = false;
            return ipc::fail("that glance has gone stale - ask again");
        }
        if (mHidden) return ipc::fail("she is hidden");
        mGlancing = false;

        ipc::Out d;
        const char* connector = mMonitor ? gdk_monitor_get_connector(mMonitor) : nullptr;
        d.str("output", connector ? connector : "");
        d.integer("scale", mScaleFactor);

        // Where she is in the screenshot the helper is about to take. grim
        // captures one whole output in device pixels with its origin at the
        // output's top-left; her box is in the strip's frame, and the strip is
        // glued to the bottom edge - so the strip's own top is the difference.
        GdkRectangle geom = {0, 0, 0, 0};
        if (mMonitor) gdk_monitor_get_geometry(mMonitor, &geom);
        const int stripTop = geom.height - mStripHeight / mScaleFactor - mOpt.bottomMargin;

        // Her outline and her bubble, together: the bubble is the more
        // important of the two to mask, because it holds her own last sentence
        // and feeding that back as "what is on screen" is how a model ends up
        // talking to itself.
        GdkRectangle box = bodyRect();
        if (GdkRectangle bubble; mBubble.rect(&bubble)) gdk_rectangle_union(&box, &bubble, &box);
        ipc::Out mask;
        mask.integer("x", box.x * mScaleFactor);
        mask.integer("y", (stripTop + box.y) * mScaleFactor);
        mask.integer("w", box.width * mScaleFactor);
        mask.integer("h", box.height * mScaleFactor);
        d.raw("mask", mask.done());
        d.raw("deny", ipc::Out::array(ext.visionDeny));

        // On the record. A program that takes pictures of the screen should
        // leave a trail in a file the user already reads, not be something they
        // have to take on trust.
        printf("asuna: screen capture granted on %s, masking %dx%d at (%d,%d)\n",
               connector ? connector : "?", box.width, box.height, box.x, stripTop + box.y);
        fflush(stdout);
        return ipc::ok(d.done());
    }

    return ipc::fail("ext takes config, status, glance, capture or cancel");
}

}  // namespace asuna
