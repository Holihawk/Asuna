#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace asuna {
namespace {

const Json kNull;
const std::string kEmpty;

// Deep enough for anything we ship (index.json bottoms out at 3), shallow enough
// that a malformed file cannot walk the stack off the end.
constexpr int kMaxDepth = 32;

void appendUtf8(std::string* out, unsigned int cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace

const std::string& Json::asString() const {
    return mType == Type::String ? mString : kEmpty;
}

size_t Json::size() const {
    if (mType == Type::Array) return mArray.size();
    if (mType == Type::Object) return mObject.size();
    return 0;
}

const Json& Json::operator[](size_t i) const {
    if (mType == Type::Array) return i < mArray.size() ? mArray[i] : kNull;
    if (mType == Type::Object) return i < mObject.size() ? mObject[i].second : kNull;
    return kNull;
}

const Json& Json::operator[](const std::string& key) const {
    if (mType != Type::Object) return kNull;
    for (const auto& kv : mObject)
        if (kv.first == key) return kv.second;
    return kNull;
}

const std::string& Json::keyAt(size_t i) const {
    if (mType != Type::Object || i >= mObject.size()) return kEmpty;
    return mObject[i].first;
}

const Json& Json::valueAt(size_t i) const { return (*this)[i]; }

// --- parser ----------------------------------------------------------------

class JsonParser {
public:
    JsonParser(const std::string& text) : mText(text) {}

    bool run(Json* out) {
        skipSpace();
        if (!parseValue(out, 0)) return false;
        skipSpace();
        if (mPos != mText.size()) return fail("trailing data");
        return true;
    }

    const std::string& error() const { return mError; }

private:
    bool fail(const char* what) {
        if (mError.empty()) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s at byte %zu", what, mPos);
            mError = buf;
        }
        return false;
    }

    void skipSpace() {
        while (mPos < mText.size()) {
            const char c = mText[mPos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++mPos;
            else
                break;
        }
    }

    bool literal(const char* word) {
        const size_t n = strlen(word);
        if (mText.compare(mPos, n, word) != 0) return false;
        mPos += n;
        return true;
    }

    bool parseValue(Json* out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        if (mPos >= mText.size()) return fail("unexpected end of input");

        switch (mText[mPos]) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                out->mType = Json::Type::String;
                return parseString(&out->mString);
            }
            case 't':
                if (!literal("true")) return fail("expected true");
                out->mType = Json::Type::Bool;
                out->mBool = true;
                return true;
            case 'f':
                if (!literal("false")) return fail("expected false");
                out->mType = Json::Type::Bool;
                out->mBool = false;
                return true;
            case 'n':
                if (!literal("null")) return fail("expected null");
                out->mType = Json::Type::Null;
                return true;
            default: return parseNumber(out);
        }
    }

    bool parseNumber(Json* out) {
        const char* begin = mText.c_str() + mPos;
        char* end = nullptr;
        const double v = strtod(begin, &end);
        if (end == begin) return fail("expected a value");
        mPos += static_cast<size_t>(end - begin);
        out->mType = Json::Type::Number;
        out->mNumber = v;
        return true;
    }

    bool parseString(std::string* out) {
        if (mPos >= mText.size() || mText[mPos] != '"') return fail("expected a string");
        ++mPos;
        out->clear();
        while (mPos < mText.size()) {
            const char c = mText[mPos++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (mPos >= mText.size()) break;
            const char esc = mText[mPos++];
            switch (esc) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!hex4(&cp)) return false;
                    // A surrogate pair is two \u escapes; the dialogue file is
                    // written as raw UTF-8, but an editor may re-encode it.
                    if (cp >= 0xD800 && cp <= 0xDBFF && mPos + 1 < mText.size() &&
                        mText[mPos] == '\\' && mText[mPos + 1] == 'u') {
                        mPos += 2;
                        unsigned int lo = 0;
                        if (!hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            appendUtf8(out, cp), cp = lo;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool hex4(unsigned int* out) {
        if (mPos + 4 > mText.size()) return fail("truncated \\u escape");
        unsigned int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = mText[mPos + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        mPos += 4;
        *out = v;
        return true;
    }

    bool parseArray(Json* out, int depth) {
        ++mPos;  // '['
        out->mType = Json::Type::Array;
        skipSpace();
        if (mPos < mText.size() && mText[mPos] == ']') {
            ++mPos;
            return true;
        }
        for (;;) {
            skipSpace();
            Json item;
            if (!parseValue(&item, depth + 1)) return false;
            out->mArray.push_back(std::move(item));
            skipSpace();
            if (mPos >= mText.size()) return fail("unterminated array");
            if (mText[mPos] == ',') { ++mPos; continue; }
            if (mText[mPos] == ']') { ++mPos; return true; }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Json* out, int depth) {
        ++mPos;  // '{'
        out->mType = Json::Type::Object;
        skipSpace();
        if (mPos < mText.size() && mText[mPos] == '}') {
            ++mPos;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (!parseString(&key)) return false;
            skipSpace();
            if (mPos >= mText.size() || mText[mPos] != ':') return fail("expected ':'");
            ++mPos;
            skipSpace();
            Json value;
            if (!parseValue(&value, depth + 1)) return false;
            out->mObject.emplace_back(std::move(key), std::move(value));
            skipSpace();
            if (mPos >= mText.size()) return fail("unterminated object");
            if (mText[mPos] == ',') { ++mPos; continue; }
            if (mText[mPos] == '}') { ++mPos; return true; }
            return fail("expected ',' or '}'");
        }
    }

    static size_t strlen(const char* s) {
        size_t n = 0;
        while (s[n]) ++n;
        return n;
    }

    const std::string& mText;
    size_t mPos = 0;
    std::string mError;
};

Json Json::parse(const std::string& text, std::string* error) {
    Json out;
    JsonParser parser(text);
    if (parser.run(&out)) return out;
    if (error) *error = parser.error();
    return Json();
}

Json Json::parseFile(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "could not open " + path;
        return Json();
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    std::string reason;
    Json out = parse(text, &reason);
    if (!reason.empty() && error) *error = path + ": " + reason;
    return out;
}

std::string Json::quote(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                // Only the C0 controls have to be escaped. Everything else goes
                // through as-is, which keeps UTF-8 - her dialogue is Chinese -
                // readable on the wire instead of a wall of \uXXXX.
                if (c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace asuna
