#pragma once

namespace asuna {

// Feel constants for the drag, the lift and the landing.
//
// Adapted from the Neri project's motion model: the same spring family and the
// same three-phase vertical machine, retuned for a bust that stands on the
// screen edge rather than a full body tucked below it. The numbers that carried
// over unchanged are the ones that encode *feel* rather than geometry.
struct MotionTuning {
    // Critically damped follow while dragging. The lag is the point, not a
    // defect: the gap between where the pointer is and where she is becomes her
    // velocity, and her velocity is what loads the hair chains.
    float dragTau = 0.09f;

    // Lift the moment she is picked up, as a fraction of her box height. Small
    // enough to read as "held" rather than "thrown".
    float grabLift = 0.035f;
    // Rubber-band asymptote when the pointer keeps pulling upward. Also sets the
    // headroom the strip has to reserve above her.
    float maxLift = 0.09f;

    // Downward acceleration on release, px/s². Tuned by fall *time*: from full
    // lift she is back on the edge in about 0.13 s.
    float fallGravity = 4200.0f;
    // Landing spring, entered carrying the impact velocity, so its amplitude
    // scales with the drop for free.
    float landingOmega = 45.0f;
    float landingZeta = 0.42f;

    // Squash spring. Negative compresses her, positive stretches her.
    float squashOmega = 30.0f;
    float squashZeta = 0.28f;
    float squashPerImpact = 1.4e-4f;   // squash units per px/s of impact
    float maxSquash = 0.12f;           // a pathological fall must not fold her flat
    float squashWidthRatio = 0.5f;     // how much vertical squash trades into width
    float liftStretch = 0.025f;        // vertical stretch at full lift

    // Below this displacement and speed nothing more would be visible, so the
    // frame loop can go back to its idle cap.
    float settleEpsPx = 0.05f;
    float settleEpsPxPerS = 0.5f;
};

// Where and how to draw her this frame. `lift` is px above the strip's bottom
// edge; the scales are multipliers on her box, applied about her bottom centre.
struct Placement {
    double x = 0.0;
    float lift = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

// Her horizontal position and vertical state, integrated at a fixed rate and
// interpolated for display.
//
// Nothing here touches GL, GTK or the model, which is the point: the entire
// drag feel is testable offline (tests/motion_test.cpp) rather than only by
// hand on a live desktop.
class Motion {
public:
    explicit Motion(MotionTuning tuning = {});

    // Teleport, killing velocity. For placement, not for animation.
    void reset(double x);
    // Travel limits for her box's left edge, and her box height (which is what
    // the lift and squash fractions are measured against).
    void setBounds(double minX, double maxX, float boxHeight);

    void beginDrag();
    // The position the pointer is asking for, plus how far above the grab point
    // it has travelled. Only meaningful between beginDrag() and release().
    void hold(double targetX, float pullUp);
    void release();
    bool dragging() const { return mPhase == Phase::Held; }

    // Walk her to a resting position under the same spring the drag uses,
    // instead of teleporting. Her feet stay on the edge throughout - this is
    // "walk over there", not "be picked up and put down".
    void glideTo(double x);

    // Runs however many fixed sim ticks `frameDt` seconds are worth, keeping the
    // remainder for interpolation.
    void advance(float frameDt);

    Placement placement() const;
    double velocityX() const { return mX.velocity; }
    double liftVelocity() const { return mLift.velocity; }

    // True while another frame would look different from this one. Drives both
    // the uncapped frame rate during motion and the return to the idle cap.
    bool animating() const;

    // Px of empty space the strip must carry above her box for the lift and the
    // stretch to have somewhere to go.
    int headroom() const;

    const MotionTuning& tuning() const { return mTuning; }
    MotionTuning& tuning() { return mTuning; }

private:
    struct Spring {
        double value = 0.0;
        double velocity = 0.0;
        // One semi-implicit Euler step. Stable while omega*dt < 2, which at the
        // fixed sim rate leaves an enormous margin over the stiffest spring here.
        void step(double target, double omega, double zeta, double dt);
        void snap(double v) { value = v; velocity = 0.0; }
    };

    enum class Phase {
        Held,      // following the pointer through a critically damped spring
        Falling,   // released above the edge: integrating under gravity
        Landing,   // an underdamped settle carrying the impact velocity
        Gliding,   // travelling to a commanded position, feet on the ground
    };

    struct Sim {
        double x = 0.0;
        double lift = 0.0;
        double squash = 0.0;
    };

    void tick(double dt);
    void land();
    double liftTarget(double pull) const;
    Sim current() const { return {mX.value, mLift.value, mSquash.value}; }

    MotionTuning mTuning;
    Spring mX, mLift, mSquash;
    Phase mPhase = Phase::Landing;
    Sim mPrevious;
    double mMinX = 0.0, mMaxX = 0.0;
    float mBoxHeight = 1.0f;
    double mTargetX = 0.0;
    float mPullUp = 0.0f;
    float mAccumulator = 0.0f;
};

}  // namespace asuna
