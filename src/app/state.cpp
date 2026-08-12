#include "app/state.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "paths.hpp"

namespace fs = std::filesystem;

namespace asuna {
namespace {

fs::path stateDir() { return paths::stateDir(); }

// The file holds four scalars written by us. A real JSON parser would be more
// code than the document has content, so scan for the key and read what follows.
// String values are taken verbatim: the only string is a filesystem path, and a
// path containing a quote or a backslash would have failed to load anyway.
bool findValue(const std::string& text, const char* key, std::string* out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = text.find(needle);
    if (p == std::string::npos) return false;
    p = text.find(':', p + needle.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && isspace(static_cast<unsigned char>(text[p]))) ++p;
    if (p >= text.size()) return false;

    if (text[p] == '"') {
        const size_t end = text.find('"', ++p);
        if (end == std::string::npos) return false;
        *out = text.substr(p, end - p);
    } else {
        const size_t end = text.find_first_of(",}\n\r \t", p);
        *out = text.substr(p, end == std::string::npos ? end : end - p);
    }
    return !out->empty();
}

}  // namespace

std::string State::path() { return (stateDir() / "state.json").string(); }

bool State::load() {
    std::ifstream f(path());
    if (!f) return true;   // first run

    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();

    std::string v;
    // A malformed value leaves the corresponding default in place rather than
    // aborting the load: half a remembered state beats none.
    if (findValue(text, "x", &v)) {
        try { x = std::stod(v); } catch (...) { x = -1.0; }
    }
    if (findValue(text, "scale", &v)) {
        try { scale = std::stof(v); } catch (...) { scale = -1.0f; }
    }
    findValue(text, "model", &model);
    findValue(text, "layer", &layer);
    findValue(text, "output", &output);
    if (findValue(text, "hidden", &v)) hidden = (v == "true" || v == "1") ? 1 : 0;
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
        f << "{\n"
          << "  \"x\": " << static_cast<long>(x + 0.5) << ",\n"
          << "  \"scale\": " << scale << ",\n"
          << "  \"model\": \"" << model << "\",\n"
          << "  \"layer\": \"" << layer << "\",\n"
          << "  \"output\": \"" << output << "\",\n"
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
