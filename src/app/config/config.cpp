#include "app/config.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "paths.hpp"

namespace asuna {

// --- the config -------------------------------------------------------------

namespace {

// "%g" rather than std::to_string, which writes 9.000000 for 9. Only used for
// the warnings, where the number is quoted back at the user and reading like
// what they typed is the whole point.
std::string plain(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// Validation shared by the enumerated strings. Rejecting rather than clamping,
// because there is no sensible nearest value for a misspelt "cetnre".
bool oneOf(const std::string& v, std::initializer_list<const char*> allowed) {
    for (const char* a : allowed)
        if (v == a) return true;
    return false;
}

}  // namespace

// The two prompts, as text rather than as a file the user has to find. Both are
// spliced into defaultText() below rather than written out a second time, so
// what `asuna config init` puts in the file and what a build with no config
// file uses cannot drift apart.
//
// Short on purpose, and the reason is the bubble: it is about twenty characters
// wide and scrolls at `bubble_rows`, so an answer in paragraphs is one read
// through a letterbox.
const char* ExtConfig::defaultPersona() {
    return u8R"(你是结城明日奈（亚丝娜），住在用户桌面上的小小陪伴角色。
说话自然、亲切、带一点关心，偶尔会调侃一句。用中文回答。
回答要短——最多两三句话，不要列表，不要 markdown，不要堆砌表情符号。
你只能看到用户告诉你的内容，不要假装知道你没被告知的事。)";
}

const char* ExtConfig::defaultGlancePrompt() {
    return u8R"(这是用户屏幕的截图，你自己所在的那一小块已经被涂掉了。
像一个坐在旁边的人那样，随口说一句关于用户正在做的事的话：关心、吐槽或者鼓励都可以。
最多两句话。如果画面上没什么值得说的，就说一句轻松的闲话。
不要罗列你看到的东西，也不要照念屏幕上的文字。)";
}

std::string Config::path() { return paths::configDir() + "/config.toml"; }

bool Config::load(const std::string& file) {
    std::ifstream f(file);
    if (!f) return true;   // no config is a perfectly good config
    std::stringstream ss;
    ss << f.rdbuf();
    source = file;
    return parse(ss.str());
}

bool Config::parse(const std::string& text) {
    Toml t;
    t.parse(text);

    // Anything out of range is reported and *ignored*, leaving the default in
    // place. Clamping silently would be answering a question the user did not
    // ask, and they would never find out.
    //
    // "Ignored" is local, though, and reads as more forgiving than it is:
    // anything pushed onto `problems` also stops the launch, because
    // applyConfig (cli/options.cpp) refuses to start on a non-empty list. So
    // the default standing here is what the value *would* have been, not what
    // the run gets - there is no run. Worth knowing before adding a check: a
    // new complaint locks out every config that currently works.
    const auto bounded = [&](const char* key, double v, double lo, double hi) {
        if (v >= lo && v <= hi) return true;
        problems.push_back(std::string("'") + key + "' must be between " +
                           std::to_string(lo) + " and " + std::to_string(hi) +
                           " - ignoring it");
        return false;
    };
    const auto atLeast = [&](const char* key, double v, double lo) {
        if (v >= lo) return true;
        problems.push_back(std::string("'") + key + "' must be at least " +
                           std::to_string(lo) + " - ignoring it");
        return false;
    };

    // [pet]
    t.str("pet.model", &model);
    if (double v = 0; t.number("pet.scale", &v))
        if (bounded("pet.scale", v, 0.5, 2.5)) scale = static_cast<float>(v);
    if (std::string v; t.str("pet.layer", &v)) {
        if (oneOf(v, {"top", "bottom", "overlay", "background"})) layer = v;
        else problems.push_back("'pet.layer' must be top, bottom, overlay or background");
    }
    if (bool v = false; t.boolean("pet.hidden", &v)) hidden = v ? 1 : 0;
    t.str("pet.output", &output);
    if (std::string v; t.str("pet.anchor", &v)) {
        if (oneOf(v, {"right", "left", "centre", "center"})) anchor = v;
        else problems.push_back("'pet.anchor' must be right, left or centre");
    }
    if (std::string v; t.str("pet.framing", &v)) {
        if (oneOf(v, {"auto", "bust", "full"})) framing = v;
        else problems.push_back("'pet.framing' must be auto, bust or full");
    }
    t.str("pet.language", &language);
    t.boolean("pet.greet", &greet);

    // [strip]
    if (int v = 0; t.integer("strip.height", &v))
        if (atLeast("strip.height", v, 80)) height = v;
    if (int v = 0; t.integer("strip.max_height", &v))
        if (atLeast("strip.max_height", v, 80)) maxHeight = v;
    if (int v = 0; t.integer("strip.margin", &v))
        if (atLeast("strip.margin", v, 0)) margin = v;
    if (int v = 0; t.integer("strip.bottom_margin", &v))
        if (atLeast("strip.bottom_margin", v, 0)) bottomMargin = v;
    if (int v = 0; t.integer("strip.pad", &v))
        if (atLeast("strip.pad", v, 0)) pad = v;
    if (int v = 0; t.integer("strip.bubble_band", &v))
        if (atLeast("strip.bubble_band", v, 0)) bubbleBand = v;
    if (double v = 0; t.number("strip.side_bleed", &v))
        if (bounded("strip.side_bleed", v, 0.0, 1.0)) sideBleed = static_cast<float>(v);
    if (int v = 0; t.integer("strip.fps", &v))
        if (atLeast("strip.fps", v, 0)) fps = v;

    // [input]
    if (int v = 0; t.integer("input.gaze_halo", &v))
        if (atLeast("input.gaze_halo", v, 0)) gazeHalo = v;

    // [bubble]
    if (double v = 0; t.number("bubble.base_seconds", &v))
        if (atLeast("bubble.base_seconds", v, 0.0)) bubbleBase = v;
    if (double v = 0; t.number("bubble.per_glyph_seconds", &v))
        if (atLeast("bubble.per_glyph_seconds", v, 0.0)) bubblePerGlyph = v;
    if (double v = 0; t.number("bubble.max_seconds", &v))
        if (atLeast("bubble.max_seconds", v, 0.5)) bubbleMax = v;

    // [behaviour]
    BehaviourTuning& b = behaviour;
    t.range("behaviour.chatter_interval", &b.chatterMin, &b.chatterMax);
    t.range("behaviour.idle_motion_interval", &b.idleGapMin, &b.idleGapMax);
    t.range("behaviour.look_interval", &b.lookGapMin, &b.lookGapMax);
    if (double v = 0; t.number("behaviour.look_range", &v))
        if (bounded("behaviour.look_range", v, 0.0, 1.0)) b.lookRange = v;
    if (double v = 0; t.number("behaviour.repeat_chance", &v))
        if (bounded("behaviour.repeat_chance", v, 0.0, 1.0)) b.repeatChance = v;
    if (double v = 0; t.number("behaviour.expression_hold", &v))
        if (atLeast("behaviour.expression_hold", v, 0.0)) b.expressionHold = v;
    if (double v = 0; t.number("behaviour.sleep_after", &v))
        if (atLeast("behaviour.sleep_after", v, 0.0)) b.sleepAfter = v;
    if (double v = 0; t.number("behaviour.drag_line_gap", &v))
        if (atLeast("behaviour.drag_line_gap", v, 0.0)) b.dragLineGap = v;
    if (double v = 0; t.number("behaviour.gaze_tau", &v))
        if (bounded("behaviour.gaze_tau", v, 0.001, 2.0)) b.gazeTau = v;

    // [ext]
    ExtConfig& e = ext;
    t.boolean("ext.enabled", &e.enabled);
    t.str("ext.command", &e.command);
    t.str("ext.prompt_command", &e.promptCommand);
    t.str("ext.prompt.persona", &e.persona);
    t.str("ext.prompt.glance", &e.glancePrompt);
    if (double v = 0; t.number("ext.temperature", &v))
        if (bounded("ext.temperature", v, 0.0, 2.0)) e.temperature = v;
    if (int v = 0; t.integer("ext.max_tokens", &v))
        if (bounded("ext.max_tokens", v, 16, 4096)) e.maxTokens = v;

    // [ext.provider.*] - every one defined, then the order to try them in.
    // Read before the priority list so a name in that list can be checked
    // against something.
    std::map<std::string, ExtProvider> defined;
    std::vector<std::string> order = t.sectionsUnder("ext.provider.");
    for (const std::string& name : order) {
        const std::string at = "ext.provider." + name + ".";
        ExtProvider p;
        p.name = name;
        // Read unconditionally, so every key in the section is marked as asked
        // for even when the section turns out not to be used - otherwise a
        // provider kept in the file but left out of `providers` would be
        // reported as a page of unknown settings.
        t.str(at + "base_url", &p.baseUrl);
        t.str(at + "model", &p.model);
        t.str(at + "api_key", &p.apiKey);
        t.str(at + "api_key_env", &p.apiKeyEnv);
        if (p.baseUrl.empty())
            problems.push_back("'ext.provider." + name + "' has no base_url");
        if (p.model.empty())
            problems.push_back("'ext.provider." + name + "' has no model");
        defined[name] = std::move(p);
    }
    // An explicit list wins; without one the file's own order is the priority,
    // which is what anyone reading it top to bottom would assume.
    if (std::vector<std::string> named; t.strings("ext.providers", &named)) order = named;
    for (const std::string& name : order) {
        auto it = defined.find(name);
        if (it == defined.end()) {
            problems.push_back("'ext.providers' names '" + name +
                               "', but there is no [ext.provider." + name + "] section");
            continue;
        }
        e.providers.push_back(it->second);
    }
    for (const auto& [name, provider] : defined)
        if (std::find(order.begin(), order.end(), name) == order.end())
            e.idleProviders.push_back(name);
    // Only once she is actually meant to use one. A file with [ext] left at its
    // defaults has no providers and needs none.
    if (e.enabled && e.providers.empty())
        problems.push_back("'ext.enabled' is true but no provider is configured - "
                           "add an [ext.provider.<name>] section with base_url and model");

    if (int v = 0; t.integer("ext.history_turns", &v))
        if (bounded("ext.history_turns", v, 0, 64)) e.historyTurns = v;
    if (int v = 0; t.integer("ext.bubble_rows", &v))
        if (bounded("ext.bubble_rows", v, 1, 24)) e.bubbleRows = v;
    // [ext.vision]
    t.boolean("ext.vision.enabled", &e.visionEnabled);
    if (double lo = 0, hi = 0; t.range("ext.vision.interval", &lo, &hi))
        // A floor rather than no bound at all: this interval decides how often
        // a program takes a picture of the screen, and a mistyped 3 instead of
        // 300 is the one typo here that would be genuinely unpleasant.
        if (atLeast("ext.vision.interval", lo, 60.0)) {
            e.visionMin = lo;
            e.visionMax = hi;
        }
    if (double v = 0; t.number("ext.vision.notice", &v))
        if (bounded("ext.vision.notice", v, 0.0, 60.0)) e.visionNotice = v;
    t.strings("ext.vision.deny", &e.visionDeny);

    // --- cross-field checks -------------------------------------------------
    //
    // Everything above validates one key against its own range, which cannot
    // see the case where two perfectly legal values contradict each other and
    // one of them quietly stops mattering. That is the failure this reader
    // exists to prevent, so it is worth saying - but as a warning, not a
    // problem: the outcome is defined, the program runs, and refusing to start
    // over a combination that has always been accepted would lock somebody out
    // of a config that works today. Each one names what actually happens.
    if (maxHeight < height)
        warnings.push_back("'strip.max_height' (" + std::to_string(maxHeight) +
                           ") is below 'strip.height' (" + std::to_string(height) +
                           ") - a full-body outfit is never shorter than the bust framing,"
                           " so max_height does nothing; raise it or lower height");
    if (bubbleMax < bubbleBase)
        warnings.push_back("'bubble.max_seconds' (" + plain(bubbleMax) +
                           ") is below 'bubble.base_seconds' (" + plain(bubbleBase) +
                           ") - every line is capped at max_seconds, so base_seconds and"
                           " per_glyph_seconds do nothing");

    t.reportUnused();
    problems.insert(problems.end(), t.problems().begin(), t.problems().end());
    return problems.empty();
}

std::string Config::defaultText() {
    // Every key this build understands, at its built-in default, commented.
    // Written by `asuna config init` and checked by the test suite: parsing this
    // must produce no problems and a Config equal to a default-constructed one,
    // which is what stops the file and the code drifting apart.
    return R"(# asuna - configuration
#
# Every setting below is at its built-in default, so this file as written
# changes nothing. Delete a line to go back to the default; delete the file to
# go back to all of them.
#
# Precedence: a command-line flag beats this file, and for the five settings
# that state.json remembers - outfit, size, position, layer, output - what you
# last did by hand beats both. So `pet.scale` here is the size she starts at on
# a machine she has never run on, not a size that overrules the one you
# scrolled to. `asuna config check` reports anything unreadable.

[pet]
model    = "02"       # outfit id from `asuna model list`, or a path to an index.json
scale    = 1.0        # 0.5 - 2.5
layer    = "top"      # top | bottom | overlay | background
hidden   = false      # start put away
output   = ""         # monitor connector from `asuna output list`; "" = the first
anchor   = "right"    # right | left | centre - the corner she starts in
framing  = "auto"     # auto | bust | full; auto asks each outfit's index.json
language = "zh"       # picks data/dialogue.<language>.json
greet    = true       # say hello on launch

[strip]
# She lives in a full-width, bottom-anchored layer-shell strip. These are its
# dimensions in logical pixels, at scale 1.0.
height        = 460   # for a bust framing; a full-body outfit asks for more
max_height    = 760   # ceiling on what it may ask for. Must be at or above
                      # height to mean anything - she is never shorter than the
                      # bust framing, so a lower ceiling is simply not used, and
                      # `asuna config check` says so
margin        = 24    # gap from the anchored screen edge
bottom_margin = 0     # gap above the bottom screen edge; 0 = flush against it
pad           = 8     # transparent margin around her inside her box
bubble_band   = 92    # strip reserved above her head for the speech bubble
side_bleed    = 0.30  # extra width drawn past each side of her box, as a
                      # fraction of it, so outstretched arms are not clipped
fps           = 30    # render cap; 0 = uncapped. The strip is full-width, so
                      # every frame is a full-width recomposite. Skipping frames
                      # takes the frame clock off vblank, so a cap above half the
                      # refresh rate judders for no real saving and is ignored:
                      # ask for 90 on a 144 Hz panel and you get 144

[input]
gaze_halo = 160       # how far past her outline the cursor is still followed,
                      # in logical px. Only claimed once she has been touched,
                      # so it is not a permanent dead zone. 0 disables it

[bubble]
# How long a line stays up: base + per_glyph x however many characters it is,
# capped. Counted in codepoints, so Chinese is not charged three times over.
# max_seconds below base_seconds caps every line at max_seconds and makes the
# other two do nothing; `asuna config check` says so.
base_seconds      = 2.0
per_glyph_seconds = 0.20
max_seconds       = 9.0

[behaviour]
# How lively she is. Intervals are [min, max] in seconds; she picks randomly in
# between each time, so nothing is ever on a metronome.
chatter_interval       = [90, 300]   # unprompted lines
idle_motion_interval   = [2.5, 8.0]  # stillness before the next idle motion
look_interval          = [3.0, 9.0]  # between idle look-around targets
look_range             = 0.45        # how far the look-around wanders, 0-1
repeat_chance          = 0.15        # chance an idle slot is a REPEAT_0x flourish
expression_hold        = 4.0         # how long a reaction face lasts
sleep_after            = 1800        # untouched seconds before she dozes off
drag_line_gap          = 20          # quiet period before carrying her is worth
                                     # commenting on again
gaze_tau               = 0.10        # gaze smoothing; smaller snaps harder

[ext]
# The extensions helper: a chat window and, if you let it, the occasional
# remark about what is on screen. It is a separate program (`asuna ext start`)
# that talks to her over the control socket, so none of this runs inside her.
enabled        = false  # the master switch; `asuna ext start` refuses without it
command        = ""     # what to run; "" = the bundled tools/asuna-ext.py
prompt_command = ""     # what asks you for a line; "" = her own prompt window,
                        # which is the one that takes Chinese input. Anything
                        # that prints a line and exits 0 works here, e.g.
                        # "fuzzel --dmenu --prompt-only=asuna> " - but fuzzel
                        # has no input-method support, so it is Latin only
history_turns  = 8      # turns she carries inside one conversation. Nothing is
                        # kept between conversations: closing the chat forgets it
bubble_rows    = 8      # rows a streamed reply may grow the bubble to. The
                        # strip reserves the height for them, so this is also
                        # how tall her speech bubble is allowed to get
temperature    = 0.8
max_tokens     = 220    # a speech bubble, not an essay

# Where to reach a model. Define as many as you like; she tries them in order
# and moves to the next one that answers, so a local model can back up a paid
# one or the other way round. `asuna ext test` reports which of them work.
#
# providers = ["openai", "local"]   # the order to try them in; without this
#                                   # line it is the order they appear below
#
# Two ways to give a key, and the difference matters. `api_key_env` names an
# environment variable the *helper* reads out of its own environment, so the
# key is never in this file, never in the daemon and never on the socket.
# `api_key` puts the key itself here, which is easier and strictly worse; this
# file is created 0600 for that reason, and `asuna config check` complains if
# that has since been loosened. Leave both empty for a local model.
#
# [ext.provider.openai]
# base_url    = "https://api.openai.com/v1"
# model       = "gpt-4o-mini"
# api_key_env = "OPENAI_API_KEY"
#
# [ext.provider.local]
# base_url = "http://127.0.0.1:11434/v1"
# model    = "qwen2.5:7b"

[ext.prompt]
# What she is, and what she is being asked. Triple quotes run to the closing
# """ and are taken literally, so write these as you would say them. Keep them
# short: the bubble is about twenty characters wide and `bubble_rows` tall.
persona = """
)" + std::string(ExtConfig::defaultPersona()) + R"(
"""
glance = """
)" + std::string(ExtConfig::defaultGlancePrompt()) + R"(
"""

[ext.vision]
# Screen capture, for the remarks about what you are doing. It uses the same
# providers as the chat - an endpoint that can see a picture is a property of
# the model, not a second thing to set up.
#
# Off, and gated twice: the helper checks this before capturing, and she
# refuses to hand over the geometry unless it is true here too, so both
# processes have to agree. Before every capture she visibly looks up, and
# poking her during the pause calls it off.
enabled  = false
interval = [900, 2700]   # seconds between glances, random in between
notice   = 3.0           # how long she looks up before the shutter
deny     = []            # window app-ids she must never look at, matched as a
                         # case-insensitive substring, so "firefox" covers
                         # org.mozilla.firefox. Read from the focused window;
                         # niri only, so on another compositor this matches
                         # nothing
)";
}

}  // namespace asuna
