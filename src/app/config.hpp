#pragma once

#include <map>
#include <string>
#include <vector>

#include "character/behaviour.hpp"
#include "app/extension_options.hpp"

namespace asuna {

struct ShellOptions;

// A very small TOML reader: `[section]` headers, `key = value`, `#` comments,
// double-quoted strings, numbers, booleans, and one-line arrays of numbers.
//
// Not a general TOML parser and not trying to be. This file is read once at
// startup by a program that already hand-rolls its JSON reader, and pulling in a
// dependency for thirty keys would be a poor trade. What it does do properly is
// *complain*: an unreadable value, a malformed line and - the one that matters -
// a key nobody ever asked for are all reported by name and line number. A typo
// in a config file is otherwise the most silent bug there is, because the
// program carries on perfectly with the default you were trying to change.
class Toml {
public:
    void parse(const std::string& text);

    // Each returns false and leaves *out alone if the key is absent or unusable,
    // having recorded why in problems(). "section.key" addressing.
    bool str(const std::string& key, std::string* out);
    bool number(const std::string& key, double* out);
    bool integer(const std::string& key, int* out);
    bool boolean(const std::string& key, bool* out);
    // `key = [lo, hi]`, the shape every interval in [behaviour] uses. Rejects a
    // reversed pair rather than quietly sorting it: an interval written the
    // wrong way round is a mistake worth hearing about.
    bool range(const std::string& key, double* lo, double* hi);
    // `key = ["a", "b"]`, and `[]` for none. Used for the window deny list and
    // the provider priority order, both settings where "it silently parsed as
    // empty" is the worst outcome available - so an unreadable entry is a
    // problem, not a skipped element.
    bool strings(const std::string& key, std::vector<std::string>* out);

    // The distinct `[<prefix>.<name>]` sections in the file, in the order they
    // appear in it. That order is meaningful: it is the fallback priority for
    // the API providers when the file does not give one explicitly, and reading
    // a file top to bottom is how anyone would guess it.
    std::vector<std::string> sectionsUnder(const std::string& prefix) const;

    // Keys that were never asked for - i.e. that this build does not know.
    // Called after all the reads, which is what makes it meaningful.
    void reportUnused();

    const std::vector<std::string>& problems() const { return mProblems; }

private:
    struct Entry {
        std::string raw;
        int line = 0;
        bool used = false;
    };
    Entry* find(const std::string& key);
    void complain(const std::string& message) { mProblems.push_back(message); }

    std::map<std::string, Entry> mEntries;
    std::vector<std::string> mProblems;
};

// Everything settable before she starts, and the personality numbers.
//
// Defaults here are the built-in defaults, and they must agree with
// ShellOptions and BehaviourTuning - the whole file being absent has to behave
// exactly like the file saying what it says. `asuna config check` on the output
// of `asuna config init` is the test that keeps that true.
struct Config {
    // [pet] - the four the state file also remembers are "unset" sentinels,
    // because config supplies a *starting* value that state then overrides.
    std::string model;                // "" => unset
    float scale = -1.0f;              // < 0 => unset
    std::string layer;                // "" => unset
    int hidden = -1;                  // -1 => unset
    std::string output;               // "" => the compositor's first monitor
    std::string anchor = "right";
    std::string framing = "auto";
    std::string language = "zh";
    bool greet = true;

    // [strip]
    int height = 460;
    int maxHeight = 760;
    int margin = 24;
    int bottomMargin = 0;
    int pad = 8;
    int bubbleBand = 92;
    float sideBleed = 0.30f;
    int fps = 30;

    // [input]
    int gazeHalo = 160;

    // [bubble]
    double bubbleBase = 2.0;
    double bubblePerGlyph = 0.20;
    double bubbleMax = 9.0;

    // [behaviour]
    BehaviourTuning behaviour;

    // [ext]
    ExtConfig ext;

    // Where it came from, empty if there was no file. Reported by `asuna status`
    // so "my config is being ignored" is answerable without guessing.
    std::string source;
    // Fatal. Anything in here stops the launch: applyConfig (cli/options.cpp) returns
    // kUsage on a non-empty list, and `config reload` refuses rather than
    // half-applying. A value that could not be read or is out of range is one
    // of these, because the user wrote it on purpose and is watching for it.
    std::vector<std::string> problems;
    // Not fatal. Settings that parse, are in range, and still do nothing -
    // because some *other* setting overrides them. The program runs exactly as
    // it did, and the line is there so the setting that does nothing is not a
    // silent nothing.
    //
    // A separate list rather than more `problems` on purpose. These are
    // cross-field contradictions that have always been accepted, so making one
    // fatal would refuse to start a config that works today, over a combination
    // that has a defined and reasonable outcome. `problems` is for what cannot
    // be honoured; this is for what is honoured in a way the user did not mean.
    //
    // These are statements about *this file*, not about the run. They are
    // computed from the file alone, before a single command-line flag is looked
    // at, so both of these hold:
    //
    //   asuna start --max-height 1000   still warns about a file whose
    //                                   max_height is below its height
    //   asuna start --height 900        does not warn, though the effective
    //                                   height now exceeds max_height
    //
    // That is the intended contract and not an oversight. A flag is one launch;
    // the file is what gets read every login, and it is the thing the user goes
    // back and edits. Reporting on the effective options instead would mean
    // validating after the flags are parsed and tracking which source supplied
    // each value - a real feature, and a different one. `asuna config check`
    // reads no flags at all and gives the same answer, which is only coherent
    // because the answer is about the file.
    std::vector<std::string> warnings;

    static std::string path();

    // Missing file is not a failure: it means "no config", which is a valid
    // state and the one most people are in. Returns false only if the file is
    // there and could not be read or made sense of.
    bool load(const std::string& file);
    bool parse(const std::string& text);

    // Applies to a fresh ShellOptions, *before* the command line is parsed, so
    // a flag naturally wins. The five settings state.json owns go to opt->seed
    // instead - see the comment there.
    //
    // Defined in config_apply.cpp, a GTK-free mapping unit kept separate from
    // config/config.cpp so parser-only users need not link application option mapping.
    void applyTo(ShellOptions* opt) const;

    // The commented file `asuna config init` writes. Also the documentation:
    // every key this build understands appears in it, at its default.
    static std::string defaultText();
};

}  // namespace asuna
