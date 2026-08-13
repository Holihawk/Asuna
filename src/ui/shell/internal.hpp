#pragma once

#include <glib.h>

// The handful of Shell constants that more than one shell/*.cpp unit needs.
// Everything else stays in the anonymous namespace of the file that uses it -
// this header exists to share three numbers, not to become a dumping ground.
namespace asuna {

// The range the scroll wheel and `asuna scale` may move her through. Read in
// two places: the constructor, clamping whatever the state file remembered, and
// setUserScale, clamping every change after that.
constexpr float kMinUserScale = 0.5f;
constexpr float kMaxUserScale = 2.5f;

// How long after she is placed before she says hello. Deferred rather than
// immediate because the first frame arrives before GTK has laid the overlay
// out - and because she reads better appearing first and speaking second. The
// ASUNA_DEBUG_* hooks hang their own timers off multiples of it, so that they
// always land after the greeting rather than in the middle of it.
constexpr guint kGreetDelayMs = 400;

}  // namespace asuna
