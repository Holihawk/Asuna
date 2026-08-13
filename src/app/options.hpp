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

    // Config values for settings also remembered in state.json. Keeping these
    // apart establishes CLI flag > state > config > built-in default.
    struct Seed {
        std::string model;
        std::string layer;
        std::string output;
        float scale = -1.0f;
        int hidden = -1;
    } seed;

    int stripHeight = 460;           // bust framing at scale 1
    int maxHeight = 760;             // ceiling for full-body outfits
    int margin = 24;                 // anchored screen-edge gap, px
    // Non-zero lifts the strip and therefore her feet above the crop line.
    int bottomMargin = 0;
    int pad = 8;
    int bubbleBand = 92;
    // Extra rendered width for poses extending beyond the standing outline.
    float sideBleed = 0.30f;
    // Logical px outside her that the pointer may still be followed; 0 disables.
    int gazeHalo = 160;
    bool greet = true;               // say hello on launch
    float scale = -1.0f;             // < 0 => take it from state, else 1.0
    double x = -1.0;                 // >= 0 => explicit pixel position
    int fps = 30;                    // render cap
    bool persist = true;             // remember state between runs
    int hidden = -1;                 // -1 => state; 0/1 => explicit override
    bool control = true;             // single-instance lock and control socket
    int readyFd = -1;                // startup handshake, -1 if nobody waits

    // Config-only tuning shared with the UI and extension helper.
    BehaviourTuning behaviour;
    BubbleTiming bubble;
    ExtConfig ext;
    // The file actually read, empty if there was none.
    std::string configSource;
};

}  // namespace asuna
