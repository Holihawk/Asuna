#include "pet/motion.hpp"

#include <algorithm>
#include <cmath>

namespace asuna {
namespace {

// One rate for the whole simulation. Above any display refresh rate we are
// likely to meet, so every rendered frame interpolates between two distinct sim
// states and the feel does not change with the monitor.
constexpr float kSimHz = 240.0f;
constexpr float kSimDt = 1.0f / kSimHz;

// Longest wall-clock gap worth simulating in one go. A suspended laptop hands
// back an enormous delta; catching up over hundreds of ticks would catapult her
// across the screen. Dropping the surplus is the right trade - she is allowed to
// miss the gap.
constexpr float kMaxFrameDt = 0.05f;

}  // namespace

void Motion::Spring::step(double target, double omega, double zeta, double dt) {
    const double accel = (target - value) * omega * omega - velocity * (2.0 * zeta * omega);
    velocity += accel * dt;
    value += velocity * dt;
}

Motion::Motion(MotionTuning tuning) : mTuning(tuning) {}

void Motion::reset(double x) {
    mX.snap(x);
    mLift.snap(0.0);
    mSquash.snap(0.0);
    mPhase = Phase::Landing;
    mTargetX = x;
    mPullUp = 0.0f;
    mAccumulator = 0.0f;
    mPrevious = current();
}

void Motion::setBounds(double minX, double maxX, float boxHeight) {
    mMinX = minX;
    mMaxX = std::max(minX, maxX);
    mBoxHeight = std::max(1.0f, boxHeight);
    mX.value = std::clamp(mX.value, mMinX, mMaxX);
}

void Motion::beginDrag() {
    // Velocity is deliberately kept: grabbing her mid-fall or mid-slide picks up
    // from where she was rather than kinking.
    mPhase = Phase::Held;
    mTargetX = mX.value;
    mPullUp = 0.0f;
}

void Motion::hold(double targetX, float pullUp) {
    mTargetX = targetX;
    mPullUp = pullUp;
}

void Motion::release() {
    if (mPhase != Phase::Held) return;
    // Falling starts from whatever velocity the drag left behind, so flicking
    // her upward keeps her rising briefly before gravity wins.
    mPhase = Phase::Falling;
}

void Motion::glideTo(double x) {
    // Not while she is being carried: the pointer owns her target then, and
    // fighting it would look like a tug of war.
    if (mPhase == Phase::Held) return;
    mTargetX = std::clamp(x, mMinX, mMaxX);
    mPhase = Phase::Gliding;
}

double Motion::liftTarget(double pull) const {
    const double base = mTuning.grabLift * mBoxHeight;
    const double ceiling = mTuning.maxLift * mBoxHeight;
    if (pull <= 0.0) return base;   // pushing down is a floor, not a rubber band
    const double slack = ceiling - base;
    if (slack <= 0.0) return ceiling;
    // Joined to the 1:1 region with unit derivative, so there is no kink where
    // the rubber band takes over.
    return base + slack * (1.0 - std::exp(-pull / slack));
}

void Motion::tick(double dt) {
    mPrevious = current();

    const double omegaDrag = 2.0 / mTuning.dragTau;

    const bool chasing = mPhase == Phase::Held || mPhase == Phase::Gliding;
    const double targetX = std::clamp(chasing ? mTargetX : mX.value, mMinX, mMaxX);
    mX.step(targetX, omegaDrag, 1.0, dt);
    // Clamped on the value as well as the target: a fast fling must not carry
    // her off the edge during the spring's approach.
    if (mX.value < mMinX || mX.value > mMaxX) {
        mX.value = std::clamp(mX.value, mMinX, mMaxX);
        mX.velocity = 0.0;
    }

    switch (mPhase) {
        case Phase::Held:
            mLift.step(liftTarget(mPullUp), omegaDrag, 1.0, dt);
            break;
        case Phase::Falling:
            mLift.velocity -= mTuning.fallGravity * dt;
            mLift.value += mLift.velocity * dt;
            if (mLift.value <= 0.0) land();
            break;
        case Phase::Landing:
        case Phase::Gliding:
            mLift.step(0.0, mTuning.landingOmega, mTuning.landingZeta, dt);
            break;
    }

    // Arrived: hand back to Landing so animating() can go quiet and the frame
    // loop can drop to its idle cap.
    if (mPhase == Phase::Gliding &&
        std::fabs(mX.value - targetX) < mTuning.settleEpsPx &&
        std::fabs(mX.velocity) < mTuning.settleEpsPxPerS) {
        mPhase = Phase::Landing;
    }

    // Stretch while she is held up; nothing to stretch once she is home.
    double stretch = 0.0;
    if (mPhase == Phase::Held) {
        const double reach =
            std::clamp(mLift.value / (mTuning.maxLift * mBoxHeight), 0.0, 1.0);
        stretch = reach * mTuning.liftStretch;
    }
    mSquash.step(stretch, mTuning.squashOmega, mTuning.squashZeta, dt);
}

void Motion::land() {
    const double impact = std::max(0.0, -mLift.velocity);
    // Not snapped to zero: the spring carries on from wherever the last tick put
    // her, keeping position and velocity continuous through the phase change.
    mPhase = Phase::Landing;

    const double kick = std::min<double>(impact * mTuning.squashPerImpact, mTuning.maxSquash);
    // A velocity kick rather than a jump in value: the compression builds over a
    // few frames instead of popping.
    mSquash.velocity -= kick * mTuning.squashOmega;
}

void Motion::advance(float frameDt) {
    mAccumulator += std::clamp(frameDt, 0.0f, kMaxFrameDt);
    while (mAccumulator >= kSimDt) {
        tick(kSimDt);
        mAccumulator -= kSimDt;
    }
}

Placement Motion::placement() const {
    // Interpolate between the last two sim states, so the display is smooth at
    // any refresh rate including ones that do not divide the sim rate.
    const float alpha = std::clamp(mAccumulator / kSimDt, 0.0f, 1.0f);
    const Sim now = current();
    const double x = mPrevious.x + (now.x - mPrevious.x) * alpha;
    const double lift = mPrevious.lift + (now.lift - mPrevious.lift) * alpha;
    const double squash = mPrevious.squash + (now.squash - mPrevious.squash) * alpha;

    Placement p;
    p.x = x;
    p.lift = static_cast<float>(std::max(0.0, lift));
    p.scaleY = static_cast<float>(1.0 + squash);
    p.scaleX = static_cast<float>(1.0 - squash * mTuning.squashWidthRatio);
    return p;
}

bool Motion::animating() const {
    if (mPhase != Phase::Landing) return true;
    const double eps = mTuning.settleEpsPx;
    const double veps = mTuning.settleEpsPxPerS;
    // Horizontally there is no target to converge on once released - she keeps
    // her position - so "settled" there means "no longer coasting".
    return std::fabs(mX.velocity) > veps || std::fabs(mLift.value) > eps ||
           std::fabs(mLift.velocity) > veps || std::fabs(mSquash.value) > 1e-4 ||
           std::fabs(mSquash.velocity) > 1e-3;
}

int Motion::headroom() const {
    return static_cast<int>(
        std::ceil(mBoxHeight * (mTuning.maxLift + mTuning.liftStretch)));
}

}  // namespace asuna
