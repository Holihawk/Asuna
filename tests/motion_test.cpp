// Offline tests for the drag feel (src/pet/motion.cpp). No GL, no compositor, no
// pointer - which is the whole reason the motion model is a separate module.
// Everything a user would have to feel by hand is asserted here instead.

#include "pet/motion.hpp"

#include <algorithm>
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
    printf("  FAIL %s:%d  %s: got %.4f, want %.4f +- %.4f\n", current, line, what,
           got, want, tol);
    ++failures;
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_NEAR(got, want, tol) checkNear((got), (want), (tol), #got, __LINE__)

constexpr float kBoxH = 460.0f;
constexpr double kMinX = 0.0;
constexpr double kMaxX = 1617.0;   // 1920 strip - 303 box

asuna::Motion make(double x = 800.0) {
    asuna::Motion m;
    m.setBounds(kMinX, kMaxX, kBoxH);
    m.reset(x);
    return m;
}

// Runs `seconds` of frames at `fps`, calling `perFrame(t)` before each advance.
template <typename F>
void run(asuna::Motion& m, double seconds, double fps, F perFrame) {
    const double dt = 1.0 / fps;
    const int frames = static_cast<int>(seconds * fps);
    for (int i = 0; i < frames; ++i) {
        perFrame(i * dt);
        m.advance(static_cast<float>(dt));
    }
}

void run(asuna::Motion& m, double seconds, double fps) {
    run(m, seconds, fps, [](double) {});
}

// --- the tests ------------------------------------------------------------

void restIsStill() {
    auto m = make();
    run(m, 1.0, 60);
    CHECK(!m.animating());
    CHECK_NEAR(m.placement().x, 800.0, 1e-6);
    CHECK_NEAR(m.placement().lift, 0.0, 1e-6);
    CHECK_NEAR(m.placement().scaleY, 1.0, 1e-6);
}

void dragReachesItsTarget() {
    auto m = make();
    m.beginDrag();
    run(m, 0.5, 60, [&](double) { m.hold(1000.0, 0.0); });
    // 0.5 s is ~5.5 time constants; anything slower would feel like syrup.
    CHECK_NEAR(m.placement().x, 1000.0, 1.0);
}

void dragDoesNotOvershoot() {
    auto m = make(800.0);
    m.beginDrag();
    double peak = 800.0;
    run(m, 1.0, 240, [&](double) {
        m.hold(1000.0, 0.0);
        peak = std::max(peak, m.placement().x);
    });
    // Critically damped: semi-implicit Euler adds a hair of energy, so allow a
    // fraction of a percent rather than demanding exactly none.
    CHECK(peak <= 1000.4);
}

// The one that matters for the hair: a steady drag has to produce a steady
// velocity, because velocity is what the lean is computed from. If she snapped
// to the pointer this would be zero and the chains would never load.
void steadyDragProducesSteadyVelocity() {
    auto m = make(400.0);
    m.beginDrag();
    const double speed = 500.0;   // px/s
    run(m, 0.8, 144, [&](double t) { m.hold(400.0 + speed * t, 0.0); });
    CHECK_NEAR(m.velocityX(), speed, speed * 0.05);
}

void grabLifts() {
    auto m = make();
    m.beginDrag();
    run(m, 0.4, 60, [&](double) { m.hold(800.0, 0.0); });
    const float want = 0.035f * kBoxH;
    CHECK_NEAR(m.placement().lift, want, want * 0.05);
    CHECK(m.placement().scaleY > 1.0f);   // stretched while held
}

void pullingUpRubberBands() {
    auto m = make();
    m.beginDrag();
    // Ask for far more lift than the ceiling allows.
    run(m, 1.0, 120, [&](double) { m.hold(800.0, 5000.0f); });
    const float ceiling = 0.09f * kBoxH;
    CHECK(m.placement().lift <= ceiling + 0.5f);
    CHECK(m.placement().lift > 0.9f * ceiling);
    // Headroom must actually cover where she can get to.
    CHECK(m.headroom() >= static_cast<int>(m.placement().lift));
}

void releaseFallsAndLands() {
    auto m = make();
    m.beginDrag();
    run(m, 0.5, 120, [&](double) { m.hold(800.0, 200.0f); });
    const float held = m.placement().lift;
    CHECK(held > 10.0f);

    m.release();
    // Falls back to the edge quickly - a drop that takes longer than a blink
    // reads as floating, not as being put down.
    run(m, 0.25, 120);
    CHECK_NEAR(m.placement().lift, 0.0, 2.0);
}

void landingSquashes() {
    auto m = make();
    m.beginDrag();
    run(m, 0.5, 120, [&](double) { m.hold(800.0, 200.0f); });
    m.release();

    double lowest = 1.0;
    run(m, 0.6, 240, [&](double) { lowest = std::min<double>(lowest, m.placement().scaleY); });
    CHECK(lowest < 0.995);            // she visibly compresses on impact
    CHECK(lowest > 1.0 - 0.12 - 0.01);  // but never past maxSquash
}

void settlesAfterRelease() {
    auto m = make();
    m.beginDrag();
    run(m, 0.4, 120, [&](double) { m.hold(1200.0, 200.0f); });
    m.release();
    CHECK(m.animating());
    run(m, 2.0, 120);
    CHECK(!m.animating());
    // She stays where she was let go, rather than springing back.
    CHECK_NEAR(m.placement().x, 1200.0, 2.0);
}

void clampsAtTheWalls() {
    auto m = make(1500.0);
    m.beginDrag();
    run(m, 1.0, 120, [&](double) { m.hold(9999.0, 0.0); });
    CHECK_NEAR(m.placement().x, kMaxX, 0.001);
    // Not exactly zero: the clamp only fires when the value crosses the wall, so
    // an approach that stops just short leaves a vanishing residue behind.
    CHECK_NEAR(m.velocityX(), 0.0, 0.01);

    m.release();
    run(m, 0.5, 120);
    CHECK_NEAR(m.placement().x, kMaxX, 0.001);
}

// The reason for the fixed sim tick: the same gesture must land in the same
// place on a 60 Hz panel and on a 144 Hz one. Tested with a held target, so the
// only difference between the two runs is how the elapsed time was chopped up.
//
// (A *moving* target does diverge between frame rates, by about half a target
// step - but that is input sampling, not integration, and it does not arise in
// the app: the pointer updates the target from motion events at device rate,
// not once per rendered frame.)
void framerateIndependence() {
    auto slow = make(400.0);
    auto fast = make(400.0);
    slow.beginDrag();
    fast.beginDrag();
    slow.hold(1200.0, 120.0f);
    fast.hold(1200.0, 120.0f);
    run(slow, 1.0, 30);
    run(fast, 1.0, 144);
    CHECK_NEAR(slow.placement().x, fast.placement().x, 0.01);
    CHECK_NEAR(slow.placement().lift, fast.placement().lift, 0.01);
    CHECK_NEAR(slow.velocityX(), fast.velocityX(), 0.01);
}

// A frame delta from a resumed suspend must not catapult her.
void hugeFrameDeltaIsClamped() {
    auto m = make(400.0);
    m.beginDrag();
    m.hold(1600.0, 0.0);
    m.advance(30.0f);   // half a minute in one frame
    CHECK(m.placement().x >= kMinX && m.placement().x <= kMaxX);
    CHECK(m.placement().x < 1000.0);   // one clamped step, not the whole journey
}

void headroomCoversLiftAndStretch() {
    auto m = make();
    // maxLift 0.09 + liftStretch 0.025 of a 460 px box.
    CHECK(m.headroom() >= 52);
    CHECK(m.headroom() <= 60);
}

// "Reset position" walks her there rather than teleporting, and unlike a drag
// her feet never leave the edge.
void glideWalksHerOverWithoutLifting() {
    auto m = make(1500.0);
    m.glideTo(200.0);
    float highest = 0;
    run(m, 2.0, 60.0, [&](double) { highest = std::max(highest, m.placement().lift); });
    CHECK_NEAR(m.placement().x, 200.0, 1.0);
    CHECK_NEAR(highest, 0.0, 1e-3);
    CHECK(!m.animating());   // and it gives the frame loop its idle cap back
}

// A glide must not fight the pointer: whoever is carrying her wins.
void glideIsIgnoredMidDrag() {
    auto m = make(800.0);
    m.beginDrag();
    m.hold(1200.0, 0.0);
    m.glideTo(0.0);
    run(m, 1.0, 60.0, [&](double) { m.hold(1200.0, 0.0); });
    CHECK_NEAR(m.placement().x, 1200.0, 1.0);
}

struct Test { const char* name; void (*fn)(); };

}  // namespace

int main() {
    const Test tests[] = {
        {"restIsStill", restIsStill},
        {"dragReachesItsTarget", dragReachesItsTarget},
        {"dragDoesNotOvershoot", dragDoesNotOvershoot},
        {"steadyDragProducesSteadyVelocity", steadyDragProducesSteadyVelocity},
        {"grabLifts", grabLifts},
        {"pullingUpRubberBands", pullingUpRubberBands},
        {"releaseFallsAndLands", releaseFallsAndLands},
        {"landingSquashes", landingSquashes},
        {"settlesAfterRelease", settlesAfterRelease},
        {"clampsAtTheWalls", clampsAtTheWalls},
        {"framerateIndependence", framerateIndependence},
        {"hugeFrameDeltaIsClamped", hugeFrameDeltaIsClamped},
        {"headroomCoversLiftAndStretch", headroomCoversLiftAndStretch},
        {"glideWalksHerOverWithoutLifting", glideWalksHerOverWithoutLifting},
        {"glideIsIgnoredMidDrag", glideIsIgnoredMidDrag},
    };

    for (const Test& t : tests) {
        current = t.name;
        const int before = failures;
        t.fn();
        printf("%-34s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall %zu tests passed\n", sizeof(tests) / sizeof(tests[0]));
    return 0;
}
