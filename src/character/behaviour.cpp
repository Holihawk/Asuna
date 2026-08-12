#include "character/behaviour.hpp"

#include <algorithm>
#include <cmath>

namespace asuna {
namespace {

// A frame delta larger than this is a stall - a resume from suspend, a long GPU
// hitch - and running the schedulers through it would fire an idle motion, a
// look-around and a chatter line all in the same frame.
constexpr double kMaxDt = 1.0;

// How long she keeps looking at where the cursor was after it leaves her.
constexpr double kLingerAfterPointer = 1.2;

struct Reaction {
    const char* area;
    std::vector<std::string> expressions;   // one is chosen at random
    const char* motion;
};

// Straight out of the README ("What she does"). The expressions and motions are the model's own
// names; an outfit that lacks one falls back inside Pet rather than here, so a
// future model with a different inventory degrades instead of going silent.
const std::vector<Reaction>& reactions() {
    static const std::vector<Reaction> table = {
        {"head",  {"F_FUN_HANIKAMI", "F_FUN_SMILE"}, "I_FUN_S"},
        {"chest", {"F_ANGRY", "F_SURPRISE"},         "I_ANGRY"},
        {"body",  {"F_FUN"},                         "I_FUN"},
        {"foot",  {"F_SURPRISE"},                    "I_SURPRISE_S"},
        // A tap that hit her but no declared area: acknowledge it warmly rather
        // than ignore it, or half her silhouette feels dead.
        {"",      {"F_FUN_WARM"},                    "I_FUN_W"},
    };
    return table;
}

const std::vector<std::string>& repeatMotions() {
    static const std::vector<std::string> names = {"REPEAT_01", "REPEAT_02", "REPEAT_03"};
    return names;
}

}  // namespace

double Behaviour::randomIn(double lo, double hi) {
    if (hi <= lo) return lo;
    return std::uniform_real_distribution<double>(lo, hi)(mRng);
}

const std::string& Behaviour::pickOf(const std::vector<std::string>& choices) {
    static const std::string empty;
    if (choices.empty()) return empty;
    return choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(mRng)];
}

void Behaviour::say(const std::string& key) {
    if (mHooks.say) mHooks.say(key);
}

void Behaviour::setExpression(const std::string& name, double hold) {
    if (mHooks.expression) mHooks.expression(name);
    mFace = name;
    mExpressionHold = hold;
}

// The two faces that are states rather than reactions: F_NOMAL is where every
// reaction returns to, and F_SLEEP is what she wears while asleep. Offering
// either as a reaction to being poked would read as no reaction at all, or as
// falling asleep on the spot.
// mLastFace as well as mFace: the reaction only holds for a few seconds and then
// goes back to F_NOMAL, so by the second tap the first face is no longer on her
// and excluding only what she is wearing would let the same one come up twice
// running. Dialogue::pick makes the same promise for the same reason.
std::string Behaviour::pickFace() {
    std::vector<std::string> choices;
    for (const std::string& name : mExpressions) {
        if (name == "F_NOMAL" || name == "F_SLEEP") continue;
        if (name == mFace || name == mLastFace) continue;
        choices.push_back(name);
    }
    if (choices.empty()) return "";
    mLastFace = pickOf(choices);
    return mLastFace;
}

std::string Behaviour::motionForFace(const std::string& face) {
    // The asset set names a face and the motion that goes with it after the same
    // family: F_FUN_SMILE and I_FUN_W are both FUN. Taking the family from the
    // name rather than from a table means an outfit that ships a family this
    // build has never heard of still pairs it correctly.
    if (face.compare(0, 2, "F_") != 0) return "";
    const size_t end = face.find('_', 2);
    const std::string family =
        face.substr(2, end == std::string::npos ? std::string::npos : end - 2);
    if (family.empty()) return "";

    const std::string prefix = "I_" + family;
    std::vector<std::string> choices;
    for (const std::string& name : mMotions) {
        // Exactly the family, or the family with a suffix: I_FUN matches
        // I_FUN_S, and must not match a future I_FUNERAL.
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.size() != prefix.size() && name[prefix.size()] != '_') continue;
        choices.push_back(name);
    }
    return choices.empty() ? std::string() : pickOf(choices);
}

void Behaviour::wake() {
    mSinceInteraction = 0;
    if (!mAsleep) return;
    mAsleep = false;
    setExpression("F_NOMAL", 0);
    say("wake");
    // Straight into a motion: waking up and then standing perfectly still for
    // the idle gap reads as a freeze, not as waking up.
    if (mHooks.idleMotion) mHooks.idleMotion("idle");
    mIdleGap = randomIn(mTuning.idleGapMin, mTuning.idleGapMax);
}

void Behaviour::advance(double dt, bool motionFinished) {
    dt = std::clamp(dt, 0.0, kMaxDt);

    // Seeded on the first step rather than at construction, so a caller that
    // sets the tuning or the seed after building still gets what it asked for.
    if (mChatterGap < 0) {
        mChatterGap = randomIn(mTuning.chatterMin, mTuning.chatterMax);
        mIdleGap = randomIn(mTuning.idleGapMin, mTuning.idleGapMax);
        mLookGap = randomIn(mTuning.lookGapMin, mTuning.lookGapMax);
    }

    mSinceInteraction += dt;
    mSinceDragLine += dt;

    if (mExpressionHold > 0) {
        mExpressionHold -= dt;
        // Not while being carried: the drag owns her face until it lets go.
        if (mExpressionHold <= 0 && !mDragging)
            setExpression(mAsleep ? "F_SLEEP" : "F_NOMAL", 0);
    }

    if (!mAsleep && !mDragging && mSinceInteraction >= mTuning.sleepAfter) {
        mAsleep = true;
        setExpression("F_SLEEP", 0);
        say("sleep");
    }

    // --- gaze ------------------------------------------------------------
    if (mAsleep) {
        mTargetX = 0;
        mTargetY = -0.55;   // head down
    } else if (!mPointerGaze && !mDragging) {
        mLookGap -= dt;
        if (mLookGap <= 0) {
            mLookGap = randomIn(mTuning.lookGapMin, mTuning.lookGapMax);
            const double r = mTuning.lookRange;
            mTargetX = randomIn(-r, r);
            mTargetY = randomIn(-r * 0.5, r * 0.5);
        }
    }
    // Exponential approach: frame-rate independent, and it is a *return* rather
    // than a snap, which is the whole point of not writing the target straight
    // into the model.
    const double k = mTuning.gazeTau > 0 ? 1.0 - std::exp(-dt / mTuning.gazeTau) : 1.0;
    mGazeX += (mTargetX - mGazeX) * k;
    mGazeY += (mTargetY - mGazeY) * k;

    if (mAsleep || mDragging) return;

    // --- idle motions ----------------------------------------------------
    // The gap is only counted down once the previous motion has actually
    // finished, so a long motion is never cut short and the pause between two
    // of them is the same whatever their lengths.
    if (motionFinished) {
        mIdleGap -= dt;
        if (mIdleGap <= 0) {
            mIdleGap = randomIn(mTuning.idleGapMin, mTuning.idleGapMax);
            const bool flourish =
                std::uniform_real_distribution<double>(0.0, 1.0)(mRng) < mTuning.repeatChance;
            if (flourish && mHooks.motion)
                mHooks.motion(pickOf(repeatMotions()));
            else if (mHooks.idleMotion)
                mHooks.idleMotion("idle");
        }
    }

    // --- unprompted chatter ----------------------------------------------
    if (!mChatter) return;
    mChatterGap -= dt;
    if (mChatterGap <= 0) {
        mChatterGap = randomIn(mTuning.chatterMin, mTuning.chatterMax);
        say("idle.chatter");
    }
}

void Behaviour::setChatter(bool on) {
    if (on == mChatter) return;
    mChatter = on;
    // A full gap on the way back, not the remainder of the one that was
    // running: the helper going away should not be followed by a line she was
    // already overdue for, arriving as if it were an answer to something.
    if (on) mChatterGap = randomIn(mTuning.chatterMin, mTuning.chatterMax);
}

void Behaviour::touch(const std::string& area) {
    const bool wasAsleep = mAsleep;
    wake();
    // Waking is the reaction. Poking someone awake and getting a shy giggle in
    // the same instant reads as two characters, not one.
    if (wasAsleep) return;

    // Her face answers with a face. Everywhere else on her plays one fixed
    // pairing, which is right for a body part - poking her waist should mean the
    // same thing twice - but wrong for the one part of her a person actually
    // looks at. So a tap on the head takes a face at random from the ones this
    // outfit ships and then the motion from the same family, and the pair is
    // never the same twice running.
    if (area == "head") {
        const std::string face = pickFace();
        if (!face.empty()) {
            setExpression(face, mTuning.expressionHold);
            const std::string motion = motionForFace(face);
            // No motion at all when the family has none: a face that changed
            // with nothing else moving still reads as a reaction, and a motion
            // from some other family would contradict the face.
            if (!motion.empty() && mHooks.motion) mHooks.motion(motion);
            say("hit.head");
            mChatterGap = randomIn(mTuning.chatterMin, mTuning.chatterMax);
            return;
        }
        // No expressions to choose from: fall through to the fixed table.
    }

    const auto& table = reactions();
    auto it = std::find_if(table.begin(), table.end(),
                           [&](const Reaction& r) { return area == r.area; });
    if (it == table.end()) it = std::prev(table.end());   // the "" fallback

    setExpression(pickOf(it->expressions), mTuning.expressionHold);
    if (mHooks.motion) mHooks.motion(it->motion);
    say(area.empty() ? "hit.body" : "hit." + area);
    // Whatever she was about to say on her own can wait.
    mChatterGap = randomIn(mTuning.chatterMin, mTuning.chatterMax);
}

void Behaviour::notice() { wake(); }

void Behaviour::wearExpression(const std::string& name) {
    notice();
    setExpression(name, 0);
}

void Behaviour::dragBegin() {
    wake();
    mDragging = true;
    setExpression("F_SURPRISE", 0);
    // Being picked up over and over should not turn into a wall of speech.
    if (mSinceDragLine >= mTuning.dragLineGap) {
        say("drag.start");
        mSinceDragLine = 0;
    }
}

void Behaviour::dragEnd() {
    if (!mDragging) return;
    mDragging = false;
    mSinceInteraction = 0;
    setExpression("F_NOMAL", 0);
    if (mSinceDragLine >= mTuning.dragLineGap) {
        say("drag.end");
        mSinceDragLine = 0;
    }
}

void Behaviour::lookAt(double x, double y) {
    wake();
    mPointerGaze = true;
    mTargetX = std::clamp(x, -1.0, 1.0);
    mTargetY = std::clamp(y, -1.0, 1.0);
}

void Behaviour::lookAway() {
    if (!mPointerGaze) return;
    mPointerGaze = false;
    // Not retargeted here: holding the last direction for a beat before drifting
    // off is what a person does. But only a beat - whatever was left of the
    // look-around gap could have been nine seconds of staring at a cursor that
    // is no longer there.
    mLookGap = std::min(mLookGap, kLingerAfterPointer);
}

}  // namespace asuna
