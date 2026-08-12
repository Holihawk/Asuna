// Offline tests for the personality: src/character/behaviour.cpp,
// src/character/dialogue.cpp and
// the JSON reader they both stand on. No GL, no compositor, no pointer.
//
// Half of Phase 4 is scheduling - when she moves on her own, when she speaks,
// when she falls asleep - on timescales of minutes. Watching that by hand is not
// testing it; nobody sits in front of a desktop pet for thirty minutes to find
// out whether the sleep timer fires. Here it takes a millisecond.

#include "character/behaviour.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "character/dialogue.hpp"
#include "json.hpp"

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
    printf("  FAIL %s:%d  %s: got %.4f, want %.4f +- %.4f\n", current, line, what,
           got, want, tol);
    ++failures;
}

void checkEq(const std::string& got, const std::string& want, const char* what, int line) {
    if (got == want) return;
    printf("  FAIL %s:%d  %s: got \"%s\", want \"%s\"\n", current, line, what,
           got.c_str(), want.c_str());
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_NEAR(got, want, tol) checkNear((got), (want), (tol), #got, __LINE__)
#define CHECK_EQ(got, want) checkEq((got), (want), #got, __LINE__)

// Records everything Behaviour asks for, so a test can assert on the sequence
// rather than on internal state.
struct Log {
    std::vector<std::string> expressions, motions, idleMotions, lines;

    asuna::Behaviour::Hooks hooks() {
        return {
            [this](const std::string& s) { expressions.push_back(s); },
            [this](const std::string& s) { motions.push_back(s); },
            [this](const std::string& s) { idleMotions.push_back(s); },
            [this](const std::string& s) { lines.push_back(s); },
        };
    }
    bool sawLine(const std::string& key) const {
        for (const auto& l : lines)
            if (l == key) return true;
        return false;
    }
    void clear() { expressions.clear(); motions.clear(); idleMotions.clear(); lines.clear(); }
};

// `seconds` of frames at 60 fps, with the current motion reported as finished
// unless a test says otherwise.
void run(asuna::Behaviour& b, double seconds, bool motionFinished = true) {
    const double dt = 1.0 / 60.0;
    for (int i = 0; i < static_cast<int>(seconds * 60); ++i) b.advance(dt, motionFinished);
}

asuna::Behaviour make(Log* log) {
    asuna::Behaviour b;
    b.setHooks(log->hooks());
    b.seed(1234);
    return b;
}

// --- JSON -----------------------------------------------------------------

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

// --- dialogue -------------------------------------------------------------

void dialogueNeverRepeatsItself() {
    asuna::Dialogue d;
    CHECK(d.load(asuna::Dialogue::defaultPath()));
    d.seed(7);

    std::string previous;
    int distinct = 0;
    for (int i = 0; i < 200; ++i) {
        const std::string line = d.pick("idle.chatter");
        CHECK(!line.empty());
        CHECK(line != previous);
        if (line != previous) ++distinct;
        previous = line;
    }
    CHECK(distinct == 200);
}

void dialogueIsQuietAboutTriggersItDoesNotHave() {
    asuna::Dialogue d;
    CHECK(d.load(asuna::Dialogue::defaultPath()));
    CHECK_EQ(d.pick("hit.elbow"), "");
}

void greetingFollowsTheClock() {
    asuna::Dialogue d;
    CHECK(d.load(asuna::Dialogue::defaultPath()));

    // A fixed local wall-clock hour, whatever the machine's timezone.
    auto at = [](int year, int month, int day, int hour) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_isdst = -1;
        return std::mktime(&tm);
    };

    CHECK_EQ(d.greetingKey(at(2026, 3, 17, 9)), "time.8-11");
    CHECK_EQ(d.greetingKey(at(2026, 3, 17, 2)), "time.0-5");
    CHECK_EQ(d.greetingKey(at(2026, 3, 17, 23)), "time.22-23");
    // A season beats the hour, and a range matches inside it.
    CHECK_EQ(d.greetingKey(at(2026, 1, 1, 9)), "season.0101");
    CHECK_EQ(d.greetingKey(at(2026, 12, 25, 9)), "season.1224-1226");
    CHECK_EQ(d.greetingKey(at(2026, 12, 27, 9)), "time.8-11");

    // {year} is substituted, not printed.
    const std::string line = d.greeting(at(2026, 1, 1, 9));
    CHECK(line.find("{year}") == std::string::npos);
    CHECK(line.find("2026") != std::string::npos);
}

// --- behaviour ------------------------------------------------------------

void idleMotionsKeepComing() {
    Log log;
    auto b = make(&log);
    run(b, 60.0);
    // idleGapMax is 8 s, so a minute must contain several - and they must not
    // all fire at once, which is what a broken countdown looks like.
    CHECK(log.idleMotions.size() + log.motions.size() >= 7);
    CHECK(log.idleMotions.size() + log.motions.size() <= 25);
    for (const auto& g : log.idleMotions) CHECK_EQ(g, "idle");
}

void idleMotionsWaitForTheCurrentOne() {
    Log log;
    auto b = make(&log);
    run(b, 60.0, /*motionFinished=*/false);
    CHECK(log.idleMotions.empty());
    CHECK(log.motions.empty());
}

void chatterIsOccasionalNotConstant() {
    Log log;
    auto b = make(&log);
    run(b, 600.0);   // ten minutes
    int chatter = 0;
    for (const auto& l : log.lines)
        if (l == "idle.chatter") ++chatter;
    // chatterMin 90 s, chatterMax 300 s: between two and seven in ten minutes.
    CHECK(chatter >= 2);
    CHECK(chatter <= 7);
}

// With an extension helper subscribed, it answers her triggers itself and she
// stops volunteering lines of her own - otherwise two voices arrive on top of
// each other, and the dialogue file's is the one that lands mid-sentence.
// Everything else she does alone is untouched: it is the talking that collides,
// not the moving.
void chatterStopsWhenSomethingElseIsSpeakingForHer() {
    Log log;
    auto b = make(&log);
    b.setChatter(false);
    CHECK(!b.chatter());
    run(b, 600.0);
    for (const auto& l : log.lines) CHECK(l != "idle.chatter");
    CHECK(log.idleMotions.size() + log.motions.size() >= 7);   // still alive

    // And she gets her voice back, with a full gap in front of it rather than
    // the remainder of one she was already overdue for.
    b.setChatter(true);
    CHECK(b.chatter());
    log.lines.clear();
    run(b, 60.0);
    int soon = 0;
    for (const auto& l : log.lines)
        if (l == "idle.chatter") ++soon;
    CHECK(soon <= 1);   // chatterMin is 90 s, so at most one in the first minute
    run(b, 540.0);
    int total = 0;
    for (const auto& l : log.lines)
        if (l == "idle.chatter") ++total;
    CHECK(total >= 2);
}

// A poke still speaks, helper or not. Suppression is about the lines she
// volunteers, and a reaction to being touched is not one of them - it is the
// answer to something the user just did, and silence there would read as her
// having stopped working.
void aPokeStillSpeaksWithTheHelperListening() {
    Log log;
    auto b = make(&log);
    b.setChatter(false);
    run(b, 5.0);
    b.touch("head");
    CHECK(!log.lines.empty());
}

void sheFallsAsleepAndWakesOnATouch() {
    Log log;
    auto b = make(&log);
    run(b, 1700.0);
    CHECK(!b.asleep());
    run(b, 200.0);
    CHECK(b.asleep());
    CHECK(log.sawLine("sleep"));
    CHECK_EQ(log.expressions.back(), "F_SLEEP");

    // Asleep means asleep: no idle motions, no chatter.
    log.clear();
    run(b, 600.0);
    CHECK(log.idleMotions.empty());
    CHECK(log.motions.empty());
    CHECK(!log.sawLine("idle.chatter"));

    log.clear();
    b.touch("head");
    CHECK(!b.asleep());
    CHECK(log.sawLine("wake"));
    // Waking is the whole reaction - she does not also giggle at being poked.
    CHECK(!log.sawLine("hit.head"));
}

void everyHitAreaHasItsOwnReaction() {
    struct Case { const char* area; const char* motion; const char* line; };
    // No inventory is set, so "head" is the fixed fallback pairing here; the
    // face reaction proper is aTapOnHerFaceIsNeverTheSameTwice.
    const Case cases[] = {
        {"head", "I_FUN_S", "hit.head"},
        {"chest", "I_ANGRY", "hit.chest"},
        {"body", "I_FUN", "hit.body"},
        {"foot", "I_SURPRISE_S", "hit.foot"},
    };
    for (const Case& c : cases) {
        Log log;
        auto b = make(&log);
        run(b, 1.0);
        log.clear();
        b.touch(c.area);
        CHECK(log.expressions.size() == 1);
        CHECK(!log.expressions.empty() && log.expressions[0].compare(0, 2, "F_") == 0);
        CHECK(log.motions.size() == 1);
        CHECK_EQ(log.motions.empty() ? "" : log.motions[0], c.motion);
        CHECK(log.lines.size() == 1);
        CHECK_EQ(log.lines.empty() ? "" : log.lines[0], c.line);
    }
}

// asuna_06's inventory, which every outfit in models/ follows the shape of.
const std::vector<std::string> kExpressions = {
    "F_FUN", "F_FUN_HANIKAMI", "F_FUN_MAX", "F_FUN_SMILE", "F_FUN_WARM",
    "F_NOMAL", "F_SAD", "F_SURPRISE", "F_ANGRY", "F_SLEEP"};
const std::vector<std::string> kMotions = {
    "I_FUN", "I_FUN_S", "I_FUN_W", "I_SAD", "I_SAD_S", "I_SAD_W", "I_SNEESE",
    "I_SURPRISE", "I_SURPRISE_S", "I_SURPRISE_W", "REPEAT_01", "REPEAT_02",
    "REPEAT_03", "I_ANGRY", "I_ANGRY_S", "I_ANGRY_W", "IDLING", "IDLING_02",
    "IDLING_03"};

// A tap on her face is the one reaction that is not a fixed pairing: it takes a
// face from the outfit's own list and the motion that belongs to it.
void aTapOnHerFaceIsNeverTheSameTwice() {
    Log log;
    auto b = make(&log);
    b.setInventory(kExpressions, kMotions);
    run(b, 1.0);
    log.clear();

    std::vector<std::string> seen;
    for (int i = 0; i < 40; ++i) {
        log.clear();
        b.touch("head");
        CHECK(log.expressions.size() == 1);
        if (log.expressions.empty()) break;
        const std::string face = log.expressions[0];
        seen.push_back(face);

        // Never the resting face, never the sleeping one - both would read as
        // no reaction at all.
        CHECK(face != "F_NOMAL");
        CHECK(face != "F_SLEEP");
        // And never the one she is already wearing, or the tap does nothing.
        CHECK(seen.size() < 2 || seen[seen.size() - 2] != face);

        // The motion that comes with it is from the same family.
        CHECK(log.motions.size() == 1);
        if (!log.motions.empty()) {
            const std::string family = face.substr(0, face.find('_', 2));
            CHECK_EQ(log.motions[0].substr(0, log.motions[0].find('_', 2)),
                     "I" + family.substr(1));
        }
        CHECK(log.sawLine("hit.head"));
        // The hold has to expire, or the next tap sees the same face still on.
        run(b, b.tuning().expressionHold + 0.5);
    }
    // Every face bar the two states should turn up over forty taps.
    for (const std::string& name : kExpressions) {
        if (name == "F_NOMAL" || name == "F_SLEEP") continue;
        CHECK(std::find(seen.begin(), seen.end(), name) != seen.end());
    }
}

// An outfit with no expressions at all still has to react to being poked.
void aFaceTapFallsBackWhenThereIsNothingToPick() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);
    log.clear();
    b.touch("head");
    CHECK(log.expressions.size() == 1);
    CHECK(log.motions.size() == 1);
    CHECK_EQ(log.motions.empty() ? "" : log.motions[0], "I_FUN_S");
    CHECK(log.sawLine("hit.head"));

    // Faces but no motions: the face is the whole reaction rather than a motion
    // borrowed from a family she has not got.
    Log log2;
    auto c = make(&log2);
    c.setInventory(kExpressions, {});
    run(c, 1.0);
    log2.clear();
    c.touch("head");
    CHECK(log2.expressions.size() == 1);
    CHECK(log2.motions.empty());
    CHECK(log2.sawLine("hit.head"));
}

void aReactionFaceGoesBackToNeutral() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);
    log.clear();
    b.touch("head");
    run(b, 1.0);
    CHECK(log.expressions.size() == 1);   // still holding the reaction
    run(b, 4.0);
    CHECK(log.expressions.size() == 2);
    CHECK_EQ(log.expressions.back(), "F_NOMAL");
}

void dragSpeaksButNotEveryTime() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);
    log.clear();

    b.dragBegin();
    CHECK_EQ(log.expressions.empty() ? "" : log.expressions[0], "F_SURPRISE");
    CHECK(log.sawLine("drag.start"));
    b.dragEnd();
    CHECK_EQ(log.expressions.back(), "F_NOMAL");

    // Picked up again a second later: she moves, but she does not narrate it.
    log.clear();
    run(b, 1.0);
    b.dragBegin();
    b.dragEnd();
    CHECK(log.lines.empty());

    // After the cooldown she has something to say again.
    log.clear();
    run(b, 25.0);
    b.dragBegin();
    CHECK(log.sawLine("drag.start"));
}

void beingCarriedSuspendsTheIdleLoop() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);
    b.dragBegin();
    log.clear();
    run(b, 120.0);
    CHECK(log.idleMotions.empty());
    CHECK(!log.sawLine("idle.chatter"));
    CHECK(!b.asleep());   // and she cannot doze off in your hand
}

void gazeFollowsThenDriftsBack() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);

    b.lookAt(0.8, -0.4);
    // gazeTau is 0.10 s, so a fifth of a second gets most of the way there.
    run(b, 0.2);
    CHECK(b.gazeX() > 0.5);
    CHECK(b.gazeY() < -0.2);

    // It is a *smoothed* follow, not a copy: one frame must not arrive.
    Log log2;
    auto c = make(&log2);
    run(c, 1.0);
    c.lookAt(1.0, 0.0);
    c.advance(1.0 / 60.0, true);
    CHECK(c.gazeX() < 0.5);

    // Pointer gone: she holds the direction for a beat, then drifts off and
    // stays within the idle look-around range from then on.
    b.lookAt(0.95, 0.0);
    run(b, 1.0);
    b.lookAway();
    run(b, 0.5);
    CHECK(b.gazeX() > 0.8);   // still looking where it was

    run(b, 2.5);
    double extreme = 0;
    for (int i = 0; i < 60 * 120; ++i) {
        b.advance(1.0 / 60.0, true);
        extreme = std::max(extreme, std::fabs(b.gazeX()));
    }
    CHECK(extreme <= b.tuning().lookRange + 0.01);
}

void aStallDoesNotFireEverythingAtOnce() {
    Log log;
    auto b = make(&log);
    run(b, 1.0);
    log.clear();
    // Resume from suspend: one frame carrying an hour.
    b.advance(3600.0, true);
    CHECK(log.idleMotions.size() + log.motions.size() <= 1);
    CHECK(!b.asleep());
}

struct Test { const char* name; void (*fn)(); };

}  // namespace

int main() {
    const Test tests[] = {
        {"jsonReadsWhatWeActuallyParse", jsonReadsWhatWeActuallyParse},
        {"jsonRejectsRubbish", jsonRejectsRubbish},
        {"jsonDecodesUtf8AndEscapes", jsonDecodesUtf8AndEscapes},
        {"dialogueNeverRepeatsItself", dialogueNeverRepeatsItself},
        {"dialogueIsQuietAboutMissingKeys", dialogueIsQuietAboutTriggersItDoesNotHave},
        {"greetingFollowsTheClock", greetingFollowsTheClock},
        {"idleMotionsKeepComing", idleMotionsKeepComing},
        {"idleMotionsWaitForTheCurrentOne", idleMotionsWaitForTheCurrentOne},
        {"chatterIsOccasionalNotConstant", chatterIsOccasionalNotConstant},
        {"chatterStopsForAHelper", chatterStopsWhenSomethingElseIsSpeakingForHer},
        {"aPokeStillSpeaksForAHelper", aPokeStillSpeaksWithTheHelperListening},
        {"sheFallsAsleepAndWakesOnATouch", sheFallsAsleepAndWakesOnATouch},
        {"everyHitAreaHasItsOwnReaction", everyHitAreaHasItsOwnReaction},
        {"aTapOnHerFaceIsNeverTheSameTwice", aTapOnHerFaceIsNeverTheSameTwice},
        {"aFaceTapFallsBackWhenNothingToPick", aFaceTapFallsBackWhenThereIsNothingToPick},
        {"aReactionFaceGoesBackToNeutral", aReactionFaceGoesBackToNeutral},
        {"dragSpeaksButNotEveryTime", dragSpeaksButNotEveryTime},
        {"beingCarriedSuspendsTheIdleLoop", beingCarriedSuspendsTheIdleLoop},
        {"gazeFollowsThenDriftsBack", gazeFollowsThenDriftsBack},
        {"aStallDoesNotFireEverythingAtOnce", aStallDoesNotFireEverythingAtOnce},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-36s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
