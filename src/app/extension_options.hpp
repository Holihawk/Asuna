#pragma once

#include <string>
#include <vector>

namespace asuna {

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

}  // namespace asuna
