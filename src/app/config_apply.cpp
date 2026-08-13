#include "app/config.hpp"

#include "app/options.hpp"

namespace asuna {

void Config::applyTo(ShellOptions* o) const {
    // The five the state file also owns go to the seed tier, not to the fields
    // the command line writes - see ShellOptions::Seed.
    o->seed.model = model;
    o->seed.scale = scale;
    o->seed.layer = layer;
    o->seed.hidden = hidden;
    o->seed.output = output;

    o->configSource = source;
    o->anchor = anchor;
    o->framing = framing;
    o->language = language;
    o->greet = greet;
    o->stripHeight = height;
    o->maxHeight = maxHeight;
    o->margin = margin;
    o->bottomMargin = bottomMargin;
    o->pad = pad;
    o->bubbleBand = bubbleBand;
    o->sideBleed = sideBleed;
    o->fps = fps;
    o->gazeHalo = gazeHalo;
    o->bubble.base = bubbleBase;
    o->bubble.perGlyph = bubblePerGlyph;
    o->bubble.max = bubbleMax;
    o->behaviour = behaviour;
    o->ext = ext;
}

}  // namespace asuna
