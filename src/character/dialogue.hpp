#pragma once

#include <ctime>
#include <random>
#include <string>
#include <unordered_map>

#include "json.hpp"

namespace asuna {

// What she says, and when.
//
// The lines live in data/dialogue.zh.json, keyed by trigger - "hit.head",
// "drag.start", "idle.chatter" - each an array of alternatives. Nothing about
// the behaviour depends on the file's contents, so the voice can be rewritten,
// translated or extended without touching code.
//
// Time and season greetings are data too: the hour bands are part of the key
// ("time.6-10"), not a table in here, so moving where "morning" ends is an edit
// to the JSON rather than a rebuild.
class Dialogue {
public:
    // A missing or malformed file is not fatal: she simply stops talking, and
    // the reason goes to stderr once.
    bool load(const std::string& path);
    bool loaded() const { return mRoot.isObject(); }

    // Where the file lives, tried in order: $ASUNA_DATA_DIR, alongside the
    // running binary (../data and ./data), then the source tree's data/.
    // Where dialogue.<language>.json is, searched the same way models/ is. Only
    // zh ships; an unknown language falls back to it rather than going silent.
    static std::string defaultPath(const std::string& language = "zh");

    // One line for a trigger, or "" if the file has nothing under that key.
    // Never repeats the previous line for the same key when there is a choice.
    std::string pick(const std::string& key);

    // The key a greeting at `when` should use: a season entry if today has one,
    // otherwise the band the hour falls in. "" if neither exists.
    std::string greetingKey(std::time_t when) const;

    // greetingKey + pick, with "{year}" replaced by the actual year.
    std::string greeting(std::time_t when);

    // Deterministic line choice, for tests.
    void seed(unsigned s) { mRng.seed(s); }

private:
    Json mRoot;
    std::unordered_map<std::string, size_t> mLast;   // last index used per key
    std::mt19937 mRng{std::random_device{}()};
};

}  // namespace asuna
