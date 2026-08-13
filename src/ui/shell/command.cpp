// Shell, the part `asuna <verb>` reaches: one function, one branch per verb,
// each returning the line the client prints. The protocol itself is
// app/ipc.cpp; this is only what the commands mean.
#include "ui/shell.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "app/config.hpp"
#include "paths.hpp"

namespace asuna {
ipc::Reply Shell::handleCommand(const Json& req) {
    const std::string cmd = req["cmd"].asString();
    const Json& args = req["args"];

    const auto text = [&args](const char* key) -> std::string {
        return args[key].isString() ? args[key].asString() : std::string();
    };
    const auto number = [&args](const char* key, bool* found) -> double {
        const bool is = args[key].type() == Json::Type::Number;
        if (found) *found = is;
        return args[key].asNumber();
    };
    // Everything that acts on her needs her on screen: the model lives in a GL
    // context that only exists while the window is mapped, and her position is
    // not decided until the compositor has told us how wide the strip is.
    const auto offScreen = [this]() -> const char* {
        if (mHidden) return "she is hidden - `asuna show` brings her back";
        if (!mPet.loaded() || !mPlaced) return "she is still starting up";
        return nullptr;
    };

    if (cmd == "ping") return ipc::ok(ipc::Out().integer("pid", getpid()).done());

    if (cmd == "status") {
        ipc::Out d;
        d.integer("pid", getpid());
        d.integer("uptime_s", (g_get_monotonic_time() - mStartedUs) / G_USEC_PER_SEC);
        d.integer("rss_kb", daemon::rssKb());
        d.boolean("hidden", mHidden);
        d.boolean("ready", mPet.loaded() && mPlaced);
        d.str("model", outfitId(mPet.modelPath()));
        d.str("model_path", mPet.modelPath());
        d.str("layer", mOpt.layer);
        const char* connector = mMonitor ? gdk_monitor_get_connector(mMonitor) : nullptr;
        d.str("output", connector ? connector : "");
        d.num("x", mMotion.placement().x);
        d.num("scale", mUserScale);
        d.integer("fps", mOpt.fps);
        d.integer("strip_width", mStripWidth);
        d.integer("strip_height", mStripHeight);
        d.boolean("asleep", mBehaviour.asleep());
        // Which config file she actually read, empty if there was none. "My
        // config is being ignored" is otherwise a guessing game, and the most
        // common answer is that it is in a directory nobody is looking at.
        d.str("config", mOpt.configSource);
        return ipc::ok(d.done());
    }

    if (cmd == "exit") {
        quit();
        return ipc::ok();
    }

    if (cmd == "hide" || cmd == "show" || cmd == "toggle") {
        setHidden(cmd == "toggle" ? !mHidden : cmd == "hide");
        return ipc::ok(ipc::Out().boolean("hidden", mHidden).done());
    }

    // Four shapes, because a line that arrives all at once and a line that
    // arrives a token at a time are genuinely different things - see
    // Bubble::hold. The plain form is unchanged and is still the common case.
    if (cmd == "say") {
        if (const char* why = offScreen()) return ipc::fail(why);
        const std::string line = text("text");
        if (args["clear"].asBool(false)) {
            mBubble.clear();
            return ipc::ok();
        }
        if (args["release"].asBool(false)) {
            if (!line.empty()) {
                placeBubble();
                mBubble.append(line);
            }
            mBubble.finish();
            return ipc::ok();
        }
        if (line.empty()) return ipc::fail("say needs some text");
        const bool append = args["append"].asBool(false);
        if (append || args["hold"].asBool(false)) {
            // Attention, once, at the start: a reply written in twenty pieces
            // must not reset her sleep countdown twenty times, and must not
            // wake her twenty times either.
            if (!append) mBehaviour.notice();
            placeBubble();
            if (append) mBubble.append(line);
            else mBubble.begin(line);
            return ipc::ok();
        }
        bool timed = false;
        const double seconds = number("seconds", &timed);
        sayText(line, timed ? seconds : 0);
        return ipc::ok();
    }

    // The tell in front of a slow answer. Not a bubble line of its own - it is
    // held, so whatever arrives next replaces it in place rather than after it.
    if (cmd == "think") {
        if (const char* why = offScreen()) return ipc::fail(why);
        const bool on = args["on"].asBool(true);
        if (on) mBehaviour.notice();
        placeBubble();
        mBubble.waiting(on);
        return ipc::ok(ipc::Out().boolean("thinking", on).done());
    }

    // The way in for everything the helper does. Deliberately not "run this
    // prompt": the daemon does not know what a prompt is. It says only that
    // someone wants to talk, and to whom.
    if (cmd == "chat") {
        if (!listening())
            return ipc::fail("nothing is listening - `asuna ext start` runs the helper");
        openChat(text("text"));
        return ipc::ok();
    }

    if (cmd == "ext") return handleExt(args);

    // The long-lived connection. Everything else here is one request and one
    // reply; this one stays open and receives events until the helper goes.
    if (cmd == "subscribe") {
        ipc::Out d;
        d.integer("pid", getpid());
        d.integer("subscribers", mServer.subscribers() + 1);
        return ipc::Reply::subscription(ipc::ok(d.done()));
    }

    if (cmd == "expression" || cmd == "motion") {
        const bool isMotion = cmd == "motion";
        if (mHidden) return ipc::fail("she is hidden - `asuna show` brings her back");
        if (!mPet.loaded()) return ipc::fail("she is still starting up");
        const std::vector<std::string>& names =
            isMotion ? mPet.motionNames() : mPet.expressionNames();
        if (args["list"].asBool(false))
            return ipc::ok(ipc::Out().raw(isMotion ? "motions" : "expressions",
                                          ipc::Out::array(names))
                               .done());
        const std::string name = text("name");
        if (name.empty()) return ipc::fail(cmd + " needs a name, or list:true");
        if (std::find(names.begin(), names.end(), name) == names.end())
            return ipc::fail("this outfit has no " + cmd + " called '" + name + "'");
        // Counted as attention either way: being played with by a script is
        // still being played with, and waking her for it is the friendlier
        // reading of `asuna motion I_FUN_S` at three in the morning. It has to
        // come first, though - this used to be a touch("") *afterwards*, which
        // is a poke, and a poke picks a reaction: every `asuna expression NAME`
        // and every face on the right-click menu ended up as F_FUN_WARM.
        mBehaviour.notice();
        if (isMotion) mPet.startMotionNamed(name, 3);
        else mBehaviour.wearExpression(name);
        return ipc::ok();
    }

    if (cmd == "move") {
        if (const char* why = offScreen()) return ipc::fail(why);
        bool given = false;
        double x = number("x", &given);
        if (!given) {
            const std::string where = text("where");
            if (where != "left" && where != "right" && where != "centre" && where != "center")
                return ipc::fail("move needs a pixel position, or left/centre/right");
            x = anchorXFor(where);
        }
        x = clampX(x);
        mMotion.glideTo(x);
        scheduleSave();
        return ipc::ok(ipc::Out().num("x", x).done());
    }

    if (cmd == "scale") {
        if (const char* why = offScreen()) return ipc::fail(why);
        bool given = false;
        const double value = number("value", &given);
        if (!given) return ipc::fail("scale needs a number");
        setUserScale(static_cast<float>(value));
        return ipc::ok(ipc::Out().num("scale", mUserScale).done());
    }

    if (cmd == "layer") {
        const std::string name = text("name");
        if (!setLayer(name))
            return ipc::fail("layer must be top, bottom, overlay or background");
        return ipc::ok(ipc::Out().str("layer", mOpt.layer).done());
    }

    if (cmd == "output") {
        if (args["list"].asBool(false)) {
            ipc::Out d;
            std::vector<std::string> names;
            if (GdkDisplay* display = gdk_display_get_default()) {
                GListModel* monitors = gdk_display_get_monitors(display);
                for (guint i = 0, n = g_list_model_get_n_items(monitors); i < n; ++i) {
                    auto* m = static_cast<GdkMonitor*>(g_list_model_get_item(monitors, i));
                    GdkRectangle geom;
                    gdk_monitor_get_geometry(m, &geom);
                    const char* name = gdk_monitor_get_connector(m);
                    // One string per monitor rather than a nested object: the
                    // writer emits flat arrays, and this is a list a human
                    // reads, not something anything computes with.
                    names.push_back(std::string(name ? name : "?") + " " +
                                    std::to_string(geom.width) + "x" +
                                    std::to_string(geom.height) + " @" +
                                    std::to_string(gdk_monitor_get_scale_factor(m)) + "x");
                    g_object_unref(m);
                }
            }
            d.raw("outputs", ipc::Out::array(names));
            const char* current = mMonitor ? gdk_monitor_get_connector(mMonitor) : nullptr;
            d.str("current", current ? current : "");
            d.str("preferred", mOpt.output);
            return ipc::ok(d.done());
        }
        const std::string name = text("name");
        if (!setOutput(name))
            return ipc::fail("no monitor called '" + name + "' - `asuna output list`");
        const char* current = mMonitor ? gdk_monitor_get_connector(mMonitor) : nullptr;
        return ipc::ok(ipc::Out().str("output", current ? current : "").done());
    }

    if (cmd == "config") {
        std::vector<std::string> problems, warnings;
        std::string note;
        const std::string where = reloadConfig(&problems, &warnings, &note);
        if (!problems.empty()) {
            ipc::Out d;
            d.raw("problems", ipc::Out::array(problems));
            // The warnings go out with the refusal too. They were collected
            // before the refusal and they are still true, and holding them back
            // would mean fixing the typo on line 12, reloading again, and only
            // then being told that max_height does nothing - two edits for one
            // sitting, when both facts were known at the same moment.
            d.raw("warnings", ipc::Out::array(warnings));
            d.str("path", Config::path());
            // Reported as a failure with the detail attached: a reload that
            // silently kept the old settings because line 12 had a typo is the
            // exact bug the parser complains about in the first place.
            return ipc::fail("config not reloaded - " +
                             std::to_string(problems.size()) + " problem(s)",
                             d.done());
        }
        // The reload did happen. The warnings ride along with the success
        // because they are about the file that was just applied, and this is
        // the only moment the person who edited it is listening.
        ipc::Out d;
        d.str("path", where);
        d.str("note", note);
        d.raw("warnings", ipc::Out::array(warnings));
        return ipc::ok(d.done());
    }

    if (cmd == "model") {
        if (args["list"].asBool(false)) {
            // Scanned on demand when she has not loaded yet, so the registry is
            // answerable while she is hidden - which is exactly when someone is
            // most likely to be looking for the id they want to come back as.
            std::vector<Outfit> outfits = mOutfits;
            if (outfits.empty()) outfits = scanOutfits(paths::modelsRoot());
            std::vector<std::string> ids, full;
            ids.reserve(outfits.size());
            for (const auto& o : outfits) {
                ids.push_back(o.id);
                if (o.framing == Framing::Full) full.push_back(o.id);
            }
            ipc::Out d;
            d.raw("models", ipc::Out::array(ids));
            // A second flat array rather than an array of objects: the writer
            // emits flat arrays, and "which of these are full-body" is the only
            // other thing the registry knows.
            d.raw("full_body", ipc::Out::array(full));
            d.str("current", outfitId(mPet.modelPath()));
            // The daemon's answer, not the client's: they can be started from
            // different directories, and only one of them has actually opened
            // the files.
            d.str("root", mPet.loaded() ? outfitsRoot(mPet.modelPath()) : paths::modelsRoot());
            return ipc::ok(d.done());
        }
        if (const char* why = offScreen()) return ipc::fail(why);
        const std::string id = text("use");
        if (id.empty()) return ipc::fail("model needs an outfit id, or list:true");
        if (!setOutfit(id)) return ipc::fail("no outfit '" + id + "' in " + paths::modelsRoot());
        return ipc::ok(ipc::Out().str("current", outfitId(mPet.modelPath())).done());
    }

    if (cmd == "menu") {
        if (const char* why = offScreen()) return ipc::fail(why);
        const std::string action = text("action");
        const bool open = action == "open" || (action == "toggle" && !mMenu.isOpen()) ||
                          action.empty();
        if (open) openMenu();
        else mMenu.close();
        return ipc::ok(ipc::Out().boolean("open", open).done());
    }

    return ipc::fail("unknown command '" + cmd + "'");
}

}  // namespace asuna
