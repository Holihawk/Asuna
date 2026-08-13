#include "app/config.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace asuna {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Drops a trailing `# comment` without being fooled by a `#` inside a string -
// which matters more than it sounds, because the one string most likely to
// contain one is a path someone pasted in.
std::string stripComment(const std::string& line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) quoted = !quoted;
        else if (line[i] == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

bool isTriple(const std::string& raw) {
    return raw.size() >= 6 && raw.compare(0, 3, "\"\"\"") == 0 &&
           raw.compare(raw.size() - 3, 3, "\"\"\"") == 0;
}

bool unquote(const std::string& raw, std::string* out) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') return false;
    // `"""…"""` spanning as many lines as it likes. Here for one reason: her
    // system prompt lives in this file now, and a prompt written as a single
    // quoted line with \n in it is a prompt nobody will tune.
    if (isTriple(raw)) {
        std::string body = raw.substr(3, raw.size() - 6);
        // A newline straight after the opening quotes is a convenience for the
        // writer, not part of the text - the same rule TOML itself uses. The
        // one before the closing quotes is dropped as well, which TOML does
        // *not* do: real TOML keeps it, so
        //
        //     persona = """
        //     …
        //     """
        //
        // would end in a newline nobody typed on purpose. These are prompts,
        // written as prose, and the closing delimiter belongs on its own line;
        // charging the text for that line would be charging it for the layout.
        if (!body.empty() && body.front() == '\n') body.erase(0, 1);
        if (!body.empty() && body.back() == '\n') body.pop_back();
        if (!body.empty() && body.back() == '\r') body.pop_back();
        *out = body;
        return true;
    }
    const std::string body = raw.substr(1, raw.size() - 2);
    std::string v;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 == body.size()) {
            v += body[i];
            continue;
        }
        switch (body[++i]) {
            case 'n': v += '\n'; break;
            case 't': v += '\t'; break;
            default: v += body[i]; break;   // covers \" and \\, and is harmless
        }
    }
    *out = v;
    return true;
}

bool toNumber(const std::string& s, double* out) {
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = v;
    return true;
}

std::string at(int line) { return "line " + std::to_string(line) + ": "; }

}  // namespace

// --- the reader -------------------------------------------------------------

void Toml::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line, section;
    int number = 0;

    while (std::getline(in, line)) {
        ++number;
        const std::string t = trim(stripComment(line));
        if (t.empty()) continue;

        if (t.front() == '[') {
            if (t.size() < 3 || t.back() != ']') {
                complain(at(number) + "malformed section header '" + t + "'");
                continue;
            }
            section = trim(t.substr(1, t.size() - 2));
            continue;
        }

        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            complain(at(number) + "expected `key = value`, got '" + t + "'");
            continue;
        }
        const std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));   // grows, for a multi-line string
        if (key.empty()) {
            complain(at(number) + "no key before the '='");
            continue;
        }
        if (value.empty()) {
            complain(at(number) + "'" + key + "' has no value");
            continue;
        }
        // A `"""` that does not close on its own line runs until one does.
        // Continuation lines are taken exactly as written - no comment
        // stripping, no trimming - because inside a prompt a '#' is a '#' and
        // the indentation is the author's.
        if (value.compare(0, 3, "\"\"\"") == 0 && !isTriple(value)) {
            const int opened = number;
            bool closed = false;
            std::string more;
            while (std::getline(in, more)) {
                ++number;
                value += "\n" + more;
                const std::string end = trim(more);
                if (end.size() >= 3 && end.compare(end.size() - 3, 3, "\"\"\"") == 0) {
                    closed = true;
                    break;
                }
            }
            if (!closed) {
                complain(at(opened) + "'" + key + "' opens \"\"\" and never closes it");
                continue;
            }
        }
        const std::string full = section.empty() ? key : section + "." + key;
        if (auto it = mEntries.find(full); it != mEntries.end()) {
            complain(at(number) + "'" + full + "' was already set on line " +
                     std::to_string(it->second.line) + "; the later one wins");
        }
        mEntries[full] = Entry{value, number, false};
    }
}

Toml::Entry* Toml::find(const std::string& key) {
    auto it = mEntries.find(key);
    if (it == mEntries.end()) return nullptr;
    // Marked read even when the type turns out to be wrong: it *was* asked for,
    // and reporting it as both unusable and unknown would be reporting one
    // mistake twice.
    it->second.used = true;
    return &it->second;
}

bool Toml::str(const std::string& key, std::string* out) {
    Entry* e = find(key);
    if (!e) return false;
    if (!unquote(e->raw, out)) {
        complain(at(e->line) + "'" + key + "' should be a quoted string, got " + e->raw);
        return false;
    }
    return true;
}

bool Toml::number(const std::string& key, double* out) {
    Entry* e = find(key);
    if (!e) return false;
    if (!toNumber(e->raw, out)) {
        complain(at(e->line) + "'" + key + "' should be a number, got " + e->raw);
        return false;
    }
    return true;
}

bool Toml::integer(const std::string& key, int* out) {
    double v = 0;
    if (!number(key, &v)) return false;
    if (v != std::floor(v)) {
        Entry* e = find(key);
        complain(at(e ? e->line : 0) + "'" + key + "' should be a whole number, got " +
                 (e ? e->raw : std::string()));
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

bool Toml::boolean(const std::string& key, bool* out) {
    Entry* e = find(key);
    if (!e) return false;
    if (e->raw != "true" && e->raw != "false") {
        complain(at(e->line) + "'" + key + "' should be true or false, got " + e->raw);
        return false;
    }
    *out = e->raw == "true";
    return true;
}

bool Toml::range(const std::string& key, double* lo, double* hi) {
    Entry* e = find(key);
    if (!e) return false;
    const std::string& raw = e->raw;
    const auto bad = [&](const char* why) {
        complain(at(e->line) + "'" + key + "' " + why + ", got " + raw);
        return false;
    };
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']')
        return bad("should be two numbers in brackets, like [90, 300]");
    const std::string body = raw.substr(1, raw.size() - 2);
    const size_t comma = body.find(',');
    if (comma == std::string::npos) return bad("needs two numbers separated by a comma");
    double a = 0, b = 0;
    if (!toNumber(trim(body.substr(0, comma)), &a) ||
        !toNumber(trim(body.substr(comma + 1)), &b))
        return bad("should be two numbers");
    // Not silently sorted: an interval written backwards is a mistake, and one
    // that would otherwise produce behaviour nobody asked for.
    if (a > b) return bad("has its bounds the wrong way round");
    *lo = a;
    *hi = b;
    return true;
}

bool Toml::strings(const std::string& key, std::vector<std::string>* out) {
    Entry* e = find(key);
    if (!e) return false;
    const std::string& raw = e->raw;
    const auto bad = [&](const char* why) {
        complain(at(e->line) + "'" + key + "' " + why + ", got " + raw);
        return false;
    };
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']')
        return bad("should be quoted names in brackets, like [\"foot\", \"org.kde.kate\"]");

    std::vector<std::string> values;
    const std::string body = trim(raw.substr(1, raw.size() - 2));
    // An empty list is a real answer - "deny nothing" - and has to be
    // distinguishable from the key being absent, which is why it is not an
    // early return of false.
    for (size_t i = 0; i < body.size();) {
        if (body[i] != '"') return bad("has an entry that is not a quoted string");
        // The closing quote of this entry, skipping an escaped one.
        size_t end = i + 1;
        while (end < body.size() && !(body[end] == '"' && body[end - 1] != '\\')) ++end;
        if (end >= body.size()) return bad("has an unterminated string");
        std::string value;
        if (!unquote(body.substr(i, end - i + 1), &value)) return bad("has an unreadable entry");
        values.push_back(value);
        i = body.find_first_not_of(" \t", end + 1);
        if (i == std::string::npos) break;
        if (body[i] != ',') return bad("needs a comma between entries");
        i = body.find_first_not_of(" \t", i + 1);
        if (i == std::string::npos) return bad("has a trailing comma");
    }
    *out = std::move(values);
    return true;
}

std::vector<std::string> Toml::sectionsUnder(const std::string& prefix) const {
    // Entries live in a map, so they come out alphabetically; the line number
    // is what puts them back in the order the file was written in.
    std::vector<std::pair<int, std::string>> found;
    for (const auto& [key, entry] : mEntries) {
        if (key.compare(0, prefix.size(), prefix) != 0) continue;
        const size_t start = prefix.size();
        const size_t dot = key.find('.', start);
        if (dot == std::string::npos) continue;   // a key of the prefix itself
        const std::string name = key.substr(start, dot - start);
        if (name.empty()) continue;
        auto it = std::find_if(found.begin(), found.end(),
                               [&](const auto& p) { return p.second == name; });
        if (it == found.end()) found.emplace_back(entry.line, name);
        else it->first = std::min(it->first, entry.line);
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> names;
    names.reserve(found.size());
    for (const auto& [line, name] : found) names.push_back(name);
    return names;
}

void Toml::reportUnused() {
    for (const auto& [key, entry] : mEntries) {
        if (entry.used) continue;
        complain(at(entry.line) + "unknown setting '" + key +
                 "' - `asuna config init` lists every setting this build knows");
    }
}

}  // namespace asuna
