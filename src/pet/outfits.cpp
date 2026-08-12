#include "pet/outfits.hpp"

#include <algorithm>
#include <filesystem>

#include "paths.hpp"

namespace asuna {
namespace {

namespace fs = std::filesystem;

constexpr const char* kPrefix = "asuna_";

}  // namespace

std::string outfitId(const std::string& modelJsonPath) {
    const std::string dir = fs::path(modelJsonPath).parent_path().filename().string();
    if (dir.compare(0, 6, kPrefix) != 0) return "";
    return dir.substr(6);
}

std::string outfitsRoot(const std::string& modelJsonPath) {
    const fs::path parent = fs::path(modelJsonPath).parent_path().parent_path();
    return parent.empty() ? std::string("models") : parent.string();
}

std::string resolveModelArg(const std::string& arg) {
    if (arg.empty()) return arg;
    if (arg.find('/') != std::string::npos || arg.size() > 3) return arg;
    // Against the resolved models root rather than a literal "models/": a
    // daemon started from ~/.config/autostart has no useful current directory.
    return paths::modelsRoot() + "/" + kPrefix + arg + "/index.json";
}

std::vector<Outfit> scanOutfits(const std::string& root) {
    std::vector<Outfit> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (name.compare(0, 6, kPrefix) != 0) continue;
        const fs::path index = entry.path() / "index.json";
        if (!fs::exists(index, ec)) continue;
        // One small read per outfit, 42 of them, at startup and on `model list`.
        // Cheap enough not to cache: the answer is a substring search over a
        // file the loader is about to read anyway.
        found.push_back({name.substr(6), index.string(),
                         framingFromModelJson(index.string().c_str())});
    }
    // Numeric where the ids are numbers - "asuna_10" must not sort before
    // "asuna_2" in a menu of 42 entries.
    std::sort(found.begin(), found.end(), [](const Outfit& a, const Outfit& b) {
        const bool na = !a.id.empty() && a.id.find_first_not_of("0123456789") == std::string::npos;
        const bool nb = !b.id.empty() && b.id.find_first_not_of("0123456789") == std::string::npos;
        if (na && nb) return std::stoi(a.id) < std::stoi(b.id);
        if (na != nb) return na;
        return a.id < b.id;
    });
    return found;
}

}  // namespace asuna
