#include "app/cli/internal.hpp"

#include <cstdio>

#include "app/argparse.hpp"
#include "app/config.hpp"
#include "pet/outfits.hpp"

namespace asuna {
namespace cli {

// --- options ----------------------------------------------------------------

// Reads the config file into `opt` before a single flag is looked at, which is
// the whole of how "a flag beats the config file" is implemented: the flags are
// parsed straight over the top of it.
//
// A broken config refuses to start rather than starting wrong. Everything else
// here treats a missing piece as "use the default", but a config file is
// something the user wrote on purpose and is watching for the effect of, and
// twenty minutes of wondering why line 12 does nothing is worse than an error.
// The other half of the same argument, for the settings that parse and are in
// range and still do nothing because another one overrides them. Said, and then
// carried on from - see the note on Config::warnings for why these are not
// problems. Printed under the path so it reads the same as a complaint does.
void sayWarnings(const Config& cfg) {
    if (cfg.warnings.empty()) return;
    fprintf(stderr, "asuna: %s\n", cfg.source.empty() ? Config::path().c_str()
                                                      : cfg.source.c_str());
    for (const std::string& w : cfg.warnings) fprintf(stderr, "  %s\n", w.c_str());
}

int applyConfig(ShellOptions* opt) {
    Config cfg;
    cfg.load(Config::path());
    if (!cfg.problems.empty()) {
        fprintf(stderr, "asuna: %s\n", Config::path().c_str());
        for (const std::string& p : cfg.problems) fprintf(stderr, "  %s\n", p.c_str());
        return kUsage;
    }
    sayWarnings(cfg);
    cfg.applyTo(opt);
    return kOk;
}

// Fills `opt` from argv[from..]. Returns kOk, or kUsage having said why.
//
// The numeric flags are read through argparse rather than atoi/atof, and the
// ranges are the ones config/config.cpp already enforces on the same settings: a
// `strip.height` of 40 is refused in the config file, so `--height 40` has no
// business being accepted here. The difference is what happens next - the
// config file reports the problem and carries on with the default, because the
// rest of the file is still worth reading, whereas a flag is the only thing the
// user just asked for and getting it wrong should stop the launch.
int parseOptions(int argc, char** argv, int from, ShellOptions* opt, bool* foreground) {
    for (int i = from; i < argc; ++i) {
        const std::string a = argv[i];
        bool missing = false;
        std::string problem;
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                missing = true;
                return "";
            }
            return argv[++i];
        };
        // Both readers leave `opt` alone and set `problem` when the text is not
        // a value, so a rejected flag cannot half-apply. `missing` is checked
        // after the chain, and wins: "--height needs a value" beats complaining
        // that the empty string is not a number.
        auto whole = [&](int lo, int hi, int fallback) -> int {
            const std::string v = next();
            int n = fallback;
            if (!missing) argparse::integerIn(a, v, lo, hi, &n, &problem);
            return n;
        };
        auto fraction = [&](double lo, double hi, double fallback) -> double {
            const std::string v = next();
            double n = fallback;
            if (!missing) argparse::realIn(a, v, lo, hi, &n, &problem);
            return n;
        };
        // A floor and no ceiling. Distinct from fraction(lo, kNoMax, ...),
        // which is a two-sided range that happens to end at INT_MAX.
        auto atLeast = [&](double lo, double fallback) -> double {
            const std::string v = next();
            double n = fallback;
            if (!missing) argparse::realAtLeast(a, v, lo, &n, &problem);
            return n;
        };
        if (a == "--model") opt->model = resolveModelArg(next());
        else if (a == "--layer") opt->layer = next();
        else if (a == "--output") opt->output = next();
        else if (a == "--framing") opt->framing = next();
        else if (a == "--anchor") opt->anchor = next();
        else if (a == "--height") opt->stripHeight = whole(80, argparse::kNoMax, opt->stripHeight);
        else if (a == "--max-height") opt->maxHeight = whole(80, argparse::kNoMax, opt->maxHeight);
        else if (a == "--margin") opt->margin = whole(0, argparse::kNoMax, opt->margin);
        else if (a == "--bottom") opt->bottomMargin = whole(0, argparse::kNoMax, opt->bottomMargin);
        else if (a == "--language") opt->language = next();
        else if (a == "--pad") opt->pad = whole(0, argparse::kNoMax, opt->pad);
        else if (a == "--band") opt->bubbleBand = whole(0, argparse::kNoMax, opt->bubbleBand);
        else if (a == "--side")
            opt->sideBleed = static_cast<float>(fraction(0.0, 1.0, opt->sideBleed));
        else if (a == "--gaze-halo") opt->gazeHalo = whole(0, argparse::kNoMax, opt->gazeHalo);
        else if (a == "--no-greet") opt->greet = false;
        else if (a == "--scale") opt->scale = static_cast<float>(fraction(0.5, 2.5, opt->scale));
        else if (a == "--x") opt->x = atLeast(0.0, opt->x);
        else if (a == "--fps") opt->fps = whole(0, argparse::kNoMax, opt->fps);
        else if (a == "--hidden") opt->hidden = 1;
        else if (a == "--show") opt->hidden = 0;
        else if (a == "--no-persist") opt->persist = false;
        else if (a == "--no-control") opt->control = false;
        else if (a == "--foreground" && foreground) *foreground = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return -1;   // handled, exit 0
        } else {
            fprintf(stderr, "asuna: unknown argument '%s'\n", a.c_str());
            return kUsage;
        }
        if (missing) {
            fprintf(stderr, "asuna: %s needs a value\n", a.c_str());
            return kUsage;
        }
        if (!problem.empty()) {
            fprintf(stderr, "asuna: %s\n", problem.c_str());
            return kUsage;
        }
    }
    return kOk;
}

}  // namespace cli
}  // namespace asuna
