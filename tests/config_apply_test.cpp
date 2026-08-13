#include <cstdio>

#include "app/config.hpp"
#include "app/options.hpp"

namespace {

int gFailures = 0;

template <typename T>
void checkEq(const T& got, const T& want, const char* field) {
    if (got == want) return;
    std::printf("  FAIL  %s was not mapped\n", field);
    ++gFailures;
}

void testMapping() {
    asuna::Config c;
    c.model = "model";
    c.scale = 1.4f;
    c.layer = "overlay";
    c.hidden = 1;
    c.output = "DP-1";
    c.source = "/tmp/config.toml";
    c.anchor = "left";
    c.framing = "full";
    c.language = "en";
    c.greet = false;
    c.height = 501;
    c.maxHeight = 777;
    c.margin = 31;
    c.bottomMargin = 7;
    c.pad = 11;
    c.bubbleBand = 103;
    c.sideBleed = 0.42f;
    c.fps = 47;
    c.gazeHalo = 211;
    c.bubbleBase = 3.1;
    c.bubblePerGlyph = 0.31;
    c.bubbleMax = 12.5;
    c.behaviour.idleGapMin = 101;
    c.ext.enabled = true;
    c.ext.command = "helper --flag";

    asuna::ShellOptions o;
    o.model = "cli model";
    o.scale = 2.0f;
    o.layer = "bottom";
    o.hidden = 0;
    o.output = "CLI-1";
    c.applyTo(&o);

    checkEq(o.seed.model, c.model, "seed.model");
    checkEq(o.seed.scale, c.scale, "seed.scale");
    checkEq(o.seed.layer, c.layer, "seed.layer");
    checkEq(o.seed.hidden, c.hidden, "seed.hidden");
    checkEq(o.seed.output, c.output, "seed.output");
    checkEq(o.configSource, c.source, "configSource");
    checkEq(o.anchor, c.anchor, "anchor");
    checkEq(o.framing, c.framing, "framing");
    checkEq(o.language, c.language, "language");
    checkEq(o.greet, c.greet, "greet");
    checkEq(o.stripHeight, c.height, "stripHeight");
    checkEq(o.maxHeight, c.maxHeight, "maxHeight");
    checkEq(o.margin, c.margin, "margin");
    checkEq(o.bottomMargin, c.bottomMargin, "bottomMargin");
    checkEq(o.pad, c.pad, "pad");
    checkEq(o.bubbleBand, c.bubbleBand, "bubbleBand");
    checkEq(o.sideBleed, c.sideBleed, "sideBleed");
    checkEq(o.fps, c.fps, "fps");
    checkEq(o.gazeHalo, c.gazeHalo, "gazeHalo");
    checkEq(o.bubble.base, c.bubbleBase, "bubble.base");
    checkEq(o.bubble.perGlyph, c.bubblePerGlyph, "bubble.perGlyph");
    checkEq(o.bubble.max, c.bubbleMax, "bubble.max");
    checkEq(o.behaviour.idleGapMin, c.behaviour.idleGapMin, "behaviour");
    checkEq(o.ext.enabled, c.ext.enabled, "ext.enabled");
    checkEq(o.ext.command, c.ext.command, "ext.command");

    // Mapping supplies the config tier without overwriting the CLI tier.
    checkEq(o.model, std::string("cli model"), "CLI model precedence");
    checkEq(o.scale, 2.0f, "CLI scale precedence");
    checkEq(o.layer, std::string("bottom"), "CLI layer precedence");
    checkEq(o.hidden, 0, "CLI hidden precedence");
    checkEq(o.output, std::string("CLI-1"), "CLI output precedence");
}

}  // namespace

int main() {
    testMapping();
    std::printf(gFailures ? "%d mapping failure(s)\n" : "all config mapping tests passed\n",
                gFailures);
    return gFailures ? 1 : 0;
}
