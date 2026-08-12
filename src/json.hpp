#pragma once

#include <string>
#include <utility>
#include <vector>

namespace asuna {

// A small read-only JSON value.
//
// Three things here are JSON: each outfit's index.json (hit areas and motion
// names), the dialogue file, and the config file later. Substring scanning was
// enough while we only wanted one key out of index.json, but it silently
// mis-reads anything nested - which is exactly the class of fault that left the
// physics dead for two phases - so this parses properly.
//
// Read-only by design: nothing in the pet writes JSON. Missing members return a
// shared null value rather than throwing, so callers can chain lookups and check
// the result once.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return mType; }
    bool isNull() const { return mType == Type::Null; }
    bool isArray() const { return mType == Type::Array; }
    bool isObject() const { return mType == Type::Object; }
    bool isString() const { return mType == Type::String; }

    bool asBool(bool def = false) const { return mType == Type::Bool ? mBool : def; }
    double asNumber(double def = 0.0) const {
        return mType == Type::Number ? mNumber : def;
    }
    // Empty unless this really is a string, so a caller cannot mistake the
    // number 3 or a missing key for text.
    const std::string& asString() const;

    // Element count of an array or object; 0 for anything else.
    size_t size() const;

    const Json& operator[](size_t i) const;                // array element
    const Json& operator[](const std::string& key) const;  // object member
    const std::string& keyAt(size_t i) const;              // object member name
    const Json& valueAt(size_t i) const;

    // Both return a Null value on failure and, if `error` is non-null, put a
    // human-readable reason in it. A parse that succeeds never sets `error`.
    static Json parse(const std::string& text, std::string* error = nullptr);
    static Json parseFile(const std::string& path, std::string* error = nullptr);

private:
    Type mType = Type::Null;
    bool mBool = false;
    double mNumber = 0;
    std::string mString;
    std::vector<Json> mArray;
    // A vector rather than a map: these objects have a handful of members, the
    // lookups happen at load time, and keeping source order makes the outfit
    // registry and the motion table deterministic.
    std::vector<std::pair<std::string, Json>> mObject;

    friend class JsonParser;
};

}  // namespace asuna
