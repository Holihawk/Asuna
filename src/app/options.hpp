#pragma once

#include <string>

#include "app/bubble_timing.hpp"
#include "app/extension_options.hpp"
#include "character/behaviour.hpp"

namespace asuna {

// Everything settable before the UI starts. This is an application interface,
// so it must remain usable without GTK include paths or compiler flags.
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
    // Written "ok" (or "err: …") once the socket is listening, then closed,
    // so `asuna start` can stop waiting. -1 when nobody is waiting.
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

}  // namespace asuna
