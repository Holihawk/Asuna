#pragma once

#include <functional>
#include <random>
#include <string>
#include <vector>

namespace asuna {

struct BehaviourTuning {
    double idleGapMin = 2.5;      // s of stillness before the next idle motion
    double idleGapMax = 8.0;
    double repeatChance = 0.15;   // an idle slot spent on a REPEAT_0x flourish
    double chatterMin = 90.0;     // s between unprompted lines
    double chatterMax = 300.0;
    double dragLineGap = 20.0;    // s before a drag is worth commenting on again
    double expressionHold = 4.0;  // s a reaction face lasts before F_NOMAL
    double sleepAfter = 1800.0;   // s without interaction before she dozes off
    double gazeTau = 0.10;        // gaze smoothing, seconds
    double lookGapMin = 3.0;      // s between idle look-around targets
    double lookGapMax = 9.0;
    double lookRange = 0.45;      // how far the idle look-around wanders, -1..1
};

// Everything about her that is neither rendering nor input plumbing: when she
// moves on her own, what she says, where she looks, and when she falls asleep.
//
// Deliberately free of GTK, GL and the Live2D runtime - it emits *intents* (an
// expression name, a motion name, a dialogue key) through hooks the shell fills
// in. That is what makes the whole personality testable offline, which matters
// here for the same reason it did for the drag: this machine has no way to
// synthesise a pointer, and a scheduler you can only check by watching it is a
// scheduler you are not really checking.
class Behaviour {
public:
    struct Hooks {
        std::function<void(const std::string&)> expression;   // "F_FUN_SMILE"
        std::function<void(const std::string&)> motion;       // by name, "I_FUN_S"
        std::function<void(const std::string&)> idleMotion;   // random from a group
        std::function<void(const std::string&)> say;          // a dialogue key
    };

    void setHooks(Hooks hooks) { mHooks = std::move(hooks); }

    // What the outfit she is wearing actually owns, from its index.json. Only
    // the face reaction uses it, and only to pick from: everything else still
    // asks for a name and lets Pet fall back if the costume has not got it.
    // Left empty (the default) the face reaction falls back to the fixed table
    // too, so a model with no expressions at all still reacts to being poked.
    void setInventory(std::vector<std::string> expressions, std::vector<std::string> motions) {
        mExpressions = std::move(expressions);
        mMotions = std::move(motions);
    }

    // Her own unprompted lines. Turned off while something else is speaking for
    // her - the extension helper, which is subscribed to her events and answers
    // them itself. Two voices on one character is worse than either alone, and
    // the dialogue file's lines are the ones that would arrive mid-sentence.
    // Everything else she does on her own - the idle motions, the look-around,
    // the sleep countdown - is untouched: it is the *talking* that collides.
    void setChatter(bool on);
    bool chatter() const { return mChatter; }

    void setTuning(const BehaviourTuning& t) { mTuning = t; }
    const BehaviourTuning& tuning() const { return mTuning; }
    void seed(unsigned s) { mRng.seed(s); }

    // One wall-clock step. `motionFinished` is the runtime's own answer, so the
    // scheduler waits for the current motion rather than cutting it off.
    void advance(double dt, bool motionFinished);

    // A tap that did not become a drag. `area` is a hit-area name from the
    // model's index.json ("head", "chest", "body", "foot"); "" means the tap
    // landed on her but in no declared area, which still counts as attention.
    void touch(const std::string& area);

    // Attention that is not a poke: a script or the menu driving her. It resets
    // the sleep countdown and wakes her if she was asleep, and that is all - the
    // caller is about to say what she should do, and a reaction of Behaviour's
    // own choosing would land on top of it. Call it *before* the thing it is
    // attending to, so the waking face does not overwrite that either.
    void notice();

    // The menu and `asuna expression` asking for a face directly. Unlike a
    // reaction it has no hold: an expression someone chose stays on until
    // something else changes it.
    void wearExpression(const std::string& name);

    void dragBegin();
    void dragEnd();

    // Pointer position over her, as -1..1 in her own frame. Only ever called
    // while the cursor is inside her input region: Wayland does not deliver
    // motion anywhere else, so "gaze follows cursor" really means "while you are
    // touching her" - see the README, "Known limits".
    void lookAt(double x, double y);
    void lookAway();

    // Smoothed, i.e. what the renderer should actually use.
    double gazeX() const { return mGazeX; }
    double gazeY() const { return mGazeY; }

    bool asleep() const { return mAsleep; }
    // True while there is something to animate beyond the idle loop - the shell
    // uses it the same way it uses Motion::animating().
    bool busy() const { return mDragging || mExpressionHold > 0; }

private:
    void wake();
    void say(const std::string& key);
    void setExpression(const std::string& name, double hold);
    double randomIn(double lo, double hi);
    const std::string& pickOf(const std::vector<std::string>& choices);
    // A face from this outfit's own list, never the resting one, never the
    // sleeping one, and never the one already on her. "" if there is no such
    // face, which is the caller's cue to use the fixed table instead.
    std::string pickFace();
    // A motion from the same family as `face` - F_FUN_SMILE -> one of I_FUN,
    // I_FUN_S, I_FUN_W - or "" if this outfit has none.
    std::string motionForFace(const std::string& face);

    BehaviourTuning mTuning;
    Hooks mHooks;
    std::mt19937 mRng{std::random_device{}()};

    std::vector<std::string> mExpressions;   // this outfit's, index.json order
    std::vector<std::string> mMotions;
    std::string mFace;                       // the expression she is wearing
    std::string mLastFace;                   // the last one a face tap chose

    double mSinceInteraction = 0;   // resets on any touch, drag or hover
    double mIdleGap = 0;            // countdown to the next idle motion
    double mChatterGap = -1;        // < 0 until the first advance() seeds it
    double mLookGap = 0;
    double mExpressionHold = 0;     // countdown to F_NOMAL
    double mSinceDragLine = 1e9;    // large, so the first drag always speaks

    double mGazeX = 0, mGazeY = 0;
    double mTargetX = 0, mTargetY = 0;
    bool mPointerGaze = false;      // the cursor is over her, so follow it
    bool mDragging = false;
    bool mAsleep = false;
    bool mChatter = true;           // she is the one doing the talking
};

}  // namespace asuna
