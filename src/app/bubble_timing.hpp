#pragma once

namespace asuna {

// How long a line stays up: base + perGlyph x its length, capped at max.
// Counted in codepoints rather than bytes - every line in the dialogue file is
// Chinese, where a byte count would triple the duration. Kept outside the GTK
// bubble so application options remain toolkit-free.
struct BubbleTiming {
    double base = 2.0;
    double perGlyph = 0.20;
    double max = 9.0;
};

}  // namespace asuna
