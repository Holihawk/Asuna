#pragma once

#include <string>
#include <vector>

// Turning user-supplied text into typed values, strictly.
//
// This is deliberately a leaf: no config, no paths, no toolkit, nothing but the
// standard library. Everything here is a pure function of its input, which is
// what lets `asuna-argparse-test` cover the boundary and overflow cases without
// a daemon, a socket or a compositor in the way.
//
// The house rule for all of it: text that is not exactly a value is an error,
// never a silent zero and never a partial read. `atoi("nope")` is 0 and
// `atof("1.2.3")` is 1.2, and both of those reach the user as something that
// looks like it worked.
namespace asuna {
namespace argparse {

// No upper bound. Spelled out because `integerIn(f, t, 0, INT_MAX, ...)` reads
// as a real ceiling, and the message it produces should say "at least 0".
constexpr int kNoMax = 2147483647;

// A whole number and nothing else: optional sign, digits, end of string. No
// leading or trailing space, no trailing unit, no empty string, and a value
// that does not fit in an int is out of range rather than truncated.
bool integer(const std::string& text, int* out);

// A real number and nothing else. Accepts what strtod does - including
// exponents - minus the three things that are never a sensible answer to "how
// tall" or "how far": nan, inf, and a partially consumed string.
bool real(const std::string& text, double* out);

// The two above, range-checked, with a complete user-facing sentence written
// into `error` when they fail. `label` is the flag or argument being read, so
// the message names the thing the user actually typed.
//
//   --height needs a whole number, not 'tall'
//   --height must be at least 80
//   --scale must be between 0.5 and 2.5
bool integerIn(const std::string& label, const std::string& text, int lo, int hi, int* out,
               std::string* error);
bool realIn(const std::string& label, const std::string& text, double lo, double hi, double* out,
            std::string* error);

// Half-open versions, for the settings that have a floor but no machine-
// independent ceiling. `kNoMax` must not be used as `hi` above to fake this:
// it is a real int, `realIn` really does compare against it, and a pixel
// position of three billion would be refused by a message about zero.
//
//   realAtLeast  v >= lo    --x, which cannot be negative but has no maximum
//   realAbove    v >  lo    say --for, where zero already means "no timer"
bool realAtLeast(const std::string& label, const std::string& text, double lo, double* out,
                 std::string* error);
bool realAbove(const std::string& label, const std::string& text, double lo, double* out,
               std::string* error);

// A real number with no range at all, for the values the daemon itself clamps.
// Still rejects malformed text, which is the whole point.
bool realAny(const std::string& label, const std::string& text, double* out, std::string* error);

// Splits a configured command string - `[ext] command`, `[ext] prompt_command` -
// into an argv.
//
// This is a quoting grammar, not a shell. It is the POSIX subset that
// `shlex.split` implements, so the C++ and Python ends of the same feature
// agree on what a config line means:
//
//   * words are separated by spaces, tabs, newlines and carriage returns;
//   * '...' is literal, with no escapes inside it;
//   * "..." processes \" and \\ only, and keeps any other backslash as it is;
//   * a backslash outside quotes takes the next character literally;
//   * "" is a real, empty argument.
//
// What it deliberately does not do is everything else a shell does: no $VAR, no
// globbing, no ~, no pipes, no redirection, no substitution. The command is
// still handed straight to execvp. The original reasoning for splitting on
// spaces alone was that "a command run through `sh -c` is a command that can be
// made to do anything by a config file" - that reasoning is about `sh -c`, and
// it survives intact here: quoting only decides where one argument ends and the
// next begins.
//
// Returns false, with `error` set, on an unterminated quote or a trailing
// backslash. An empty or all-whitespace string is not an error - it yields an
// empty argv, and the caller decides what that means.
bool splitCommand(const std::string& text, std::vector<std::string>* argv, std::string* error);

}  // namespace argparse
}  // namespace asuna
