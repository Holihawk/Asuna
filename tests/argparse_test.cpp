// Tests for src/app/argparse.cpp: strict numeric parsing, and the quoting
// grammar the configured extension commands are split by.
//
// Both used to be the loose thing. Numbers went through atoi/atof, where "nope"
// is 0 and "1.2.3" is 1.2, so `asuna scale nope` shrank her to nothing and
// `asuna move .` sent her to the left edge - neither with a word of complaint.
// Commands were split on literal spaces, so a helper living in a directory with
// a space in its name could not be named at all.
//
// The tokenizer cases are the ones `python3 -c 'import shlex'` produces for the
// same input. That is not a coincidence and not decoration: `[ext] command` is
// read by this file and `[ext] prompt_command` by shlex.split in asuna-ext.py,
// and one config line meaning two different things depending on which end read
// it is the bug this pins down.

#include "app/argparse.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
const char* current = "";

void check(bool ok, const char* expr, int line) {
    if (ok) return;
    printf("  FAIL %s:%d  %s\n", current, line, expr);
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using asuna::argparse::kNoMax;

// --- helpers ---------------------------------------------------------------

// Parses, and reports what came back, so a failure names the input.
void wants(const std::string& text, int want, int line) {
    int got = 0;
    if (!asuna::argparse::integer(text, &got)) {
        printf("  FAIL %s:%d  integer(\"%s\") was rejected, wanted %d\n", current, line,
               text.c_str(), want);
        ++failures;
        return;
    }
    if (got != want) {
        printf("  FAIL %s:%d  integer(\"%s\") = %d, wanted %d\n", current, line, text.c_str(),
               got, want);
        ++failures;
    }
}

void rejects(const std::string& text, int line) {
    int got = 12345;
    if (asuna::argparse::integer(text, &got)) {
        printf("  FAIL %s:%d  integer(\"%s\") was accepted as %d\n", current, line, text.c_str(),
               got);
        ++failures;
    }
}

void wantsReal(const std::string& text, double want, int line) {
    double got = 0;
    if (!asuna::argparse::real(text, &got)) {
        printf("  FAIL %s:%d  real(\"%s\") was rejected, wanted %g\n", current, line, text.c_str(),
               want);
        ++failures;
        return;
    }
    if (got != want) {
        printf("  FAIL %s:%d  real(\"%s\") = %g, wanted %g\n", current, line, text.c_str(), got,
               want);
        ++failures;
    }
}

void rejectsReal(const std::string& text, int line) {
    double got = 12345;
    if (asuna::argparse::real(text, &got)) {
        printf("  FAIL %s:%d  real(\"%s\") was accepted as %g\n", current, line, text.c_str(),
               got);
        ++failures;
    }
}

#define WANTS(text, want) wants((text), (want), __LINE__)
#define REJECTS(text) rejects((text), __LINE__)
#define WANTS_REAL(text, want) wantsReal((text), (want), __LINE__)
#define REJECTS_REAL(text) rejectsReal((text), __LINE__)

// Tokenises, and compares against the argv shlex.split produces for the same
// string. `want` empty means "no arguments at all", which is not the same as an
// argv holding one empty string - see emptyArgumentsSurvive.
void splits(const std::string& text, const std::vector<std::string>& want, int line) {
    std::vector<std::string> got;
    std::string error;
    if (!asuna::argparse::splitCommand(text, &got, &error)) {
        printf("  FAIL %s:%d  splitCommand(\"%s\") failed: %s\n", current, line, text.c_str(),
               error.c_str());
        ++failures;
        return;
    }
    if (got == want) return;
    printf("  FAIL %s:%d  splitCommand(\"%s\") gave %zu:", current, line, text.c_str(),
           got.size());
    for (const std::string& w : got) printf(" [%s]", w.c_str());
    printf("  wanted %zu:", want.size());
    for (const std::string& w : want) printf(" [%s]", w.c_str());
    printf("\n");
    ++failures;
}

void splitFails(const std::string& text, int line) {
    std::vector<std::string> got;
    std::string error;
    if (asuna::argparse::splitCommand(text, &got, &error)) {
        printf("  FAIL %s:%d  splitCommand(\"%s\") should have been refused\n", current, line,
               text.c_str());
        ++failures;
        return;
    }
    if (error.empty()) {
        printf("  FAIL %s:%d  splitCommand(\"%s\") refused without saying why\n", current, line,
               text.c_str());
        ++failures;
    }
}

#define SPLITS(text, ...) splits((text), __VA_ARGS__, __LINE__)
#define SPLIT_FAILS(text) splitFails((text), __LINE__)

// --- whole numbers ---------------------------------------------------------

void wholeNumbersAreRead() {
    WANTS("0", 0);
    WANTS("30", 30);
    WANTS("460", 460);
    WANTS("-12", -12);
    WANTS("+12", 12);
    // Leading zeroes are decimal, not octal. `--fps 030` is 30 frames.
    WANTS("030", 30);
}

void malformedWholeNumbersAreRefused() {
    REJECTS("");
    REJECTS("nope");
    REJECTS("-");
    REJECTS("+");
    REJECTS(".");
    // The atoi behaviour this replaces: a number with something stuck to it
    // used to read as the number and drop the rest.
    REJECTS("12px");
    REJECTS("460 ");
    REJECTS(" 460");
    REJECTS("4 6 0");
    REJECTS("1.5");
    REJECTS("1e3");
    REJECTS("0x10");
    REJECTS("--");
}

void wholeNumberOverflowIsRefusedNotWrapped() {
    WANTS("2147483647", 2147483647);
    WANTS("-2147483648", -2147483648);
    // Fits in the long strtol returns, but not in the int it is asked for.
    // Truncating would land on -2147483648, which is a plausible-looking
    // number and completely wrong.
    REJECTS("2147483648");
    REJECTS("-2147483649");
    REJECTS("99999999999999999999");
    REJECTS("-99999999999999999999");
}

// --- real numbers ----------------------------------------------------------

void realNumbersAreRead() {
    WANTS_REAL("0", 0.0);
    WANTS_REAL("1", 1.0);
    WANTS_REAL("1.5", 1.5);
    WANTS_REAL("-1.5", -1.5);
    WANTS_REAL("0.30", 0.30);
    WANTS_REAL("1e3", 1000.0);
    WANTS_REAL(".5", 0.5);
}

void malformedRealNumbersAreRefused() {
    REJECTS_REAL("");
    REJECTS_REAL("nope");
    // The two from the report, both of which used to reach the daemon: a bare
    // dot as 0, and a version-shaped string as its first component.
    REJECTS_REAL(".");
    REJECTS_REAL("1.2.3");
    REJECTS_REAL("-");
    REJECTS_REAL("1.5x");
    REJECTS_REAL(" 1.5");
    REJECTS_REAL("1.5 ");
}

void nonFiniteRealsAreRefused() {
    // strtod reads all of these happily. None of them is an answer to "how far
    // across the screen" or "how big".
    REJECTS_REAL("nan");
    REJECTS_REAL("NaN");
    REJECTS_REAL("-nan");
    REJECTS_REAL("inf");
    REJECTS_REAL("INF");
    REJECTS_REAL("-inf");
    REJECTS_REAL("infinity");
    // Overflow arrives as HUGE_VAL, which is the same problem by another route.
    REJECTS_REAL("1e400");
    REJECTS_REAL("-1e400");
}

// --- ranges, and what they say ---------------------------------------------

void rangesAreInclusiveAtBothEnds() {
    int n = 0;
    std::string error;
    CHECK(asuna::argparse::integerIn("--height", "80", 80, kNoMax, &n, &error) && n == 80);
    CHECK(!asuna::argparse::integerIn("--height", "79", 80, kNoMax, &n, &error));
    CHECK(asuna::argparse::integerIn("--fps", "0", 0, kNoMax, &n, &error) && n == 0);
    CHECK(!asuna::argparse::integerIn("--fps", "-1", 0, kNoMax, &n, &error));

    double d = 0;
    CHECK(asuna::argparse::realIn("--scale", "0.5", 0.5, 2.5, &d, &error) && d == 0.5);
    CHECK(asuna::argparse::realIn("--scale", "2.5", 0.5, 2.5, &d, &error) && d == 2.5);
    CHECK(!asuna::argparse::realIn("--scale", "0.49", 0.5, 2.5, &d, &error));
    CHECK(!asuna::argparse::realIn("--scale", "2.51", 0.5, 2.5, &d, &error));
    CHECK(asuna::argparse::realIn("--side", "0", 0.0, 1.0, &d, &error) && d == 0.0);
    CHECK(asuna::argparse::realIn("--side", "1", 0.0, 1.0, &d, &error) && d == 1.0);
}

void aRejectedValueLeavesTheDestinationAlone() {
    // parseOptions passes the current setting in as the fallback, so a refused
    // flag must not overwrite it on the way out.
    int n = 460;
    double d = 0.30;
    std::string error;
    CHECK(!asuna::argparse::integerIn("--height", "nope", 80, kNoMax, &n, &error));
    CHECK(n == 460);
    CHECK(!asuna::argparse::realIn("--side", "nope", 0.0, 1.0, &d, &error));
    CHECK(d == 0.30);
}

void theComplaintNamesTheFlagAndTheValue() {
    int n = 0;
    std::string error;
    asuna::argparse::integerIn("--height", "tall", 80, kNoMax, &n, &error);
    CHECK(error.find("--height") != std::string::npos);
    CHECK(error.find("tall") != std::string::npos);

    // No ceiling means the message should not invent one out of kNoMax.
    asuna::argparse::integerIn("--fps", "-1", 0, kNoMax, &n, &error);
    CHECK(error.find("at least 0") != std::string::npos);
    CHECK(error.find("2147483647") == std::string::npos);

    // Too big is not the same complaint as not-a-number, and saying "needs a
    // whole number, not '2147483648'" would send the reader hunting a typo.
    asuna::argparse::integerIn("--height", "2147483648", 80, kNoMax, &n, &error);
    CHECK(error.find("out of range") != std::string::npos);
    CHECK(error.find("whole number") == std::string::npos);

    double d = 0;
    asuna::argparse::realIn("--scale", "9", 0.5, 2.5, &d, &error);
    CHECK(error.find("between 0.5 and 2.5") != std::string::npos);

    asuna::argparse::realAtLeast("--x", "-5", 0.0, &d, &error);
    CHECK(error.find("at least 0") != std::string::npos);
    CHECK(error.find("e+09") == std::string::npos);

    // realAny is the no-range one, for the values the daemon clamps itself.
    CHECK(asuna::argparse::realAny("move", "-500", &d, &error) && d == -500.0);
    CHECK(asuna::argparse::realAny("scale", "9", &d, &error) && d == 9.0);
    CHECK(!asuna::argparse::realAny("move", "1.2.3", &d, &error));
}

void aFloorWithNoCeilingIsNotARangeEndingAtIntMax() {
    double d = 0;
    std::string error;
    // The bug this pins down: --x was written as realIn(0, kNoMax), and kNoMax
    // is a real int that realIn really does compare against. A position of
    // three billion was refused - by a message about zero, which sent the
    // reader looking in exactly the wrong place.
    CHECK(asuna::argparse::realAtLeast("--x", "3000000000", 0.0, &d, &error));
    CHECK(d == 3000000000.0);
    CHECK(asuna::argparse::realAtLeast("--x", "0", 0.0, &d, &error) && d == 0.0);
    CHECK(!asuna::argparse::realAtLeast("--x", "-1", 0.0, &d, &error));
    CHECK(!asuna::argparse::realAtLeast("--x", "nope", 0.0, &d, &error));
    // Still finite-only, like everything else here.
    CHECK(!asuna::argparse::realAtLeast("--x", "inf", 0.0, &d, &error));

    // `say --for` is the exclusive one: zero is not a duration, it is how the
    // daemon spells "no timer", which is what omitting --for already does.
    CHECK(!asuna::argparse::realAbove("say --for", "0", 0.0, &d, &error));
    CHECK(error.find("more than 0") != std::string::npos);
    CHECK(!asuna::argparse::realAbove("say --for", "-3", 0.0, &d, &error));
    // No millisecond floor: a short timer is a policy this end does not own.
    CHECK(asuna::argparse::realAbove("say --for", "0.0001", 0.0, &d, &error));
    CHECK(d == 0.0001);
    CHECK(asuna::argparse::realAbove("say --for", "3600", 0.0, &d, &error) && d == 3600.0);
}

// --- the command tokenizer -------------------------------------------------

void theOrdinaryCommandsStillSplitTheSameWay() {
    // Everything that worked before quoting existed has to keep working, or a
    // config file that has been fine for months stops starting the helper.
    SPLITS("python3 /path/thing.py", (std::vector<std::string>{"python3", "/path/thing.py"}));
    SPLITS("asuna-ext", (std::vector<std::string>{"asuna-ext"}));
    SPLITS("py a b c", (std::vector<std::string>{"py", "a", "b", "c"}));
    // Runs of separators collapse, as they did.
    SPLITS("py   a", (std::vector<std::string>{"py", "a"}));
    SPLITS("  py a  ", (std::vector<std::string>{"py", "a"}));
    // Empty is empty, and is the caller's business rather than an error here.
    SPLITS("", (std::vector<std::string>{}));
    SPLITS("   ", (std::vector<std::string>{}));
    // A tab used to end up *inside* an argument, because the old splitter knew
    // only about ' '.
    SPLITS("a\tb", (std::vector<std::string>{"a", "b"}));
    SPLITS("a\nb", (std::vector<std::string>{"a", "b"}));
}

void quotedArgumentsHoldTogether() {
    SPLITS("\"/home/my helper.py\" --flag",
           (std::vector<std::string>{"/home/my helper.py", "--flag"}));
    SPLITS("'/home/my helper.py' --flag",
           (std::vector<std::string>{"/home/my helper.py", "--flag"}));
    SPLITS("py \"x y\" 'z w'", (std::vector<std::string>{"py", "x y", "z w"}));
    // Quotes end where they end - the rest of the word carries on.
    SPLITS("a\"b c\"d", (std::vector<std::string>{"ab cd"}));
}

void escapesAreTheShlexSubset() {
    // A space escaped outside quotes.
    SPLITS("/home/my\\ helper.py", (std::vector<std::string>{"/home/my helper.py"}));
    // Single quotes are literal all through: no escape processing at all.
    SPLITS("'a\\b'", (std::vector<std::string>{"a\\b"}));
    // Double quotes process \" and \\ and nothing else.
    SPLITS("\"a\\\"b\"", (std::vector<std::string>{"a\"b"}));
    SPLITS("\"a\\\\b\"", (std::vector<std::string>{"a\\b"}));
    // \n is not an escape here; both characters stay, which is what keeps a
    // regex or a Windows-shaped path in an argument intact.
    SPLITS("\"a\\nb\"", (std::vector<std::string>{"a\\nb"}));
}

void emptyArgumentsSurvive() {
    // "" is an argument. The old splitter dropped empty words, so a deliberate
    // empty argument could not be passed at all.
    SPLITS("a \"\" b", (std::vector<std::string>{"a", "", "b"}));
    SPLITS("a '' b", (std::vector<std::string>{"a", "", "b"}));
    SPLITS("\"\"", (std::vector<std::string>{""}));
}

void unfinishedQuotingIsRefusedNotGuessed() {
    SPLIT_FAILS("'unterminated");
    SPLIT_FAILS("\"unterminated");
    SPLIT_FAILS("py 'a b");
    SPLIT_FAILS("trailing\\");
    // A backslash swallowed by an unclosed quote is still an unclosed quote.
    SPLIT_FAILS("\"abc\\");
}

void nothingIsExpandedOrSubstituted() {
    // The grammar decides where arguments end. It is emphatically not a shell:
    // if any of these ever start meaning something, a config file has become a
    // way to run arbitrary commands at session start.
    SPLITS("py $HOME", (std::vector<std::string>{"py", "$HOME"}));
    SPLITS("py ~/x", (std::vector<std::string>{"py", "~/x"}));
    SPLITS("py *.txt", (std::vector<std::string>{"py", "*.txt"}));
    SPLITS("py a;b", (std::vector<std::string>{"py", "a;b"}));
    SPLITS("py a|b", (std::vector<std::string>{"py", "a|b"}));
    SPLITS("py a>b", (std::vector<std::string>{"py", "a>b"}));
    SPLITS("py `id`", (std::vector<std::string>{"py", "`id`"}));
    SPLITS("py $(id)", (std::vector<std::string>{"py", "$(id)"}));
    SPLITS("py a&&b", (std::vector<std::string>{"py", "a&&b"}));
    SPLITS("py #x", (std::vector<std::string>{"py", "#x"}));
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"wholeNumbersAreRead", wholeNumbersAreRead},
        {"malformedWholeNumbersAreRefused", malformedWholeNumbersAreRefused},
        {"wholeNumberOverflowIsRefusedNotWrapped", wholeNumberOverflowIsRefusedNotWrapped},
        {"realNumbersAreRead", realNumbersAreRead},
        {"malformedRealNumbersAreRefused", malformedRealNumbersAreRefused},
        {"nonFiniteRealsAreRefused", nonFiniteRealsAreRefused},
        {"rangesAreInclusiveAtBothEnds", rangesAreInclusiveAtBothEnds},
        {"aRejectedValueLeavesTheDestinationAlone", aRejectedValueLeavesTheDestinationAlone},
        {"theComplaintNamesTheFlagAndTheValue", theComplaintNamesTheFlagAndTheValue},
        {"aFloorWithNoCeilingIsNotARangeEndingAtIntMax", aFloorWithNoCeilingIsNotARangeEndingAtIntMax},
        {"theOrdinaryCommandsStillSplitTheSameWay", theOrdinaryCommandsStillSplitTheSameWay},
        {"quotedArgumentsHoldTogether", quotedArgumentsHoldTogether},
        {"escapesAreTheShlexSubset", escapesAreTheShlexSubset},
        {"emptyArgumentsSurvive", emptyArgumentsSurvive},
        {"unfinishedQuotingIsRefusedNotGuessed", unfinishedQuotingIsRefusedNotGuessed},
        {"nothingIsExpandedOrSubstituted", nothingIsExpandedOrSubstituted},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-42s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
