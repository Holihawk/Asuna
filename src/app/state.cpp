#include "app/state.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"
#include "paths.hpp"

namespace fs = std::filesystem;

namespace asuna {
namespace {

fs::path stateDir() { return paths::stateDir(); }

}  // namespace

std::string State::path() { return (stateDir() / "state.json").string(); }

bool State::load() {
    std::ifstream f(path());
    if (!f) return true;   // first run

    // Parsed rather than scanned. The scan this replaces looked for `"x"`
    // anywhere in the file, which meant a model path containing the text of
    // another key could answer for that key, and it stopped a string at the
    // first `"` - so a path with a quote or a backslash in it came back cut
    // short or as invalid JSON. Both are legal in a Linux filename.
    std::stringstream ss;
    ss << f.rdbuf();
    std::string reason;
    const Json doc = Json::parse(ss.str(), &reason);
    if (!doc.isObject()) {
        // Not fatal, and deliberately so: this file is disposable, and refusing
        // to start because of it would be a pet held hostage by its own
        // scratchpad. Said out loud, though - the alternative is a position and
        // an outfit that silently revert on every login.
        fprintf(stderr, "asuna: ignoring %s - %s\n", path().c_str(),
                reason.empty() ? "not a JSON object" : reason.c_str());
        return true;
    }

    // Per-field tolerance survives the change: a member that is missing or of
    // the wrong type leaves its default in place, because half a remembered
    // state beats none. Present-but-wrong is worth a line though - it is the
    // one case that means somebody edited this file and got it wrong, and a
    // setting that quietly does nothing is the thing that wastes an evening.
    std::string wrong;
    const auto shaped = [&](const char* key, Json::Type want) {
        const Json::Type got = doc[key].type();
        if (got == want) return true;
        if (got != Json::Type::Null) wrong += wrong.empty() ? key : std::string(", ") + key;
        return false;
    };

    if (shaped("x", Json::Type::Number)) x = doc["x"].asNumber();
    if (shaped("scale", Json::Type::Number)) scale = static_cast<float>(doc["scale"].asNumber());
    if (shaped("model", Json::Type::String)) model = doc["model"].asString();
    if (shaped("layer", Json::Type::String)) layer = doc["layer"].asString();
    if (shaped("output", Json::Type::String)) output = doc["output"].asString();
    if (shaped("hidden", Json::Type::Bool)) hidden = doc["hidden"].asBool() ? 1 : 0;

    if (!wrong.empty())
        fprintf(stderr, "asuna: %s: ignoring %s - wrong type\n", path().c_str(), wrong.c_str());
    return true;
}

bool State::save() const {
    std::error_code ec;
    fs::create_directories(stateDir(), ec);

    const std::string target = path();
    const std::string tmp = target + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        // Every string goes through Json::quote. A model path is a filename,
        // and a filename may legally hold a quote, a backslash or a newline;
        // written raw, any of the three produced a file that would not parse
        // and took the position and the outfit down with it.
        //
        // lround rather than (long)(x + 0.5): the two agree everywhere she
        // actually stands, but x carries -1 for "never placed", and adding a
        // half to that truncates to 0 - which reads back as "placed hard against
        // the left edge" and outranks the config's anchor for good.
        f << "{\n"
          << "  \"x\": " << std::lround(x) << ",\n"
          << "  \"scale\": " << scale << ",\n"
          << "  \"model\": " << Json::quote(model) << ",\n"
          << "  \"layer\": " << Json::quote(layer) << ",\n"
          << "  \"output\": " << Json::quote(output) << ",\n"
          << "  \"hidden\": " << (hidden > 0 ? "true" : "false") << "\n"
          << "}\n";
        if (!f) return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace asuna
