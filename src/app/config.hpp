#pragma once

#include <map>
#include <string>
#include <vector>

#include "character/behaviour.hpp"

namespace asuna {

struct ShellOptions;

// One place to reach a model: `[ext.provider.<name>]`.
//
// Grouped rather than three loose keys because the three only mean anything
// together - a base_url from one service with a model name from another is not
// a configuration, it is a mistake waiting for a 404. Several may be defined,
// and the helper walks them in order until one answers; `asuna ext test` says
// which ones would.
struct ExtProvider {
    std::string name;        // the section name, and what `ext test` reports on
    std::string baseUrl;     // OpenAI-compatible, i.e. ending in /v1
    std::string model;
    // Two ways to give it a key, and the difference matters. `api_key_env`
    // names an environment variable the *helper* reads out of its own
    // environment, so the key is never in this file, never in the daemon and
    // never on the socket. `api_key` puts the literal key in the config file,
    // which is more convenient and strictly worse; `asuna config init` writes
    // the file 0600 for that reason, and `asuna config check` says so if the
    // permissions have since been loosened.
    std::string apiKey;
    std::string apiKeyEnv;
};

// The out-of-process extension helper: everything the daemon knows about it.
//
// The daemon reads these but acts on almost none of them - the helper asks for
// them over the control socket (`asuna ext config`) rather than parsing the
// file a second time. That is on purpose: one parser means one set of
// complaints, `asuna config check` validates the extension settings like any
// other, and `asuna config reload` can hand the helper new ones without
// restarting it. It is also what puts her persona in the same file as her
// idle timings, which is where anyone tuning her would look for it.
struct ExtConfig {
    // The master switch. `asuna ext start` refuses without it, so a helper
    // cannot be started by accident, or by something that is not the user.
    bool enabled = false;
    std::string command;                            // "" => the bundled helper
    // What to run to ask the user for a line. "" is her own prompt window,
    // which is the only one here that can take Chinese input - see
    // paths::extPrompt and the README.
    std::string promptCommand;

    // In priority order. Chat and vision both use this one list: an endpoint
    // that can see a picture is a property of the model, not a second thing to
    // configure.
    std::vector<ExtProvider> providers;
    // Defined but not named in `providers`, so not used. Kept so `asuna ext
    // config` can say so out loud - a provider that is silently ignored because
    // of a typo in the priority list is exactly the kind of quiet nothing this
    // config reader exists to prevent.
    std::vector<std::string> idleProviders;

    // How much of the conversation the helper carries. Cleared when the chat
    // window closes - she remembers within a conversation and not between them.
    int historyTurns = 8;
    // Rows a streamed reply may grow the bubble to before it starts scrolling.
    // The band above her head is sized from this (Bubble::bandFor), so raising
    // it makes the strip taller rather than pushing text off the top of it.
    int bubbleRows = 8;
    double temperature = 0.8;
    // A bubble, not an essay. Eight rows of about twenty characters is what she
    // can show at once, and this is roughly that much Chinese - enough that a
    // whole answer is on screen rather than scrolled halfway past.
    int maxTokens = 220;

    // What she is. Kept here rather than in the helper so that tuning her voice
    // and tuning her timings are the same job in the same file.
    std::string persona = defaultPersona();
    std::string glancePrompt = defaultGlancePrompt();

    // Screen capture. Off by default, and gated twice: the helper checks this
    // before it captures anything, and the daemon refuses to answer `ext
    // capture` unless it is true here as well. Two processes have to agree, and
    // only one of them can see the picture.
    bool visionEnabled = false;
    double visionMin = 900, visionMax = 2700;       // s between glances, random in between
    double visionNotice = 3.0;                      // s she looks up before the shutter
    std::vector<std::string> visionDeny;            // window app-ids never captured

    // The built-in text of the two prompts. Defined in config.cpp next to the
    // file that documents them, so the default and the documentation cannot
    // drift; the config test asserts they are the same string.
    static const char* defaultPersona();
    static const char* defaultGlancePrompt();
};

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
    std::vector<std::string> problems;

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
    // Defined in ui/shell.cpp, not here: this is the only part of the config that
    // needs to know what a ShellOptions is, and keeping it out of this file is
    // what lets the parser's test link a parser instead of a toolkit.
    void applyTo(ShellOptions* opt) const;

    // The commented file `asuna config init` writes. Also the documentation:
    // every key this build understands appears in it, at its default.
    static std::string defaultText();
};

}  // namespace asuna
