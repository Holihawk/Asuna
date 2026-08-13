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
// This is deliberately a GTK-free value type shared by app and UI.
struct ExtConfig {
    // The master switch. `asuna ext start` refuses without it, so a helper
    // cannot be started by accident, or by something that is not the user.
    bool enabled = false;
    std::string command;                            // "" => the bundled helper
    // What to run to ask the user for a line. "" is her own prompt window,
    // which is the only one here that can take Chinese input.
    std::string promptCommand;

    // In priority order. Chat and vision both use this one list: an endpoint
    // that can see a picture is a property of the model, not a second thing to
    // configure.
    std::vector<ExtProvider> providers;
    // Defined but not named in `providers`, so not used. Kept so `asuna ext
    // config` can expose a provider silently omitted by a misspelt priority.
    std::vector<std::string> idleProviders;

    // Cleared when the chat window closes: remembered within one conversation.
    int historyTurns = 8;
    // Rows a streamed reply may grow before it starts scrolling.
    int bubbleRows = 8;
    double temperature = 0.8;
    // Roughly eight rows of Chinese: a bubble, not an essay.
    int maxTokens = 220;

    // Kept here so tuning her voice and timings remains one configuration job.
    std::string persona = defaultPersona();
    std::string glancePrompt = defaultGlancePrompt();

    // Capture is gated by both helper and daemon; only one can see the picture.
    bool visionEnabled = false;
    double visionMin = 900, visionMax = 2700;       // s between glances
    double visionNotice = 3.0;                      // s before the shutter
    std::vector<std::string> visionDeny;            // window app-ids never captured

    // Defined in config.cpp next to the default file that documents them.
    static const char* defaultPersona();
    static const char* defaultGlancePrompt();
};

}  // namespace asuna
