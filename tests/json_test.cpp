// The shared JSON reader and writer: src/json.cpp.
//
// It had no suite of its own until now - the three tests that existed rode
// along in behaviour_test.cpp, which is where the parser was first needed - and
// four things stand on it: every outfit's index.json, the dialogue file, the
// control protocol's replies, and the state file. A reader that quietly accepts
// something it should not is therefore wrong in four places at once, which is
// the argument for putting the grammar under test rather than the callers.
//
// What Phase 2.1 tightened, and why each one is here: the number path handed
// the text to strtod, which takes `+1`, `.5`, `01`, `1.`, `0x10`, `NaN` and
// `inf` - none of them JSON, all of them plausible typos, and `01` in
// particular parsed as 1 with the leading digit silently gone. The string path
// took raw control characters, so a file truncated mid-line read as a short
// value instead of an error. Everything we ship and everything we write
// ourselves already satisfied the stricter rules; nothing here changed what a
// correct document means.

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
const char* current = "";

void check(bool ok, const char* expr, int line) {
    if (ok) return;
    printf("  FAIL %s:%d  %s\n", current, line, expr);
    ++failures;
}

void checkNear(double got, double want, double tol, const char* what, int line) {
    if (std::fabs(got - want) <= tol) return;
    printf("  FAIL %s:%d  %s: got %.8g, want %.8g +- %g\n", current, line, what, got, want, tol);
    ++failures;
}

void checkEq(const std::string& got, const std::string& want, const char* what, int line) {
    if (got == want) return;
    printf("  FAIL %s:%d  %s: got \"%s\", want \"%s\"\n", current, line, what, got.c_str(),
           want.c_str());
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_NEAR(got, want, tol) checkNear((got), (want), (tol), #got, __LINE__)
#define CHECK_EQ(got, want) checkEq((got), (want), #got, __LINE__)

// Parses `text` and expects it to be refused with some reason given. Reported
// with the input in the message, because a table-driven failure that only says
// "line 214" makes you count rows.
void refuses(const char* text, int line) {
    std::string error;
    const asuna::Json j = asuna::Json::parse(text, &error);
    if (j.isNull() && !error.empty()) return;
    printf("  FAIL %s:%d  parsed [%s] that should have been refused\n", current, line, text);
    ++failures;
}

// The other half: accepted, and with the value we meant.
void accepts(const char* text, double want, int line) {
    std::string error;
    const asuna::Json j = asuna::Json::parse(text, &error);
    if (!error.empty()) {
        printf("  FAIL %s:%d  refused [%s]: %s\n", current, line, text, error.c_str());
        ++failures;
        return;
    }
    checkNear(j.asNumber(), want, 1e-9, text, line);
}

#define REFUSES(text) refuses((text), __LINE__)
#define ACCEPTS(text, want) accepts((text), (want), __LINE__)

// --- what the shipped files actually look like -----------------------------
// Moved here from behaviour_test.cpp unchanged: they are about the reader, not
// about the personality that was the first thing to need it.

void jsonReadsWhatWeActuallyParse() {
    // The two shapes that matter: index.json's hit_areas and its motions map.
    std::string error;
    const asuna::Json j = asuna::Json::parse(R"({
        "hit_areas": [{"name": "head", "id": "D_REF.PT_HEAD"}],
        "motions": {"": [{"file": "mtn/I_FUN.mtn"}], "idle": [{"file": "mtn/IDLING.mtn"}]},
        "layout": {"center_x": 0, "y": 1.3, "width": 2.9}
    })", &error);
    CHECK(error.empty());
    CHECK(j.isObject());
    CHECK_EQ(j["hit_areas"][0]["name"].asString(), "head");
    CHECK_EQ(j["motions"].keyAt(1), "idle");
    CHECK_EQ(j["motions"]["idle"][0]["file"].asString(), "mtn/IDLING.mtn");
    CHECK_NEAR(j["layout"]["y"].asNumber(), 1.3, 1e-9);

    // A missing key must be silent rather than a crash: outfits differ, and a
    // chain of lookups is how they are read.
    CHECK(j["nope"]["also-nope"][3].isNull());
    CHECK_EQ(j["nope"].asString(), "");
    CHECK_NEAR(j["layout"]["width"].asNumber(99), 2.9, 1e-9);
    CHECK_NEAR(j["layout"]["missing"].asNumber(99), 99, 1e-9);
}

void jsonRejectsRubbish() {
    std::string error;
    CHECK(asuna::Json::parse("{\"a\": }", &error).isNull());
    CHECK(!error.empty());
    error.clear();
    CHECK(asuna::Json::parse("{\"a\": 1} trailing", &error).isNull());
    CHECK(!error.empty());
}

void jsonDecodesUtf8AndEscapes() {
    const asuna::Json j = asuna::Json::parse(R"({"a": "你好", "b": "raw ä \n"})");
    CHECK_EQ(j["a"].asString(), "\xe4\xbd\xa0\xe5\xa5\xbd");   // 你好
    CHECK_EQ(j["b"].asString(), "raw \xc3\xa4 \n");
}

// --- the number grammar ----------------------------------------------------

void everyNumberJsonAllowsIsRead() {
    ACCEPTS("0", 0);
    ACCEPTS("-0", 0);
    ACCEPTS("1", 1);
    ACCEPTS("-1", -1);
    ACCEPTS("42", 42);
    ACCEPTS("1.5", 1.5);
    ACCEPTS("-1.5", -1.5);
    ACCEPTS("0.5", 0.5);
    ACCEPTS("0.0", 0);
    ACCEPTS("1e3", 1000);
    ACCEPTS("1E3", 1000);
    ACCEPTS("1e+3", 1000);
    ACCEPTS("1e-3", 0.001);
    ACCEPTS("-2.75e2", -275);
    ACCEPTS("1.3", 1.3);   // the layout value in every index.json
    // Underflow is a value, not an error: this really is zero to a double, and
    // saying so is more useful than refusing the document over it.
    ACCEPTS("1e-400", 0);
    // Whitespace around a document is still whitespace.
    ACCEPTS("  7  ", 7);
}

void numbersJsonDoesNotAllowAreRefused() {
    REFUSES("+1");     // strtod takes it; JSON has no leading plus
    REFUSES(".5");     // strtod takes it; JSON wants the zero
    REFUSES("-.5");
    REFUSES("01");     // the bad one: used to read as 1
    REFUSES("00");
    REFUSES("-01");
    REFUSES("007");
    REFUSES("1.");     // strtod takes it; JSON wants a digit after the dot
    REFUSES("1.e3");
    REFUSES("-");
    REFUSES("--1");
    REFUSES("1e");     // exponent with nothing in it
    REFUSES("1e+");
    REFUSES("1e-");
    REFUSES("0x10");   // strtod reads hex floats, so this was 16
    REFUSES("NaN");
    REFUSES("nan");
    REFUSES("Infinity");
    REFUSES("inf");
    REFUSES("-inf");
    REFUSES("1_000");
}

void aNumberTooLargeToHoldIsRefusedRatherThanBecomingInfinity() {
    // Well-formed and still not a value we can carry. It used to arrive as inf
    // and go on to be a scale, a position or a duration.
    REFUSES("1e400");
    REFUSES("-1e400");
    REFUSES("{\"scale\": 1e400}");
    REFUSES("[1, 2, 1e999]");
}

void aBadNumberInsideAStructureTakesTheDocumentWithIt() {
    // Not silently skipped: a document that half-parsed would leave the caller
    // deciding which half to trust.
    REFUSES("{\"x\": 01}");
    REFUSES("{\"x\": 1, \"y\": .5}");
    REFUSES("[1, +2, 3]");
    // And the well-formed neighbours still read.
    std::string error;
    const asuna::Json j = asuna::Json::parse("{\"x\": 0, \"y\": 1.5, \"z\": -3e2}", &error);
    CHECK_EQ(error, "");
    CHECK_NEAR(j["x"].asNumber(9), 0, 1e-9);
    CHECK_NEAR(j["y"].asNumber(), 1.5, 1e-9);
    CHECK_NEAR(j["z"].asNumber(), -300, 1e-9);
}

// --- strings ---------------------------------------------------------------

void aRawControlCharacterInAStringIsRefused() {
    // JSON requires these escaped, and Json::quote escapes them - so a raw one
    // means something else wrote the file, or it was cut short mid-line. Read
    // literally, a truncated document came back as a plausible short value.
    REFUSES("{\"a\": \"two\nlines\"}");
    REFUSES("{\"a\": \"a\tb\"}");
    REFUSES("{\"a\": \"a\rb\"}");
    REFUSES("{\"a\": \"bell\x07\"}");
    REFUSES("\"\x01\"");
    // The same characters as escapes are the correct spelling and still work.
    std::string error;
    const asuna::Json j = asuna::Json::parse("{\"a\": \"two\\nlines\", \"b\": \"a\\tb\"}", &error);
    CHECK_EQ(error, "");
    CHECK_EQ(j["a"].asString(), "two\nlines");
    CHECK_EQ(j["b"].asString(), "a\tb");
    // 0x7f is not a C0 control and JSON does not require it escaped.
    CHECK_EQ(asuna::Json::parse("\"\x7f\"").asString(), "\x7f");
}

void everythingQuoteWritesParsesBackToItself() {
    // The writer and the reader are the pair that has to agree; the stricter
    // reader must not have made anything the writer emits unreadable. Note the
    // control characters: quote escapes them, which is exactly what keeps them
    // legal now that raw ones are refused.
    const char* samples[] = {
        "plain",         "a\"b",        "a\\b",      "one\ntwo",   "tab\there",
        "cr\rhere",      "\x01\x02\x1f", "bell\x07",  "del\x7f",    "早安",
        "emoji \xF0\x9F\x91\x97", "mixed \"a\\b\" 早", "",         "/slash/",
    };
    for (const char* s : samples) {
        const std::string doc = "{\"v\": " + asuna::Json::quote(s) + "}";
        std::string error;
        const asuna::Json parsed = asuna::Json::parse(doc, &error);
        CHECK_EQ(error, "");
        CHECK_EQ(parsed["v"].asString(), s);
    }
}

void surrogatePairsAndEscapesStillDecode() {
    CHECK_EQ(asuna::Json::parse(R"("早安")").asString(), "早安");
    CHECK_EQ(asuna::Json::parse(R"("👗")").asString(), "\xF0\x9F\x91\x97");
    CHECK_EQ(asuna::Json::parse(R"("\/\b\f")").asString(), "/\b\f");
    REFUSES(R"("\q")");        // not an escape
    REFUSES(R"("\u12")");      // truncated
    REFUSES(R"("\uzzzz")");
    REFUSES("\"unterminated");
}

// --- membership ------------------------------------------------------------

void aMissingMemberAndANullMemberAreTellableApart() {
    // Both read as Null through operator[], which is why `has` exists: State
    // stays quiet about a key nobody wrote and complains about one written
    // wrong, and it cannot do both without this.
    std::string error;
    const asuna::Json j = asuna::Json::parse("{\"a\": null, \"b\": 1}", &error);
    CHECK_EQ(error, "");
    CHECK(j.has("a"));
    CHECK(j["a"].isNull());
    CHECK(j.has("b"));
    CHECK(!j.has("c"));
    CHECK(j["c"].isNull());
    // Not an object, so there is nothing to have.
    CHECK(!asuna::Json::parse("[1, 2]").has("a"));
    CHECK(!asuna::Json::parse("7").has("a"));
    CHECK(!asuna::Json().has("a"));
}

void nestingStopsBeforeItRunsOffTheStack() {
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += '[';
    for (int i = 0; i < 200; ++i) deep += ']';
    std::string error;
    CHECK(asuna::Json::parse(deep, &error).isNull());
    CHECK(!error.empty());
    // A depth we really do ship - index.json bottoms out around three.
    CHECK(!asuna::Json::parse("[[[[[1]]]]]", &error).isNull());
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"jsonReadsWhatWeActuallyParse", jsonReadsWhatWeActuallyParse},
        {"jsonRejectsRubbish", jsonRejectsRubbish},
        {"jsonDecodesUtf8AndEscapes", jsonDecodesUtf8AndEscapes},
        {"everyNumberJsonAllowsIsRead", everyNumberJsonAllowsIsRead},
        {"numbersJsonDoesNotAllowAreRefused", numbersJsonDoesNotAllowAreRefused},
        {"aNumberTooLargeToHoldIsRefusedRatherThanBecomingInfinity",
         aNumberTooLargeToHoldIsRefusedRatherThanBecomingInfinity},
        {"aBadNumberInsideAStructureTakesTheDocumentWithIt",
         aBadNumberInsideAStructureTakesTheDocumentWithIt},
        {"aRawControlCharacterInAStringIsRefused", aRawControlCharacterInAStringIsRefused},
        {"everythingQuoteWritesParsesBackToItself", everythingQuoteWritesParsesBackToItself},
        {"surrogatePairsAndEscapesStillDecode", surrogatePairsAndEscapesStillDecode},
        {"aMissingMemberAndANullMemberAreTellableApart",
         aMissingMemberAndANullMemberAreTellableApart},
        {"nestingStopsBeforeItRunsOffTheStack", nestingStopsBeforeItRunsOffTheStack},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-56s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu json tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
