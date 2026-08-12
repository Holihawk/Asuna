// The config file, asserted without a compositor.
//
// Two things are being checked, and the second is the one that matters.
//
// The first is that the reader reads: sections, quoting, numbers, booleans,
// intervals, comments. That is ordinary parser testing.
//
// The second is that it *complains*. A config file is a thing someone writes on
// purpose and then watches for the effect of, so the failure mode that costs
// real time is not a crash - it is a misspelt key that silently changes nothing
// while the user rereads their own file wondering what they got wrong. Most of
// the cases below are misspellings, wrong types and out-of-range values, and
// each asserts that a problem was reported rather than merely that the value
// was not applied.
//
// And one structural check: parsing what `asuna config init` writes must produce
// no problems and a Config identical to a default-constructed one. That is what
// stops the documented defaults and the compiled-in defaults drifting apart -
// the file claims to be "every setting at its default", and this is the claim.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "paths.hpp"

namespace {

int gFailures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        printf("  FAIL  %s\n", what.c_str());
        ++gFailures;
    }
}

template <typename T>
void checkEq(const T& got, const T& want, const std::string& what) {
    if (got != want) {
        std::string g, w;
        if constexpr (std::is_same_v<T, std::string>) { g = got; w = want; }
        else { g = std::to_string(got); w = std::to_string(want); }
        printf("  FAIL  %s\n        got  %s\n        want %s\n", what.c_str(), g.c_str(),
               w.c_str());
        ++gFailures;
    }
}

// 1e-6, not 1e-9: several of these fields are float, so a literal like 0.4 has
// already lost more than a double's worth of precision on the way in.
void checkNear(double got, double want, const std::string& what) {
    if (got < want - 1e-6 || got > want + 1e-6) {
        printf("  FAIL  %s\n        got  %g\n        want %g\n", what.c_str(), got, want);
        ++gFailures;
    }
}

// Parses and asserts a clean read, returning the config.
asuna::Config good(const std::string& text, const std::string& what) {
    asuna::Config c;
    c.parse(text);
    if (!c.problems.empty()) {
        printf("  FAIL  %s: unexpected problems\n", what.c_str());
        for (const auto& p : c.problems) printf("        %s\n", p.c_str());
        ++gFailures;
    }
    return c;
}

// Parses and asserts that something was reported, and that the reported text
// mentions `mentions` - a complaint that does not name the key it is about is
// only half a complaint.
void bad(const std::string& text, const std::string& mentions, const std::string& what) {
    asuna::Config c;
    const bool ok = c.parse(text);
    if (ok || c.problems.empty()) {
        printf("  FAIL  %s: parsed clean, expected a complaint about '%s'\n", what.c_str(),
               mentions.c_str());
        ++gFailures;
        return;
    }
    for (const auto& p : c.problems)
        if (p.find(mentions) != std::string::npos) return;
    printf("  FAIL  %s: complained, but never mentioned '%s'\n", what.c_str(),
           mentions.c_str());
    for (const auto& p : c.problems) printf("        %s\n", p.c_str());
    ++gFailures;
}

// --- reading ----------------------------------------------------------------

void testReading() {
    printf("reading\n");

    const asuna::Config c = good(R"(
# a comment on its own line
[pet]
model  = "31"        # and one after a value
anchor = "left"
greet  = false

[strip]
height     = 500
side_bleed = 0.4
fps        = 0

[behaviour]
chatter_interval = [120, 600]
sleep_after      = 900
)",
                                 "a whole file");

    checkEq(c.model, std::string("31"), "model");
    checkEq(c.anchor, std::string("left"), "anchor");
    check(!c.greet, "greet = false");
    checkEq(c.height, 500, "strip.height");
    checkNear(c.sideBleed, 0.4, "side_bleed");
    checkEq(c.fps, 0, "fps = 0 is uncapped, not missing");
    checkNear(c.behaviour.chatterMin, 120, "chatter min");
    checkNear(c.behaviour.chatterMax, 600, "chatter max");
    checkNear(c.behaviour.sleepAfter, 900, "sleep_after");

    // Untouched keys keep the built-in defaults, which is the property that
    // makes a two-line config file a legitimate config file.
    const asuna::Config d;
    checkEq(c.maxHeight, d.maxHeight, "an unmentioned key keeps its default");
    checkNear(c.behaviour.idleGapMin, d.behaviour.idleGapMin, "so does an unmentioned interval");

    // A `#` inside a string is not a comment. The value most likely to contain
    // one is a path someone pasted in, so getting this wrong truncates exactly
    // the setting that is hardest to notice being truncated.
    const asuna::Config h = good("[pet]\nmodel = \"/tmp/a#b/index.json\"\n", "hash in a string");
    checkEq(h.model, std::string("/tmp/a#b/index.json"), "hash inside quotes survives");

    // Whitespace, blank lines and a section repeated later in the file.
    const asuna::Config w = good("  [strip]  \n\n  pad   =   12  \n\n[pet]\nscale = 1.5\n"
                                 "\n[strip]\nmargin = 40\n",
                                 "loose whitespace and a reopened section");
    checkEq(w.pad, 12, "pad through whitespace");
    checkEq(w.margin, 40, "a section reopened later still lands");
    checkNear(w.scale, 1.5, "scale");

    // An empty file, and a file of nothing but comments, are both valid.
    good("", "an empty file");
    good("# nothing but this\n\n# and this\n", "comments only");
}

// --- complaining ------------------------------------------------------------

void testComplaints() {
    printf("complaining\n");

    // The one that costs real time: a key that is almost right.
    bad("[behaviour]\nchater_interval = [90, 300]\n", "chater_interval", "misspelt key");
    bad("[pet]\nscale = 1.5\n[strip]\nheigth = 500\n", "heigth", "misspelt key in a later section");
    // A key in the wrong section is the same mistake wearing a hat.
    bad("[pet]\nheight = 500\n", "pet.height", "right key, wrong section");
    bad("fps = 30\n", "fps", "a key with no section at all");

    // Wrong types.
    bad("[pet]\nmodel = 31\n", "pet.model", "unquoted string");
    bad("[strip]\nheight = \"500\"\n", "strip.height", "quoted number");
    bad("[pet]\ngreet = yes\n", "pet.greet", "yes is not a boolean");
    bad("[strip]\nheight = 460.5\n", "strip.height", "a fraction where a whole number goes");
    bad("[strip]\nfps = 30fps\n", "strip.fps", "trailing junk after a number");

    // Enumerated strings, where there is no sensible nearest value.
    bad("[pet]\nanchor = \"cetnre\"\n", "anchor", "misspelt anchor");
    bad("[pet]\nlayer = \"middle\"\n", "layer", "no such layer");
    bad("[pet]\nframing = \"waist\"\n", "framing", "no such framing");

    // Ranges. Out of range is reported and ignored, not clamped: clamping would
    // answer a question the user did not ask, silently.
    bad("[pet]\nscale = 9.0\n", "scale", "scale above the maximum");
    bad("[strip]\nside_bleed = 2.0\n", "side_bleed", "side_bleed above 1");
    bad("[strip]\nmargin = -5\n", "margin", "a negative margin");

    asuna::Config c;
    c.parse("[pet]\nscale = 9.0\n");
    checkNear(c.scale, -1.0, "a rejected value leaves the field untouched");

    // Intervals.
    bad("[behaviour]\nchatter_interval = 90\n", "chatter_interval", "an interval that is one number");
    bad("[behaviour]\nchatter_interval = [300, 90]\n", "chatter_interval", "an interval backwards");
    bad("[behaviour]\nchatter_interval = [90]\n", "chatter_interval", "an interval of one");
    bad("[behaviour]\nchatter_interval = [a, b]\n", "chatter_interval", "an interval of words");

    // Syntax.
    bad("[pet\nmodel = \"31\"\n", "section", "unterminated section header");
    bad("[pet]\nmodel\n", "key = value", "a line with no equals sign");
    bad("[pet]\nmodel =\n", "no value", "a key with nothing after the equals");
    bad("[pet]\nmodel = \"31\"\nmodel = \"02\"\n", "already set", "the same key twice");

    // Everything wrong at once still reports everything, rather than stopping at
    // the first: fixing a config file one error per run is a miserable loop.
    asuna::Config many;
    many.parse("[pet]\nanchor = \"nowhere\"\nnonsense = 1\n[strip]\nheight = \"tall\"\n");
    check(many.problems.size() >= 3, "three mistakes are reported as three, not one");
}

// --- the file `config init` writes ------------------------------------------

void testDefaultText() {
    printf("the default file\n");

    const std::string text = asuna::Config::defaultText();
    asuna::Config written;
    const bool clean = written.parse(text);
    if (!clean) {
        printf("  FAIL  `asuna config init` writes a file this build rejects\n");
        for (const auto& p : written.problems) printf("        %s\n", p.c_str());
        ++gFailures;
        return;
    }

    // It claims every setting is at its default. Check the claim - if a default
    // moves in the code and not in the file, this is what says so.
    const asuna::Config d;
    checkEq(written.anchor, d.anchor, "anchor default matches");
    checkEq(written.framing, d.framing, "framing default matches");
    checkEq(written.language, d.language, "language default matches");
    check(written.greet == d.greet, "greet default matches");
    checkEq(written.height, d.height, "strip.height default matches");
    checkEq(written.maxHeight, d.maxHeight, "strip.max_height default matches");
    checkEq(written.margin, d.margin, "strip.margin default matches");
    checkEq(written.bottomMargin, d.bottomMargin, "strip.bottom_margin default matches");
    checkEq(written.pad, d.pad, "strip.pad default matches");
    checkEq(written.bubbleBand, d.bubbleBand, "strip.bubble_band default matches");
    checkNear(written.sideBleed, d.sideBleed, "strip.side_bleed default matches");
    checkEq(written.fps, d.fps, "strip.fps default matches");
    checkEq(written.gazeHalo, d.gazeHalo, "input.gaze_halo default matches");
    checkNear(written.bubbleBase, d.bubbleBase, "bubble.base_seconds default matches");
    checkNear(written.bubblePerGlyph, d.bubblePerGlyph, "bubble.per_glyph_seconds default matches");
    checkNear(written.bubbleMax, d.bubbleMax, "bubble.max_seconds default matches");

    const asuna::BehaviourTuning& b = written.behaviour;
    const asuna::BehaviourTuning& e = d.behaviour;
    checkNear(b.chatterMin, e.chatterMin, "chatter min default matches");
    checkNear(b.chatterMax, e.chatterMax, "chatter max default matches");
    checkNear(b.idleGapMin, e.idleGapMin, "idle gap min default matches");
    checkNear(b.idleGapMax, e.idleGapMax, "idle gap max default matches");
    checkNear(b.lookGapMin, e.lookGapMin, "look gap min default matches");
    checkNear(b.lookGapMax, e.lookGapMax, "look gap max default matches");
    checkNear(b.lookRange, e.lookRange, "look range default matches");
    checkNear(b.repeatChance, e.repeatChance, "repeat chance default matches");
    checkNear(b.expressionHold, e.expressionHold, "expression hold default matches");
    checkNear(b.sleepAfter, e.sleepAfter, "sleep after default matches");
    checkNear(b.dragLineGap, e.dragLineGap, "drag line gap default matches");
    checkNear(b.gazeTau, e.gazeTau, "gaze tau default matches");

    // The two that decide whether a program may talk to the network and take a
    // picture of the screen. If the file ever comes to say `true` where the
    // code says false, this is the line that catches it - and it is the one
    // drift in this file that would matter beyond a wrong number.
    const asuna::ExtConfig& x = written.ext;
    const asuna::ExtConfig& y = d.ext;
    check(x.enabled == y.enabled, "ext.enabled default matches (and is off)");
    check(!x.enabled, "the written file does not enable extensions");
    check(x.visionEnabled == y.visionEnabled, "ext.vision.enabled default matches");
    check(!x.visionEnabled, "the written file does not enable vision");
    checkEq(x.historyTurns, y.historyTurns, "ext.history_turns default matches");
    checkEq(x.bubbleRows, y.bubbleRows, "ext.bubble_rows default matches");
    checkEq(x.maxTokens, y.maxTokens, "ext.max_tokens default matches");
    checkNear(x.temperature, y.temperature, "ext.temperature default matches");
    checkNear(x.visionMin, y.visionMin, "ext.vision.interval low end default matches");
    checkNear(x.visionMax, y.visionMax, "ext.vision.interval high end default matches");
    checkNear(x.visionNotice, y.visionNotice, "ext.vision.notice default matches");
    check(x.visionDeny.empty(), "the written deny list is empty, like the default");
    // The prompts are spliced into the file from the same constants the
    // defaults come from, so this cannot fail by drift - only by the splice
    // being broken, which is exactly what it is here to catch.
    checkEq(x.persona, y.persona, "the written persona is the built-in one");
    checkEq(x.glancePrompt, y.glancePrompt, "and the written glance prompt too");
    // The file ships with every provider commented out, so a fresh install has
    // nowhere to send anything until someone decides where.
    check(x.providers.empty(), "the written file configures no provider");

    // The four the state file owns must be *present* in the written file, since
    // it doubles as the documentation - but they are seeds, so their being set
    // here is not the same as their taking effect.
    check(text.find("model") != std::string::npos, "the default file mentions model");
    check(text.find("output") != std::string::npos, "the default file mentions output");
    check(written.scale > 0, "the default file sets a scale");
    checkEq(written.model, std::string("02"), "the default file names the default outfit");
}

// --- the extension settings -------------------------------------------------
//
// Worth a section of its own for one reason: two of these keys decide whether a
// program may take a picture of the screen, and the mode of failure that
// matters is not a crash but a value that quietly does not mean what it says.
// So the checks here are mostly about what happens when the file is *wrong*.

void testExt() {
    printf("ext\n");
    const asuna::Config c = good(R"(
[ext]
enabled        = true
providers      = ["local", "paid"]
history_turns  = 4
bubble_rows    = 6
temperature    = 0.4
max_tokens     = 120

[ext.provider.paid]
base_url    = "https://api.openai.com/v1"
model       = "gpt-4o-mini"
api_key_env = "OPENAI_API_KEY"

[ext.provider.local]
base_url = "http://127.0.0.1:11434/v1"
model    = "qwen2.5:7b"

[ext.vision]
enabled  = true
interval = [600, 1200]
notice   = 5.0
deny     = ["org.keepassxc.KeePassXC", "firefox"]
)",
                                 "a full [ext] section");
    check(c.ext.enabled, "enabled");
    checkEq(c.ext.historyTurns, 4, "history turns");
    checkEq(c.ext.bubbleRows, 6, "bubble rows");
    checkNear(c.ext.temperature, 0.4, "temperature");
    checkEq(c.ext.maxTokens, 120, "max tokens");

    // The priority list wins over the order the sections are written in - the
    // whole point of naming it - so `local` comes first even though `paid` is
    // the earlier section.
    checkEq(c.ext.providers.size(), size_t{2}, "both providers are read");
    if (c.ext.providers.size() == 2) {
        checkEq(c.ext.providers[0].name, std::string("local"), "the list sets the order");
        checkEq(c.ext.providers[0].baseUrl, std::string("http://127.0.0.1:11434/v1"),
                "base_url");
        checkEq(c.ext.providers[0].model, std::string("qwen2.5:7b"), "model");
        check(c.ext.providers[0].apiKey.empty() && c.ext.providers[0].apiKeyEnv.empty(),
              "a local provider needs no key at all");
        checkEq(c.ext.providers[1].name, std::string("paid"), "and the second");
        checkEq(c.ext.providers[1].apiKeyEnv, std::string("OPENAI_API_KEY"),
                "the key is named, not given");
    }
    check(c.ext.idleProviders.empty(), "both are in use");

    check(c.ext.visionEnabled, "vision enabled");
    checkNear(c.ext.visionMin, 600, "vision interval low end");
    checkNear(c.ext.visionMax, 1200, "vision interval high end");
    checkNear(c.ext.visionNotice, 5.0, "vision notice");
    checkEq(c.ext.visionDeny.size(), size_t{2}, "two names on the deny list");
    if (c.ext.visionDeny.size() == 2) {
        checkEq(c.ext.visionDeny[0], std::string("org.keepassxc.KeePassXC"), "first denied");
        checkEq(c.ext.visionDeny[1], std::string("firefox"), "second denied");
    }

    // Without a priority list, the file's own order is the priority. Anyone
    // reading the file top to bottom would assume that, so it had better be
    // true.
    const asuna::Config implied = good(R"(
[ext.provider.first]
base_url = "http://a/v1"
model    = "a"

[ext.provider.second]
base_url = "http://b/v1"
model    = "b"
)",
                                       "providers with no priority list");
    checkEq(implied.ext.providers.size(), size_t{2}, "both are used");
    if (implied.ext.providers.size() == 2) {
        checkEq(implied.ext.providers[0].name, std::string("first"), "in file order");
        checkEq(implied.ext.providers[1].name, std::string("second"), "not alphabetically");
    }

    // A provider defined but left out of the list is a legitimate way to park
    // one. It must not be a problem, and it must not be silent either - the
    // name is kept so `asuna ext config` can say it is being skipped.
    const asuna::Config parked = good(R"(
[ext]
providers = ["a"]

[ext.provider.a]
base_url = "http://a/v1"
model    = "a"

[ext.provider.b]
base_url = "http://b/v1"
model    = "b"
)",
                                      "a provider left out of the list");
    checkEq(parked.ext.providers.size(), size_t{1}, "only the named one is used");
    checkEq(parked.ext.idleProviders.size(), size_t{1}, "and the other is reported as idle");

    // The mistakes worth catching, all of them silent in a config reader that
    // did not check: a name in the list with no section behind it, a section
    // missing the two keys that make it reachable, and extensions switched on
    // with nothing to reach.
    bad("[ext]\nproviders = [\"nowhere\"]\n", "nowhere",
        "a priority list naming a section that does not exist");
    bad("[ext.provider.x]\nmodel = \"m\"\n", "base_url", "a provider with no base_url");
    bad("[ext.provider.x]\nbase_url = \"http://a/v1\"\n", "model",
        "a provider with no model");
    bad("[ext]\nenabled = true\n", "ext.enabled",
        "extensions switched on with nothing to reach");

    // An empty list is a real answer - "deny nothing" - and has to parse as one
    // rather than as a mistake.
    const asuna::Config empty = good("[ext.vision]\ndeny = []\n", "an empty deny list");
    check(empty.ext.visionDeny.empty(), "denies nothing, and is not a mistake");

    // A list that is not a list of strings. The failure this guards against is
    // a bare word being taken as a name and silently denying nothing, which
    // would leave someone believing their password manager was excluded.
    bad("[ext.vision]\ndeny = [firefox]\n", "vision.deny",
        "an unquoted entry in the deny list");
    bad("[ext.vision]\ndeny = [\"a\" \"b\"]\n", "vision.deny",
        "a missing comma in the deny list");
    bad("[ext.vision]\ndeny = [\"a\", ]\n", "vision.deny",
        "a trailing comma in the deny list");
    bad("[ext.vision]\ndeny = [\"a]\n", "vision.deny",
        "an unterminated string in the deny list");

    // The floor under the glance interval: a mistyped 3 for 300 would be a
    // program taking a screenshot every three seconds.
    bad("[ext.vision]\ninterval = [3, 9]\n", "vision.interval",
        "an implausibly short glance interval");
    asuna::Config hasty;
    hasty.parse("[ext.vision]\ninterval = [3, 9]\n");
    const asuna::Config d;
    checkNear(hasty.ext.visionMin, d.ext.visionMin, "and the default interval stands");

    // The two switches must default to off in the compiled-in defaults, not
    // only in the file: a build with no config file at all is the state most
    // people are in, and it has to be the quiet one.
    check(!d.ext.enabled, "extensions are off by default");
    check(!d.ext.visionEnabled, "vision is off by default");
    check(d.ext.providers.empty(), "with nowhere to send anything");
    check(d.ext.visionDeny.empty(), "and nothing on the deny list");
}

// --- prompts in the config file ---------------------------------------------
//
// Her persona lives in config.toml so that tuning her voice and tuning her
// timings are the same job in the same place. That needs multi-line strings,
// which the reader did not have.

void testMultiLineStrings() {
    printf("triple-quoted strings\n");
    const asuna::Config c =
        good("[ext.prompt]\npersona = \"\"\"\nline one\nline two\n\"\"\"\n",
             "a triple-quoted string");
    checkEq(c.ext.persona, std::string("line one\nline two"),
            "it keeps its newlines, and drops the layout ones at each end");

    // Taken literally, which is the whole reason for having it: a prompt is
    // prose, and a '#' in it is a '#'.
    const asuna::Config hash =
        good("[ext.prompt]\npersona = \"\"\"\nuse # for headings\n\"\"\"\n",
             "a '#' inside a triple-quoted string, which is not a comment");
    checkEq(hash.ext.persona, std::string("use # for headings"), "and survives intact");

    const asuna::Config oneLine =
        good("[ext.prompt]\nglance = \"\"\"all on one line\"\"\"\n",
             "triple quotes that open and close on one line");
    checkEq(oneLine.ext.glancePrompt, std::string("all on one line"), "read as that line");

    // Unterminated is the case that would otherwise eat the rest of the file
    // in silence - every setting after it would simply stop working.
    bad("[ext.prompt]\npersona = \"\"\"\nand then nothing\n", "persona",
        "a triple quote that never closes");

    // The defaults are the same string in the code and in the written file.
    const asuna::Config d;
    checkEq(d.ext.persona, std::string(asuna::ExtConfig::defaultPersona()) ,
            "the built-in persona is the documented one");
    check(!d.ext.persona.empty(), "and she has one at all");
}

// --- where it lives ---------------------------------------------------------

void testPath() {
    printf("path\n");
    setenv("XDG_CONFIG_HOME", "/tmp/asuna-config-test", 1);
    checkEq(asuna::Config::path(), std::string("/tmp/asuna-config-test/asuna/config.toml"),
            "config.toml under $XDG_CONFIG_HOME");

    // A file that is not there is not an error: no config is the state most
    // people are in, and it has to behave exactly like a file of defaults.
    asuna::Config missing;
    check(missing.load("/tmp/asuna-config-test/definitely-not-here.toml"),
          "a missing file loads clean");
    check(missing.source.empty(), "and reports no source");
    const asuna::Config d;
    checkEq(missing.height, d.height, "and leaves every default in place");
}

}  // namespace

int main() {
    testReading();
    testComplaints();
    testDefaultText();
    testExt();
    testMultiLineStrings();
    testPath();

    printf(gFailures ? "\n%d failure(s)\n" : "\nall config tests passed\n", gFailures);
    return gFailures ? 1 : 0;
}
