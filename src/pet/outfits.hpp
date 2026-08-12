#pragma once

#include <string>
#include <vector>

#include "pet/framing.hpp"

namespace asuna {

// One costume on disk.
struct Outfit {
    std::string id;          // "02", "31" - the bare number the CLI accepts
    std::string indexJson;   // models/asuna_02/index.json
    // What the outfit asks to be cropped to, read from its own index.json. Worth
    // carrying in the registry because it is the one difference between two
    // outfits that is visible before you put one on: a full-body costume needs a
    // taller strip, so switching to one changes the shape of the whole thing.
    Framing framing = Framing::Bust;
};

// Every models/asuna_<id>/index.json under `root`, sorted by id.
//
// The registry is scanned rather than listed anywhere: dropping a folder into
// models/ is meant to be all it takes to add an outfit, which is also what makes
// the 42-outfit sweep in tests/ a real regression test rather than a fixture.
std::vector<Outfit> scanOutfits(const std::string& root);

// The models/ directory that a model path belongs to, e.g.
// "models/asuna_02/index.json" -> "models". Derived from the path in use rather
// than assumed, so the registry follows --model wherever it points.
std::string outfitsRoot(const std::string& modelJsonPath);

// "31" -> "models/asuna_31/index.json"; anything containing a separator, or too
// long to be an id, is already a path and comes back unchanged.
std::string resolveModelArg(const std::string& arg);

// The id embedded in a model path, or "" if it does not look like one.
std::string outfitId(const std::string& modelJsonPath);

}  // namespace asuna
