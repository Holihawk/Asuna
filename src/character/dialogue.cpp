#include "character/dialogue.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace asuna {
namespace {

constexpr const char* kFileName = "dialogue.zh.json";

// "12" -> 12, and tells the caller how many digits it consumed, so "1105-1112"
// can be read without allocating substrings.
bool readInt(const std::string& s, size_t at, size_t digits, int* out) {
    if (at + digits > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < digits; ++i) {
        const char c = s[at + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

// "season.1224" or "season.1224-1226" -> does `mmdd` fall inside it?
bool seasonMatches(const std::string& key, int mmdd) {
    constexpr size_t kPrefix = 7;   // "season."
    int from = 0;
    if (!readInt(key, kPrefix, 4, &from)) return false;
    if (key.size() == kPrefix + 4) return mmdd == from;
    if (key.size() != kPrefix + 9 || key[kPrefix + 4] != '-') return false;
    int to = 0;
    if (!readInt(key, kPrefix + 5, 4, &to)) return false;
    // A range that runs over new year (e.g. 1230-0102) is two intervals.
    if (from <= to) return mmdd >= from && mmdd <= to;
    return mmdd >= from || mmdd <= to;
}

// "time.6-10" -> does `hour` fall inside it? Bands are inclusive at both ends,
// matching how they read: 6-10 is "six through ten".
bool timeMatches(const std::string& key, int hour) {
    constexpr size_t kPrefix = 5;   // "time."
    const size_t dash = key.find('-', kPrefix);
    if (dash == std::string::npos) return false;
    char* end = nullptr;
    const long from = strtol(key.c_str() + kPrefix, &end, 10);
    if (end != key.c_str() + dash) return false;
    const long to = strtol(key.c_str() + dash + 1, &end, 10);
    if (*end != '\0') return false;
    if (from <= to) return hour >= from && hour <= to;
    return hour >= from || hour <= to;   // a band that wraps midnight
}

}  // namespace

std::string Dialogue::defaultPath(const std::string& language) {
    namespace fs = std::filesystem;
    const std::string name =
        language.empty() ? kFileName : "dialogue." + language + ".json";
    std::vector<fs::path> candidates;

    if (const char* dir = getenv("ASUNA_DATA_DIR")) candidates.emplace_back(fs::path(dir) / name);

    // Next to the binary, and one level up from it - which is where the file is
    // when running out of build/ against the source tree.
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const fs::path dir = exe.parent_path();
        candidates.push_back(dir / "data" / name);
        candidates.push_back(dir.parent_path() / "data" / name);
        candidates.push_back(dir.parent_path() / "share" / "asuna" / name);
    }
    candidates.push_back(fs::path("data") / name);   // cwd, like models/

    for (const auto& p : candidates)
        if (fs::exists(p, ec)) return p.string();
    // Only zh ships today. Falling back to it beats going silent, because a
    // language nobody has written a file for is a typo far more often than it is
    // a request, and a mute pet gives no clue which.
    if (name != kFileName) {
        fprintf(stderr, "asuna: no %s, falling back to %s\n", name.c_str(), kFileName);
        return defaultPath("zh");
    }
    return (fs::path("data") / kFileName).string();
}

bool Dialogue::load(const std::string& path) {
    std::string error;
    Json root = Json::parseFile(path, &error);
    if (!root.isObject()) {
        fprintf(stderr, "asuna: no dialogue (%s)\n",
                error.empty() ? "not an object" : error.c_str());
        return false;
    }
    mRoot = std::move(root);
    mLast.clear();
    return true;
}

std::string Dialogue::pick(const std::string& key) {
    const Json& lines = mRoot[key];
    const size_t n = lines.isArray() ? lines.size() : 0;
    if (n == 0) return "";
    if (n == 1) return lines[0].asString();

    // Choose among the other n-1, then map back around the last one used. That
    // is uniform over the alternatives and cannot repeat, without the retry loop
    // the obvious version needs.
    size_t index = std::uniform_int_distribution<size_t>(0, n - 2)(mRng);
    auto seen = mLast.find(key);
    if (seen != mLast.end() && index >= seen->second) ++index;
    mLast[key] = index;
    return lines[index].asString();
}

std::string Dialogue::greetingKey(std::time_t when) const {
    if (!mRoot.isObject()) return "";
    std::tm local{};
    if (!localtime_r(&when, &local)) return "";
    const int mmdd = (local.tm_mon + 1) * 100 + local.tm_mday;

    // A season line wins over the hour: "新年快乐" beats "早上好" on 1 January.
    for (size_t i = 0; i < mRoot.size(); ++i) {
        const std::string& key = mRoot.keyAt(i);
        if (key.compare(0, 7, "season.") == 0 && seasonMatches(key, mmdd)) return key;
    }
    for (size_t i = 0; i < mRoot.size(); ++i) {
        const std::string& key = mRoot.keyAt(i);
        if (key.compare(0, 5, "time.") == 0 && timeMatches(key, local.tm_hour)) return key;
    }
    return "";
}

std::string Dialogue::greeting(std::time_t when) {
    const std::string key = greetingKey(when);
    if (key.empty()) return "";
    std::string line = pick(key);

    const std::string token = "{year}";
    const size_t at = line.find(token);
    if (at != std::string::npos) {
        std::tm local{};
        if (localtime_r(&when, &local)) {
            char year[8];
            snprintf(year, sizeof(year), "%d", local.tm_year + 1900);
            line.replace(at, token.size(), year);
        }
    }
    return line;
}

}  // namespace asuna
