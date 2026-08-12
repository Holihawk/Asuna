#pragma once

#include <string>

namespace asuna {

// What the user changed by hand, remembered between runs.
//
// Deliberately tiny: only things there is no other way to recover - where they
// dragged her, how big they made her, which outfit they picked. Anything
// derivable from the config or from the model itself stays out, so deleting
// this file restores defaults rather than breaking startup.
//
// Lives at $XDG_STATE_HOME/asuna/state.json (~/.local/state/asuna/state.json).
// Every field carries an "absent" value, and absent has to be distinguishable
// from a value that happens to equal the default: this file sits *above* the
// config file in the precedence order, so a scale of 1.0 that was never recorded
// must not silently outrank a config that asked for 1.4.
struct State {
    double x = -1.0;       // box left edge in device px; < 0 => never placed
    float scale = -1.0f;   // user size multiplier; < 0 => never recorded
    std::string model;     // last outfit's index.json; empty => never recorded
    // Put away with `asuna hide` or the menu. Remembered because the alternative
    // is a pet that comes back on every login after being dismissed - and
    // because hiding is the one setting whose absence is invisible, so guessing
    // it wrong is guessing it wrong silently. -1 => never recorded.
    int hidden = -1;
    // top | bottom | overlay | background. Empty means never chosen, so the
    // config file's value (or the built-in default) stands. Here rather than
    // only in the config file because `asuna layer bottom` is the user changing
    // it by hand, which is this file's whole remit.
    std::string layer;
    // Monitor connector, for the same reason as `layer`: `asuna output DP-1` is
    // a by-hand change, and a pet that walks back to the laptop screen every
    // login is a pet that has forgotten something it was told.
    std::string output;

    static std::string path();

    // Missing file is not a failure - it just means "first run", and the
    // defaults above stand. Returns false only if the file exists but could not
    // be read.
    bool load();

    // Written to a temporary and renamed, so a crash mid-write cannot leave a
    // truncated file that makes the next start fail.
    bool save() const;
};

}  // namespace asuna
