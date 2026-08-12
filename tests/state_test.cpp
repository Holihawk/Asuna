// Round trips for src/app/state.cpp - the file that remembers where she was
// left, how big she was made, and which outfit was on.
//
// The whole suite runs against a temporary XDG_STATE_HOME, set before the first
// call to State::path(). paths::stateDir() reads that variable, so nothing here
// can see - let alone overwrite - a real ~/.local/state/asuna/state.json.
//
// What is actually being pinned down: this file used to be written by pasting
// strings between two quote characters and read back by scanning for `"key"`.
// A model path is a filename, and a Linux filename may hold a quote, a
// backslash or a newline. Written raw, any of those produced a file that would
// not parse, and took the position, the outfit and the layer down with it.
//
// The stderr assertions matter as much as the values. Everything load() refuses
// it refuses non-fatally, leaving the default in place - so the only thing
// standing between "handled" and "quietly threw away the outfit you picked" is
// the line it writes, and a test that ignores stderr cannot tell the two apart.

#include "app/state.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "json.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;
const char* current = "";
fs::path tmpDir;

void check(bool ok, const char* expr, int line) {
    if (ok) return;
    printf("  FAIL %s:%d  %s\n", current, line, expr);
    ++failures;
}

void checkEq(const std::string& got, const std::string& want, const char* what, int line) {
    if (got == want) return;
    printf("  FAIL %s:%d  %s\n    got  [%s]\n    want [%s]\n", current, line, what, got.c_str(),
           want.c_str());
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(got, want) checkEq((got), (want), #got, __LINE__)

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

void removeState() {
    std::error_code ec;
    fs::permissions(asuna::State::path(), fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add, ec);
    fs::remove(asuna::State::path(), ec);
}

// Runs `fn` with stderr pointed at a file and hands back what it wrote.
//
// Needed because half of what load() promises is a diagnostic: a state file
// that is there but unreadable, and a member written with the wrong type, both
// carry on with the defaults - so the only difference between "handled" and
// "silently discarded somebody's outfit" is the line on stderr. Asserting the
// return value alone cannot tell those two apart.
std::string captureStderr(const std::function<void()>& fn) {
    const std::string log = (tmpDir / "stderr.txt").string();
    fflush(stderr);
    const int saved = dup(STDERR_FILENO);
    const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (saved < 0 || fd < 0) {
        printf("  FAIL %s  could not redirect stderr\n", current);
        ++failures;
        if (fd >= 0) close(fd);
        if (saved >= 0) close(saved);
        fn();
        return "";
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
    fn();
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    const std::string out = slurp(log);
    std::error_code ec;
    fs::remove(log, ec);
    return out;
}

// `text` appears somewhere in `haystack`.
void checkSays(const std::string& haystack, const std::string& text, int line) {
    if (haystack.find(text) != std::string::npos) return;
    printf("  FAIL %s:%d  stderr said [%s]\n    wanted it to mention [%s]\n", current, line,
           haystack.c_str(), text.c_str());
    ++failures;
}

#define CHECK_SAYS(haystack, text) checkSays((haystack), (text), __LINE__)

// Saves `in`, reads it back into a fresh State, and hands both to the caller.
asuna::State roundTrip(const asuna::State& in) {
    removeState();
    if (!in.save()) {
        printf("  FAIL %s  save() failed\n", current);
        ++failures;
        return {};
    }
    asuna::State out;
    out.load();
    return out;
}

// --- the tests -------------------------------------------------------------

void anOrdinaryStateComesBackUnchanged() {
    asuna::State in;
    in.x = 412;
    in.scale = 1.25f;
    in.model = "/home/someone/.local/share/asuna/models/asuna_03/index.json";
    in.layer = "top";
    in.output = "eDP-2";
    in.hidden = 0;

    const asuna::State out = roundTrip(in);
    CHECK(out.x == 412);
    CHECK(out.scale == 1.25f);
    CHECK_EQ(out.model, in.model);
    CHECK_EQ(out.layer, in.layer);
    CHECK_EQ(out.output, in.output);
    CHECK(out.hidden == 0);

    // And what it wrote is real JSON, not something that only our own reader
    // would accept.
    std::string reason;
    const asuna::Json doc = asuna::Json::parse(slurp(asuna::State::path()), &reason);
    CHECK(doc.isObject());
    CHECK_EQ(reason, "");
}

void awkwardCharactersInAPathSurvive() {
    // Every one of these is a legal Linux filename, and every one of them broke
    // the old writer, the old reader, or both.
    const char* paths[] = {
        "/home/a/my \"quoted\" outfit/index.json",   // stopped the old scan early
        "/home/a/back\\slash/index.json",            // produced an invalid escape
        "/home/a/one\ttab/index.json",
        "/home/a/trailing space /index.json",
        "/home/a/衣装/index.json",                    // UTF-8, which must not be mangled
        "/home/a/emoji \xF0\x9F\x91\x97/index.json",  // outside the BMP
        "/home/a/{brace}/index.json",
        "/home/a/comma,and:colon/index.json",
        // The old reader searched for `"scale"` anywhere in the file, so a path
        // that contained the text of another key could answer for that key.
        "/home/a/\"scale\": 9/index.json",
    };
    for (const char* p : paths) {
        asuna::State in;
        in.x = 10;
        in.scale = 1.0f;
        in.model = p;
        in.layer = "bottom";
        in.output = "DP-1";
        in.hidden = 1;

        const asuna::State out = roundTrip(in);
        CHECK_EQ(out.model, p);
        // The decoys must not have moved anything else.
        CHECK(out.scale == 1.0f);
        CHECK(out.x == 10);
        CHECK_EQ(out.layer, "bottom");
        CHECK_EQ(out.output, "DP-1");
        CHECK(out.hidden == 1);
    }
}

void aNewlineInAPathSurvives() {
    // Its own case because it is the one that used to produce a file whose
    // *shape* was wrong rather than whose value was wrong.
    asuna::State in;
    in.model = "/home/a/two\nlines/index.json";
    in.layer = "overlay";
    in.x = 1;
    in.scale = 1.0f;

    const asuna::State out = roundTrip(in);
    CHECK_EQ(out.model, in.model);
    CHECK_EQ(out.layer, "overlay");
    // The escape really is an escape, not a raw newline in the file.
    CHECK(slurp(asuna::State::path()).find("\\n") != std::string::npos);
}

void theNeverPlacedSentinelIsNotRoundedIntoAPosition() {
    // x carries -1 for "she has never been put anywhere", and shell_render.cpp
    // restores the remembered position on `mState.x >= 0`. The old writer used
    // (long)(x + 0.5), which turns -1 into 0 - a position, hard against the
    // left edge, outranking the config's anchor from then on.
    asuna::State in;
    in.x = -1.0;
    in.scale = -1.0f;
    in.hidden = -1;

    const asuna::State out = roundTrip(in);
    CHECK(out.x < 0);
    CHECK(out.scale < 0);
}

void positionsRoundToTheNearestPixel() {
    asuna::State in;
    in.scale = 1.0f;
    in.x = 2.4;
    CHECK(roundTrip(in).x == 2);
    in.x = 2.6;
    CHECK(roundTrip(in).x == 3);
    in.x = 0.0;
    CHECK(roundTrip(in).x == 0);
}

void aMissingFileIsAFirstRunNotAFailure() {
    removeState();
    asuna::State s;
    // And it says nothing while doing it. A first start is the common case, and
    // a complaint about a file that is *supposed* not to exist yet is noise in
    // asuna.log that trains you to ignore the next one.
    CHECK_EQ(captureStderr([&] { CHECK(s.load()); }), "");
    // Every default still standing is what makes "delete the file to reset"
    // work.
    CHECK(s.x < 0);
    CHECK(s.scale < 0);
    CHECK(s.hidden < 0);
    CHECK(s.model.empty());
    CHECK(s.layer.empty());
    CHECK(s.output.empty());
}

void aMalformedFileLeavesEveryDefaultStanding() {
    const char* rubbish[] = {
        "",
        "   ",
        "not json at all",
        "{",
        "{\"x\": }",
        "[1, 2, 3]",           // valid JSON, wrong shape
        "\"just a string\"",   // ditto
        "42",
        "{\"x\": 5,}",         // trailing comma
    };
    for (const char* text : rubbish) {
        write(asuna::State::path(), text);
        asuna::State s;
        // Controlled: it carries on, and it does not fail the startup it is
        // part of. A pet that will not launch because of its own scratchpad is
        // worse than a pet that forgets where it was.
        //
        // Carrying on quietly would be a different matter - that is a position
        // and an outfit reverting on every login with nothing said - so the
        // line on stderr is part of the behaviour, not decoration.
        const std::string said = captureStderr([&] { CHECK(s.load()); });
        CHECK_SAYS(said, asuna::State::path());
        CHECK_SAYS(said, "ignoring");
        CHECK(s.x < 0);
        CHECK(s.scale < 0);
        CHECK(s.model.empty());
    }
}

void anUnreadableFileIsSaidRatherThanTakenForAFirstRun() {
    // The case a plain `if (!stream) return true;` cannot see: the file is
    // there, it has her position in it, and it cannot be opened. Treated as a
    // first run it looks exactly like a fresh install - she comes up at the
    // config's anchor, every login, and nothing anywhere says why.
    if (geteuid() == 0) {
        printf("  (skipped: running as root, where mode 000 still reads)\n");
        return;
    }
    write(asuna::State::path(), "{\"x\": 300, \"scale\": 1.5}");
    std::error_code ec;
    fs::permissions(asuna::State::path(), fs::perms::none, ec);
    if (ec) {
        printf("  (skipped: could not make the file unreadable)\n");
        return;
    }

    asuna::State s;
    const std::string said = captureStderr([&] { CHECK(s.load()); });
    CHECK_SAYS(said, asuna::State::path());
    // Still non-fatal, and still every default standing.
    CHECK(s.x < 0);
    CHECK(s.scale < 0);

    fs::permissions(asuna::State::path(), fs::perms::owner_read | fs::perms::owner_write, ec);
    removeState();
}

void aWrongTypedMemberIsIgnoredAndTheRestIsKept() {
    // Half a remembered state beats none: the members that are the right shape
    // still apply.
    write(asuna::State::path(),
          "{\"x\": \"nope\", \"scale\": \"big\", \"model\": 7, \"layer\": \"bottom\","
          " \"output\": null, \"hidden\": \"yes\"}");
    asuna::State s;
    const std::string said = captureStderr([&] { CHECK(s.load()); });
    CHECK(s.x < 0);            // refused, default kept
    CHECK(s.scale < 0);        // refused
    CHECK(s.model.empty());    // refused
    CHECK_EQ(s.layer, "bottom");   // the one good member still lands
    CHECK(s.output.empty());
    CHECK(s.hidden < 0);

    // Every refused member named, including the null one. This file is only
    // ever hand-edited, so a member that was written and does nothing is
    // somebody watching for an effect that is not coming.
    CHECK_SAYS(said, "x");
    CHECK_SAYS(said, "scale");
    CHECK_SAYS(said, "model");
    CHECK_SAYS(said, "output");
    CHECK_SAYS(said, "hidden");
    CHECK_SAYS(said, "wrong type");
    // The one that was right is not in the complaint.
    CHECK(said.find("layer") == std::string::npos);
}

void anExplicitNullIsNamedAndAMissingKeyIsNot() {
    // Both read as Null through Json::operator[], and they are not the same
    // event: nothing here writes null, so `"output": null` is a hand edit that
    // does nothing, while an absent `output` is just a setting never used.
    // Telling them apart is the whole reason Json::has exists.
    write(asuna::State::path(), "{\"output\": null, \"x\": 40}");
    asuna::State withNull;
    const std::string said = captureStderr([&] { withNull.load(); });
    CHECK_SAYS(said, "output");
    CHECK(withNull.output.empty());
    CHECK(withNull.x == 40);

    write(asuna::State::path(), "{\"x\": 40}");
    asuna::State without;
    CHECK_EQ(captureStderr([&] { without.load(); }), "");
    CHECK(without.output.empty());
    CHECK(without.x == 40);
}

void aStringThatLooksLikeANumberIsStillNotANumber() {
    // The old reader ran stod() over whatever text followed the colon, so
    // "5" and 5 were the same thing to it. They are not the same thing.
    write(asuna::State::path(), "{\"x\": \"5\", \"scale\": 1.5}");
    asuna::State s;
    CHECK_SAYS(captureStderr([&] { s.load(); }), "x");
    CHECK(s.x < 0);
    CHECK(s.scale == 1.5f);
}

void keysNestedInsideValuesAreNotMistakenForTheRealThing() {
    // The scan this replaces looked for `"hidden"` anywhere in the document.
    write(asuna::State::path(),
          "{\"model\": \"/a/\\\"hidden\\\": true/b.json\", \"hidden\": false, \"x\": 3}");
    asuna::State s;
    s.load();
    CHECK(s.hidden == 0);
    CHECK_EQ(s.model, "/a/\"hidden\": true/b.json");
    CHECK(s.x == 3);
}

void savingIsAtomicEnoughToLeaveNoLitter() {
    asuna::State in;
    in.x = 5;
    in.scale = 1.0f;
    in.model = "/a/b.json";
    CHECK(in.save());
    // The write goes to a temporary and is renamed, so a crash mid-write cannot
    // leave a half file. Nothing should be left behind afterwards either.
    CHECK(!fs::exists(asuna::State::path() + ".tmp"));
    CHECK(fs::exists(asuna::State::path()));
}

void escapingMatchesTheDecoderExactly() {
    // Json::quote is what State::save writes through and what ipc uses on the
    // wire; Json::parse is what reads it back. These are the pairs that have to
    // agree, checked here rather than assumed.
    const char* samples[] = {"plain", "a\"b", "a\\b", "one\ntwo", "tab\there",
                             "\x01\x02", "早安", "mixed \"a\\b\" 早"};
    for (const char* s : samples) {
        const std::string doc = "{\"v\": " + asuna::Json::quote(s) + "}";
        std::string reason;
        const asuna::Json parsed = asuna::Json::parse(doc, &reason);
        CHECK_EQ(reason, "");
        CHECK_EQ(parsed["v"].asString(), s);
    }
}

}  // namespace

int main() {
    // Before anything calls State::path(). Everything below writes here.
    const fs::path tmp = fs::temp_directory_path() /
                         ("asuna-state-test-" + std::to_string(getpid()));
    fs::create_directories(tmp);
    setenv("XDG_STATE_HOME", tmp.c_str(), 1);
    tmpDir = tmp;   // where captureStderr puts its scratch file

    // If this is not under our temporary directory, something resolved the path
    // before the variable was set and the suite would be writing over somebody's
    // real state file. Stop rather than find out.
    if (asuna::State::path().rfind(tmp.string(), 0) != 0) {
        printf("refusing to run: State::path() is %s, outside %s\n",
               asuna::State::path().c_str(), tmp.c_str());
        return 1;
    }

    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"anOrdinaryStateComesBackUnchanged", anOrdinaryStateComesBackUnchanged},
        {"awkwardCharactersInAPathSurvive", awkwardCharactersInAPathSurvive},
        {"aNewlineInAPathSurvives", aNewlineInAPathSurvives},
        {"theNeverPlacedSentinelIsNotRoundedIntoAPosition",
         theNeverPlacedSentinelIsNotRoundedIntoAPosition},
        {"positionsRoundToTheNearestPixel", positionsRoundToTheNearestPixel},
        {"aMissingFileIsAFirstRunNotAFailure", aMissingFileIsAFirstRunNotAFailure},
        {"anUnreadableFileIsSaidRatherThanTakenForAFirstRun",
         anUnreadableFileIsSaidRatherThanTakenForAFirstRun},
        {"aMalformedFileLeavesEveryDefaultStanding", aMalformedFileLeavesEveryDefaultStanding},
        {"aWrongTypedMemberIsIgnoredAndTheRestIsKept", aWrongTypedMemberIsIgnoredAndTheRestIsKept},
        {"anExplicitNullIsNamedAndAMissingKeyIsNot", anExplicitNullIsNamedAndAMissingKeyIsNot},
        {"aStringThatLooksLikeANumberIsStillNotANumber",
         aStringThatLooksLikeANumberIsStillNotANumber},
        {"keysNestedInsideValuesAreNotMistakenForTheRealThing",
         keysNestedInsideValuesAreNotMistakenForTheRealThing},
        {"savingIsAtomicEnoughToLeaveNoLitter", savingIsAtomicEnoughToLeaveNoLitter},
        {"escapingMatchesTheDecoderExactly", escapingMatchesTheDecoderExactly},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-52s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu state tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
