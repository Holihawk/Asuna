#include "UtSystem.hpp"
#include <chrono>

namespace live2d {

// Milliseconds since the first call, not since the machine booted.
//
// steady_clock's epoch on Linux is CLOCK_MONOTONIC, so this used to hand back a
// number as large as the uptime. Every caller that holds it in a float then
// loses the low bits: a 24-bit mantissa resolves 64 ms once the value reaches
// nine days' worth of milliseconds, which quantises every wall-clock-driven
// animation into a staircase no matter how fast we render. Rebasing to the
// first call bounds the magnitude by how long asuna has been up rather than by
// how long the machine has.
//
// The callers were widened to double as well, so this is belt and braces. It is
// kept because it is what makes the float case merely survivable: any caller
// re-vendored from upstream, which is float throughout, stays well conditioned
// for the length of a plausible session instead of degrading with the host's
// uptime - a number this process does not control and cannot see coming.
double UtSystem::getUserTimeMSec() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start)
        .count();
}

} // namespace live2d
