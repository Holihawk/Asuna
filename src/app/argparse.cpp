#include "app/argparse.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace asuna {
namespace argparse {
namespace {

// The separators. Space alone is what the old extension tokenizer used; a tab
// in a config file split nothing and ended up inside an argument, which is the
// kind of thing nobody debugs twice.
constexpr const char* kSpace = " \t\r\n";

bool isSpace(char c) {
    for (const char* s = kSpace; *s; ++s)
        if (c == *s) return true;
    return false;
}

// Numbers in messages, without the trailing zeroes `std::to_string` insists on.
// config.cpp reports "must be between 0.500000 and 2.500000" through
// std::to_string; there is no reason for the command line to read that way too.
std::string plain(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// Why a whole number was refused. The two are worth telling apart only in the
// message: "2147483648 is not a whole number" is a lie, and the user who typed
// it needs to be told it is too big rather than sent looking for a typo.
enum class Whole { kOk, kMalformed, kOutOfRange };

Whole readInteger(const std::string& text, int* out) {
    if (text.empty() || isSpace(text[0])) return Whole::kMalformed;
    errno = 0;
    char* end = nullptr;
    const long v = strtol(text.c_str(), &end, 10);
    // Nothing consumed, or something left over: "12px" and "1.5" are both
    // answers to a question that was not asked.
    if (end == text.c_str() || *end != '\0') return Whole::kMalformed;
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return Whole::kOutOfRange;
    *out = static_cast<int>(v);
    return Whole::kOk;
}

}  // namespace

bool integer(const std::string& text, int* out) {
    return readInteger(text, out) == Whole::kOk;
}

bool real(const std::string& text, double* out) {
    if (text.empty() || isSpace(text[0])) return false;
    errno = 0;
    char* end = nullptr;
    const double v = strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return false;
    // Catches "nan" and "inf" as typed, and "1e400", which strtod reports as
    // ERANGE and returns as HUGE_VAL. Underflow is also ERANGE but arrives as a
    // finite number near zero, which is a perfectly good answer - so the test is
    // on the value, not on errno.
    if (!std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool integerIn(const std::string& label, const std::string& text, int lo, int hi, int* out,
               std::string* error) {
    int v = 0;
    switch (readInteger(text, &v)) {
        case Whole::kMalformed:
            *error = label + " needs a whole number, not '" + text + "'";
            return false;
        case Whole::kOutOfRange:
            *error = label + " is out of range: '" + text + "'";
            return false;
        case Whole::kOk:
            break;
    }
    if (v < lo || v > hi) {
        *error = hi == kNoMax
                     ? label + " must be at least " + std::to_string(lo)
                     : label + " must be between " + std::to_string(lo) + " and " +
                           std::to_string(hi);
        return false;
    }
    *out = v;
    return true;
}

bool realIn(const std::string& label, const std::string& text, double lo, double hi, double* out,
            std::string* error) {
    double v = 0;
    if (!real(text, &v)) {
        *error = label + " needs a number, not '" + text + "'";
        return false;
    }
    if (v < lo || v > hi) {
        *error = hi >= static_cast<double>(kNoMax)
                     ? label + " must be at least " + plain(lo)
                     : label + " must be between " + plain(lo) + " and " + plain(hi);
        return false;
    }
    *out = v;
    return true;
}

bool realAny(const std::string& label, const std::string& text, double* out, std::string* error) {
    if (real(text, out)) return true;
    *error = label + " needs a number, not '" + text + "'";
    return false;
}

bool splitCommand(const std::string& text, std::vector<std::string>* argv, std::string* error) {
    argv->clear();
    std::string word;
    // Distinct from `word.empty()`: `""` is an argument, and an empty one.
    bool building = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (isSpace(c)) {
            if (building) {
                argv->push_back(word);
                word.clear();
                building = false;
            }
            continue;
        }

        if (c == '\'') {
            // Literal to the next quote. No escapes inside, which is what makes
            // '...' the way to write a path that has a backslash in it.
            const size_t end = text.find('\'', i + 1);
            if (end == std::string::npos) {
                *error = "unterminated ' quote";
                return false;
            }
            word.append(text, i + 1, end - i - 1);
            building = true;
            i = end;
            continue;
        }

        if (c == '"') {
            bool closed = false;
            for (++i; i < text.size(); ++i) {
                const char d = text[i];
                if (d == '"') {
                    closed = true;
                    break;
                }
                // Only \" and \\ mean anything here. Every other backslash is
                // kept as it stands, so a Windows-shaped path or a regex in an
                // argument does not quietly lose characters.
                if (d == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) {
                    word.push_back(text[++i]);
                    continue;
                }
                word.push_back(d);
            }
            if (!closed) {
                *error = "unterminated \" quote";
                return false;
            }
            building = true;
            continue;
        }

        if (c == '\\') {
            if (i + 1 >= text.size()) {
                *error = "trailing backslash";
                return false;
            }
            word.push_back(text[++i]);
            building = true;
            continue;
        }

        word.push_back(c);
        building = true;
    }

    if (building) argv->push_back(word);
    return true;
}

}  // namespace argparse
}  // namespace asuna
