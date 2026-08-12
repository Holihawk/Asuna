/**
 * Phase 0 gate: prove the vendored Cubism 2.1 C++ port renders *our* asuna_02
 * model. No layer-shell, no transparency plumbing, no pet logic - just load,
 * animate and dump pixels so the result can be inspected.
 *
 * Renders into an FBO with an alpha channel and writes a binary PAM (P7,
 * RGB_ALPHA). Alpha is captured on purpose: a transparent background is a hard
 * requirement for Phase 2, so it is worth confirming this early.
 *
 *   ./asuna-render-test [--model P] [--out P] [--size WxH] [--frames N]
 *                       [--expression NAME] [--motion GROUP] [--window]
 *
 * It has since grown three checks that need a real GL context and the shipping
 * framing solver, and so live here rather than in the offline test suites:
 *
 *   --hit           the pointer <-> canvas transform and the hit areas
 *   --extent        how far outside its box a pose can reach (sizes --side)
 *   --below         how much artwork is under the crop line (sizes --bleed)
 *   --drop N        slide the framing N px down inside the viewport
 *   --motion-sweep  the same, playing every motion the outfit has, in real time
 *   --motion-name N play one named motion and dump its *widest* frame
 */

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include "GLCompat.hpp"   // GLES 3 or glad desktop GL, matching how V2 was built

#include "LAppModel.hpp"
#include "Log.hpp"
#include "pet/framing.hpp"
#include "json.hpp"
#include "pet/pet.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <memory>
#include <vector>

namespace {

struct Options {
    std::string model = "models/asuna_02/index.json";
    std::string out = "build/phase0.pam";
    std::string expression;
    std::string motion = "";      // our index.json uses "" as the default group
    int width = 600;
    int height = 800;
    int frames = 60;
    float scale = 1.0f;
    // Empty means the raw path: use --scale verbatim and let the model sit
    // centred in the viewport. Otherwise run the production framing solver.
    std::string framing;
    // Extra models to load into the same GL context after the first, which is
    // exactly what a costume change does. Catches texture/program leaks and
    // state left behind by the previous outfit.
    std::string switchTo;
    int pad = 8;
    int bleed = 0;   // extra body rendered below the visible bottom edge
    // Slide the framing down inside the viewport by this many px. What
    // Pet::fit derives per outfit; exposed here so a chosen value can be
    // rendered and looked at, and so --below can confirm it closed the gap.
    int drop = 0;
    float side = 0.0f;   // extra width rendered past each side, as a fraction
    bool window = false;
    // Holds the drag lean at this value and traces what it does to the model:
    // the host sets the lean, the runtime turns it into PARAM_ANGLE_* before the
    // physics pass, and the physics turns that into hair sway. Any break in that
    // chain shows up as a flat column here.
    float lean = 0.0f;
    // Verify the pointer <-> canvas mapping and the hit areas instead of
    // rendering. This is the one part of the input path that can be checked
    // without a pointer, and it is the part everything else in Phase 4 stands
    // on: get it wrong and every tap reacts as the wrong body part.
    bool hit = false;
    // Sweep the model's parameter space and report how far outside its box the
    // artwork can reach. This is what sizes the side bleed: a pose that goes
    // wider than the viewport does not overflow, it gets cut off.
    bool extent = false;
    // Measure how much artwork sits below the crop line instead of rendering.
    // This is what sizes the downward bleed, and the only way to tell a crop
    // that ends at her feet from one that ends at the edge of the canvas.
    bool below = false;
    // With --extent: also play every motion the outfit has and measure what
    // they actually reach. Real time, because the runtime's motion manager runs
    // off the wall clock.
    bool motionSweep = false;
    // Play one named motion (e.g. "I_ANGRY") instead of a random one from a
    // group, and dump the *widest* frame of it rather than the last. The widest
    // instant of a motion is transient, which is exactly why side clipping is
    // hard to catch by taking a screenshot and hoping.
    std::string motionName;
    // --param ID=VALUE, repeatable. Written every frame *after* update() and
    // before draw(), which is exactly where Pet applies its own overrides - the
    // runtime runs the deformer chain in draw(), so a value set in that window
    // beats the motion, the expression and the blink for that frame.
    //
    // This is how a face the model does not ship gets tuned against pixels:
    // F_SLEEP keeps a smiling mouth, and the value that fixes it is a number
    // nobody can guess from the parameter's name.
    std::vector<std::pair<std::string, float>> params;
};

int findParam(const live2d::LAppModel& model, const char* id) {
    for (int i = 0; i < model.getParameterCount(); ++i)
        if (model.getParameterId(i) == id) return i;
    return -1;
}

// Same shorthand the app accepts: "31" means models/asuna_31/index.json.
std::string resolveModel(const std::string& arg) {
    if (arg.find('/') != std::string::npos || arg.size() > 3) return arg;
    return "models/asuna_" + arg + "/index.json";
}

void onGlfwError(int code, const char* msg) {
    fprintf(stderr, "[GLFW ERROR %d] %s\n", code, msg);
}

bool writePam(const std::string& path, const std::vector<unsigned char>& rgba,
              int w, int h) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s for writing\n", path.c_str());
        return false;
    }
    fprintf(f,
            "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\n"
            "TUPLTYPE RGB_ALPHA\nENDHDR\n",
            w, h);
    // glReadPixels gives bottom-up rows; PAM wants top-down.
    for (int y = h - 1; y >= 0; --y)
        fwrite(rgba.data() + static_cast<size_t>(y) * w * 4, 1, w * 4, f);
    fclose(f);
    return true;
}

// Coverage/opacity stats, so the gate can be judged without eyeballing alone.
void reportPixels(const std::vector<unsigned char>& rgba, int w, int h) {
    size_t opaque = 0, partial = 0, clear = 0;
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] == 0) ++clear;
        else if (rgba[i] == 255) ++opaque;
        else ++partial;
    }
    const double total = static_cast<double>(w) * h;
    printf("  pixels: %.1f%% opaque, %.1f%% partial, %.1f%% transparent\n",
           100.0 * opaque / total, 100.0 * partial / total, 100.0 * clear / total);
}

// --- hit testing ------------------------------------------------------------

int hitFailures = 0;

void expect(bool ok, const char* what) {
    if (ok) return;
    printf("    FAIL %s\n", what);
    ++hitFailures;
}

void expectNear(double got, double want, double tol, const char* what) {
    if (std::fabs(got - want) <= tol) return;
    printf("    FAIL %s: got %.2f, want %.2f +- %.2f\n", what, got, want, tol);
    ++hitFailures;
}

// Round trip a grid of canvas points through the transform and back.
void checkRoundTrip(const asuna::Pet& pet, const asuna::Placement& p, const char* label) {
    const asuna::Fit& fit = pet.fitResult();
    double worst = 0;
    for (int i = 0; i <= 8; ++i) {
        for (int j = 0; j <= 8; ++j) {
            const float mx = fit.target[0] + (fit.target[2] - fit.target[0]) * i / 8.0f;
            const float my = fit.target[1] + (fit.target[3] - fit.target[1]) * j / 8.0f;
            double sx = 0, sy = 0;
            float bx = 0, by = 0;
            if (!pet.modelToScreen(p, mx, my, &sx, &sy)) { expect(false, "modelToScreen"); return; }
            if (!pet.screenToModel(p, sx, sy, &bx, &by)) { expect(false, "screenToModel"); return; }
            worst = std::max(worst, static_cast<double>(std::fabs(bx - mx)));
            worst = std::max(worst, static_cast<double>(std::fabs(by - my)));
        }
    }
    printf("    round trip %-22s worst error %.4f canvas units\n", label, worst);
    expect(worst < 0.01, "round trip is exact");
}

int runHitTest(const Options& opt) {
    asuna::Pet pet;
    if (!pet.load(opt.model)) return 1;
    if (!opt.framing.empty() && opt.framing != "auto")
        pet.setFraming(asuna::parseFraming(opt.framing.c_str()));
    pet.fit(opt.height, opt.pad, opt.bleed, opt.side);

    const asuna::Fit& fit = pet.fitResult();
    printf("  framing=%s box=%dx%d bleed=%d\n", asuna::framingName(pet.framing()),
           fit.boxWidth, fit.boxHeight, fit.bleed);

    asuna::Placement rest;
    rest.x = 640;

    // The framing solver promises the target rectangle lands exactly inside the
    // visible box with `pad` px around it. Checking the transform against that
    // promise tests the two against each other: they are derived separately, so
    // agreeing to a fraction of a pixel is real evidence, not a tautology.
    {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        pet.modelToScreen(rest, fit.target[0], fit.target[3], &x0, &y0);
        pet.modelToScreen(rest, fit.target[2], fit.target[1], &x1, &y1);
        printf("    target rect on screen: x %.2f..%.2f  y %.2f..%.2f (box %dx%d, pad %d)\n",
               x0 - rest.x, x1 - rest.x, y0, y1, fit.boxWidth, fit.boxHeight, opt.pad);
        expectNear(x0 - rest.x, opt.pad, 1.0, "target left sits pad px inside the box");
        expectNear(x1 - rest.x, fit.boxWidth - opt.pad, 1.0, "target right");
        expectNear(y0, opt.pad, 1.0, "target bottom sits pad px above the screen edge");
        expectNear(y1, fit.boxHeight - opt.pad, 1.0, "target top");
    }

    checkRoundTrip(pet, rest, "at rest");

    // Carried, and mid-landing-squash: the same mapping has to hold, or a tap
    // during a drag reacts as the wrong part of her.
    asuna::Placement lifted;
    lifted.x = 200;
    lifted.lift = 40.0f;
    lifted.scaleY = 1.025f;
    lifted.scaleX = 0.987f;
    checkRoundTrip(pet, lifted, "lifted and stretched");

    asuna::Placement squashed;
    squashed.x = 1301;
    squashed.scaleY = 0.9f;
    squashed.scaleX = 1.05f;
    checkRoundTrip(pet, squashed, "mid landing squash");

    // Sweep the box and see which area answers where. The vertical order of the
    // answers is the check: head above chest above body above foot is the one
    // thing that must be true of any outfit, and it is exactly what a wrong
    // transform (or the runtime's own hitTest, which assumes a projection we do
    // not use) gets wrong.
    for (const asuna::Placement& p : {rest, lifted}) {
        std::map<std::string, std::pair<double, int>> centroid;   // sum of y, count
        for (int sy = 0; sy < fit.boxHeight; sy += 3) {
            for (int sx = 0; sx < fit.boxWidth; sx += 3) {
                const std::string area = pet.hitAreaAt(p, p.x + sx, sy);
                if (area.empty()) continue;
                centroid[area].first += sy;
                centroid[area].second += 1;
            }
        }
        std::vector<std::pair<double, std::string>> byHeight;
        for (const auto& kv : centroid)
            byHeight.emplace_back(kv.second.first / kv.second.second, kv.first);
        std::sort(byHeight.rbegin(), byHeight.rend());

        printf("    areas found (top to bottom, lift=%.0f):", p.lift);
        for (const auto& kv : byHeight) printf(" %s@%.0f", kv.second.c_str(), kv.first);
        printf("\n");

        expect(centroid.count("head") == 1, "the head is reachable");
        expect(centroid.count("chest") == 1, "the chest is reachable");
        static const std::vector<std::string> order = {"head", "chest", "body", "foot"};
        int previous = -1;
        for (const auto& kv : byHeight) {
            const int rank = static_cast<int>(
                std::find(order.begin(), order.end(), kv.second) - order.begin());
            expect(rank > previous, "hit areas run head to foot down the body");
            previous = rank;
        }
    }

    // Outside the rendered area entirely - past the side bleed as well as the
    // box, since the bleed is drawn and a point inside it is a real canvas
    // position, just not one any hit-area marker covers.
    const int wide = fit.boxWidth + 2 * fit.sideBleed;
    expect(pet.hitAreaAt(rest, rest.x - fit.sideBleed - 50, 100).empty(),
           "nothing left of what she covers");
    expect(pet.hitAreaAt(rest, rest.x - fit.sideBleed + wide + 50, 100).empty(),
           "nothing right of it");

    printf("\n  %s\n", hitFailures ? "HIT TEST FAILED" : "hit test passed");
    return hitFailures ? 1 : 0;
}

// --- pose envelope ----------------------------------------------------------
//
// How far outside its box the artwork can reach once she moves.
//
// The viewport is the clip: nothing outside the box is drawn, so a motion that
// throws an elbow sideways loses the hand off the edge. What has to be measured
// is the *rendered alpha*, over the rows the strip actually shows - the model's
// own figure bounds are no use here, because they include everything below a
// bust crop, and a hand at knee height is clipped by the crop long before the
// side of the box matters.
//
// So: render into a deliberately over-wide viewport and read back what lands
// outside the box. Sweeping every parameter to each of its extremes and then
// random combinations of them is an upper bound on any pose the model can hold;
// --motion-sweep additionally plays every motion the outfit has, in real time,
// for the number the side bleed should actually be sized from.
int runExtent(const Options& opt, GLuint tex) {
    live2d::LAppModel model;
    if (!model.loadModelJson(opt.model)) {
        fprintf(stderr, "ERROR: loadModelJson() failed\n");
        return 1;
    }
    const asuna::Framing framing =
        (opt.framing.empty() || opt.framing == "auto")
            ? asuna::framingFromModelJson(opt.model.c_str())
            : asuna::parseFraming(opt.framing.c_str());

    model.refreshGeometry();
    asuna::Fit fit;
    // A probe viewport far wider than anything is expected to need, so the
    // measurement is never itself clipped.
    constexpr float kProbeSide = 0.75f;
    if (!asuna::solveFit(model, framing, opt.height, opt.pad, opt.bleed, kProbeSide, opt.drop, &fit)) {
        fprintf(stderr, "ERROR: solveFit() found no geometry\n");
        return 1;
    }
    const int vw = fit.boxWidth + 2 * fit.sideBleed;
    const int vh = fit.boxHeight + fit.bleed;
    model.resize(vw, vh);
    model.setScale(fit.scale);
    model.setOffset(fit.offsetX, fit.offsetY);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::vector<unsigned char> px(static_cast<size_t>(vw) * vh * 4);
    double worstLeft = 0, worstRight = 0;

    // One rendered frame -> how far past each side of the box the alpha reaches,
    // counting only the rows inside the visible box. glReadPixels rows start at
    // the bottom of the viewport, which is `bleed` px below the screen edge.
    const auto sample = [&]() {
        glViewport(0, 0, vw, vh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        model.draw();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        int lo = vw, hi = -1;
        for (int y = fit.bleed; y < vh; ++y) {
            const unsigned char* row = px.data() + static_cast<size_t>(y) * vw * 4;
            for (int x = 0; x < lo; ++x)
                if (row[x * 4 + 3] > 32) { lo = x; break; }
            for (int x = vw - 1; x > hi; --x)
                if (row[x * 4 + 3] > 32) { hi = x; break; }
        }
        if (hi < 0) return;
        worstLeft = std::max(worstLeft, static_cast<double>(fit.sideBleed - lo));
        worstRight = std::max(worstRight,
                              static_cast<double>(hi - (fit.sideBleed + fit.boxWidth - 1)));
    };

    model.setAutoBreathEnable(false);
    model.setAutoBlinkEnable(false);
    sample();
    const double restLeft = worstLeft, restRight = worstRight;

    const int n = model.getParameterCount();
    std::vector<std::string> ids(n);
    std::vector<float> lo(n), hi(n), def(n);
    for (int i = 0; i < n; ++i) {
        ids[i] = model.getParameterId(i);
        lo[i] = model.getParameterMin(i);
        hi[i] = model.getParameterMax(i);
        def[i] = model.getParameterDefault(i);
    }

    for (int i = 0; i < n; ++i) {
        for (float v : {lo[i], hi[i]}) {
            model.setParameterValue(ids[i], v);
            model.refreshGeometry();
            sample();
        }
        model.setParameterValue(ids[i], def[i]);
    }
    const double singleLeft = worstLeft, singleRight = worstRight;

    // Interactions: a shoulder and an elbow at once reach further than either
    // alone, and no per-parameter sweep can see that.
    unsigned seed = 12345;
    const auto rnd = [&]() {
        seed = seed * 1103515245u + 12345u;
        return static_cast<double>((seed >> 16) & 0x7fff) / 32767.0;
    };
    for (int trial = 0; trial < 200; ++trial) {
        for (int i = 0; i < n; ++i)
            model.setParameterValue(ids[i], lo[i] + (hi[i] - lo[i]) * rnd());
        model.refreshGeometry();
        sample();
    }
    for (int i = 0; i < n; ++i) model.setParameterValue(ids[i], def[i]);
    const double combinedLeft = worstLeft, combinedRight = worstRight;

    // The parameter sweep bounds poses that *exist*; motions only visit some of
    // them, and that is the number worth sizing to. Real time, because the
    // runtime's motion manager reads the wall clock, so it is opt-in.
    double motionLeft = 0, motionRight = 0;
    std::string widest;
    if (opt.motionSweep) {
        model.setAutoBreathEnable(true);
        model.setAutoBlinkEnable(true);
        const asuna::Json root = asuna::Json::parseFile(opt.model);
        const asuna::Json& groups = root["motions"];
        for (size_t g = 0; g < groups.size(); ++g) {
            const asuna::Json& list = groups.valueAt(g);
            for (size_t i = 0; i < list.size(); ++i) {
                worstLeft = worstRight = 0;
                model.startMotion(groups.keyAt(g), static_cast<int>(i), 3);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(8);
                do {
                    model.update();
                    sample();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                } while (!model.isMotionFinished() &&
                         std::chrono::steady_clock::now() < deadline);
                const std::string name = list[i]["file"].asString();
                printf("      %-28s %4.0f / %-4.0f px  %4.0f%% of box\n", name.c_str(),
                       worstLeft, worstRight,
                       100.0 * std::max(worstLeft, worstRight) / fit.boxWidth);
                if (std::max(worstLeft, worstRight) > std::max(motionLeft, motionRight))
                    widest = name;
                motionLeft = std::max(motionLeft, worstLeft);
                motionRight = std::max(motionRight, worstRight);
            }
        }
    }

    printf("  %-28s %-4s box=%dx%d  overflow px L/R: rest %.0f/%.0f  single %.0f/%.0f"
           "  combined %.0f/%.0f (%.0f%%)",
           opt.model.c_str(), asuna::framingName(framing), fit.boxWidth, opt.height,
           restLeft, restRight, singleLeft, singleRight, combinedLeft, combinedRight,
           100.0 * std::max(combinedLeft, combinedRight) / fit.boxWidth);
    if (opt.motionSweep)
        printf("  motions %.0f/%.0f (%.0f%%, %s)", motionLeft, motionRight,
               100.0 * std::max(motionLeft, motionRight) / fit.boxWidth, widest.c_str());
    printf("\n");
    return 0;
}

// --- what is under the crop line --------------------------------------------
//
// The mirror of --extent, on the other axis and for a different reason. The side
// bleed is meant to be seen at rest; the downward bleed is only ever seen while
// she is lifted, and it can only reveal artwork that exists. A bust crop cuts her
// at the waist, where there is always more torso underneath. A full-body crop
// cuts her wherever measureTarget() lands - at her feet if the artwork ends
// inside the canvas, at the canvas edge if it does not - and only a rendered
// measurement can tell those two apart.
//
// So: solve the fit with an absurd bleed, render, and read back how far down the
// alpha still goes. A depth of ~0 means the crop is already the end of her and a
// bleed would reveal nothing; anything more is the artwork that a lift is
// currently throwing away.
int runBelow(const Options& opt, GLuint tex) {
    live2d::LAppModel model;
    if (!model.loadModelJson(opt.model)) {
        fprintf(stderr, "ERROR: loadModelJson() failed\n");
        return 1;
    }
    const asuna::Framing framing =
        (opt.framing.empty() || opt.framing == "auto")
            ? asuna::framingFromModelJson(opt.model.c_str())
            : asuna::parseFraming(opt.framing.c_str());

    model.refreshGeometry();
    asuna::Fit fit;
    // Far more bleed than any lift asks for, so the measurement is never itself
    // the thing doing the clipping.
    const int probe = opt.bleed > 0 ? opt.bleed : opt.height;
    if (!asuna::solveFit(model, framing, opt.height, opt.pad, probe, 0.0f, opt.drop, &fit)) {
        fprintf(stderr, "ERROR: solveFit() found no geometry\n");
        return 1;
    }
    const int vw = fit.boxWidth;
    const int vh = fit.boxHeight + fit.bleed;
    model.resize(vw, vh);
    model.setScale(fit.scale);
    model.setOffset(fit.offsetX, fit.offsetY);

    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, vw, vh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    std::vector<unsigned char> px(static_cast<size_t>(vw) * vh * 4);
    double deepest = 0;

    // glReadPixels row 0 is the bottom of the viewport, which sits `bleed` px
    // below the visible bottom edge; row `bleed` is the screen edge itself.
    const auto sample = [&]() {
        glViewport(0, 0, vw, vh);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        model.draw();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, vw, vh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

        for (int y = 0; y < fit.bleed; ++y) {
            const unsigned char* row = px.data() + static_cast<size_t>(y) * vw * 4;
            bool any = false;
            for (int x = 0; x < vw && !any; ++x) any = row[x * 4 + 3] > 32;
            if (any) {
                deepest = std::max(deepest, static_cast<double>(fit.bleed - y));
                break;
            }
        }
    };

    model.setAutoBreathEnable(false);
    model.setAutoBlinkEnable(false);
    sample();
    const double rest = deepest;
    // The rest frame, viewport and all, so what is under the crop can be looked
    // at rather than inferred from a pixel count. A number cannot tell a foot
    // from a ground shadow.
    if (!opt.out.empty()) writePam(opt.out, px, vw, vh);

    const int n = model.getParameterCount();
    std::vector<std::string> ids(n);
    std::vector<float> lo(n), hi(n), def(n);
    for (int i = 0; i < n; ++i) {
        ids[i] = model.getParameterId(i);
        lo[i] = model.getParameterMin(i);
        hi[i] = model.getParameterMax(i);
        def[i] = model.getParameterDefault(i);
    }
    for (int i = 0; i < n; ++i) {
        for (float v : {lo[i], hi[i]}) {
            model.setParameterValue(ids[i], v);
            model.refreshGeometry();
            sample();
        }
        model.setParameterValue(ids[i], def[i]);
    }
    model.refreshGeometry();
    const double moved = deepest;

    // The model-space side of the same question, which says *why*: a crop at the
    // canvas edge has artwork under it by definition, a crop above the canvas
    // edge is where the artwork itself ran out.
    float figure[4] = {0, 0, 0, 0};
    model.getFigureBounds(figure);
    float foot[4] = {0, 0, 0, 0};
    const bool haveFoot = model.getDrawableBounds("D_REF.PT_FOOT", foot);

    printf("  %-30s %-4s box=%dx%d  below edge: rest %.0f px  posed %.0f px (%.0f%% of box)"
           "   crop y=%.0f  canvas %.0f  figure %.0f%s",
           opt.model.c_str(), asuna::framingName(framing), fit.boxWidth, opt.height,
           rest, moved, 100.0 * moved / opt.height, fit.target[3],
           model.getCanvasHeight(), figure[3],
           fit.target[3] >= model.getCanvasHeight() - 0.5f ? "  [cropped by canvas]" : "");
    if (haveFoot) printf("  PT_FOOT %.0f..%.0f", foot[1], foot[3]);
    printf("\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string();
        };
        if (a == "--model") opt.model = resolveModel(next());
        else if (a == "--out") opt.out = next();
        else if (a == "--expression") opt.expression = next();
        else if (a == "--motion") opt.motion = next();
        else if (a == "--motion-name") opt.motionName = next();
        else if (a == "--frames") opt.frames = std::stoi(next());
        else if (a == "--scale") opt.scale = std::stof(next());
        else if (a == "--framing") opt.framing = next();
        else if (a == "--pad") opt.pad = std::stoi(next());
        else if (a == "--bleed") opt.bleed = std::stoi(next());
        else if (a == "--side") opt.side = std::stof(next());
        else if (a == "--switch") opt.switchTo = next();
        else if (a == "--lean") opt.lean = std::stof(next());
        else if (a == "--param") {
            const std::string s = next();
            const size_t eq = s.find('=');
            if (eq == std::string::npos) {
                fprintf(stderr, "--param wants ID=VALUE, got %s\n", s.c_str());
                return 2;
            }
            opt.params.emplace_back(s.substr(0, eq), std::stof(s.substr(eq + 1)));
        }
        else if (a == "--hit") opt.hit = true;
        else if (a == "--extent") opt.extent = true;
        else if (a == "--drop") opt.drop = std::stoi(next());
        else if (a == "--below") opt.below = true;
        else if (a == "--motion-sweep") { opt.extent = true; opt.motionSweep = true; }
        else if (a == "--window") opt.window = true;
        else if (a == "--size") {
            const std::string s = next();
            const size_t x = s.find('x');
            if (x != std::string::npos) {
                opt.width = std::stoi(s.substr(0, x));
                opt.height = std::stoi(s.substr(x + 1));
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 2;
        }
    }

    glfwSetErrorCallback(onGlfwError);
    if (!glfwInit()) {
        fprintf(stderr, "ERROR: glfwInit() failed\n");
        return 1;
    }

#if defined(LIVE2D_GLES)
    // Match the context the GTK GLArea gives the real app, so this harness
    // keeps exercising exactly the code path that ships.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    // Desktop GL needs a compatibility profile: those shaders are GLSL 1.20,
    // which a core profile refuses to compile.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
#endif
    glfwWindowHint(GLFW_VISIBLE, opt.window ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_ALPHA_BITS, 8);

    GLFWwindow* win = glfwCreateWindow(opt.width, opt.height,
                                       "asuna phase 0", nullptr, nullptr);
    if (!win) {
        fprintf(stderr, "ERROR: glfwCreateWindow() failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
#if !defined(LIVE2D_GLES)
    if (!gladLoadGL()) {   // GLES needs no loader: real exported symbols
        fprintf(stderr, "ERROR: gladLoadGL() failed\n");
        glfwTerminate();
        return 1;
    }
#endif
    printf("OpenGL %s\n  GLSL %s\n  renderer %s\n",
           glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION),
           glGetString(GL_RENDERER));

    // Offscreen target with alpha.
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "ERROR: offscreen framebuffer incomplete\n");
        glfwTerminate();
        return 1;
    }

    EnableLive2DLog(true);
    SetLive2DLogLevel(LV_INFO);

    if (opt.hit || opt.extent || opt.below) {
        if (opt.hit) printf("Hit test: %s\n", opt.model.c_str());
        const int rc = opt.hit    ? runHitTest(opt)
                       : opt.below ? runBelow(opt, tex)
                                   : runExtent(opt, tex);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glfwDestroyWindow(win);
        glfwTerminate();
        return rc;
    }

    int rc = 0;
    {
        live2d::LAppModel model;
        printf("Loading %s\n", opt.model.c_str());
        if (!model.loadModelJson(opt.model)) {
            fprintf(stderr, "ERROR: loadModelJson() failed\n");
            glfwTerminate();
            return 1;
        }

        printf("Model loaded.\n");
        printf("  canvas: %.0f x %.0f\n", model.getCanvasWidth(),
               model.getCanvasHeight());
        printf("  parameters: %d, parts: %d\n", model.getParameterCount(),
               model.getPartCount());

        // Hit areas declared by our index.json.
        for (const char* area : {"head", "chest", "body", "foot"}) {
            const bool centre = model.hitTest(area, 0.0f, 0.0f);
            printf("  hit area '%s': present (centre hit=%s)\n", area,
                   centre ? "yes" : "no");
        }

        if (opt.framing.empty()) {
            model.resize(opt.width, opt.height);
            model.setScale(opt.scale);
        } else {
            // Exercise exactly what the app does, so a framing regression shows
            // up here rather than only on the desktop.
            const asuna::Framing f = opt.framing == "auto"
                ? asuna::framingFromModelJson(opt.model.c_str())
                : asuna::parseFraming(opt.framing.c_str());
            model.refreshGeometry();
            asuna::Fit fit;
            if (!asuna::solveFit(model, f, opt.height, opt.pad, opt.bleed, opt.side, opt.drop, &fit)) {
                fprintf(stderr, "ERROR: solveFit() found no geometry\n");
                glfwTerminate();
                return 1;
            }
            // The solver picks the box width, so the offscreen target has to
            // be reallocated to match before anything is rendered into it.
            opt.width = fit.boxWidth + 2 * fit.sideBleed;
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            // The matrix manager is sized by the viewport, bleed included - the
            // same thing Pet::fit() does.
            model.resize(opt.width, fit.boxHeight + fit.bleed);
            model.setScale(fit.scale);
            model.setOffset(fit.offsetX, fit.offsetY);
            printf("  framing: %s box=%dx%d scale=%.3f target=(%.0f,%.0f)-(%.0f,%.0f)\n",
                   asuna::framingName(f), fit.boxWidth, fit.boxHeight, fit.scale,
                   fit.target[0], fit.target[1], fit.target[2], fit.target[3]);
        }
        if (!opt.expression.empty()) {
            printf("  expression: %s\n", opt.expression.c_str());
            model.setExpression(opt.expression);
        }
        if (opt.motionName.empty()) {
            printf("  motion group: '%s'\n", opt.motion.c_str());
            model.startRandomMotion(opt.motion, 3);
        } else {
            const asuna::Json root = asuna::Json::parseFile(opt.model);
            const asuna::Json& groups = root["motions"];
            bool found = false;
            for (size_t g = 0; g < groups.size() && !found; ++g) {
                const asuna::Json& list = groups.valueAt(g);
                for (size_t i = 0; i < list.size() && !found; ++i) {
                    if (list[i]["file"].asString().find(opt.motionName) == std::string::npos)
                        continue;
                    printf("  motion: %s (group '%s' index %zu)\n",
                           list[i]["file"].asString().c_str(), groups.keyAt(g).c_str(), i);
                    model.startMotion(groups.keyAt(g), static_cast<int>(i), 3);
                    found = true;
                }
            }
            if (!found) {
                fprintf(stderr, "ERROR: no motion matching '%s'\n", opt.motionName.c_str());
                glfwTerminate();
                return 1;
            }
        }
        // The widest frame seen so far, kept for the dump.
        std::vector<unsigned char> widest;
        int widestSpan = -1;

        static const char* kLeanTrace[] = {"PARAM_ANGLE_X", "PARAM_BODY_ANGLE_X",
                                           "PARAM_HAIR_FRONT", "PARAM_HAIR_SIDE",
                                           "PARAM_HAIR_BACK"};
        int leanIdx[5];
        if (opt.lean != 0.0f) {
            printf("  lean trace at %.2f\n", opt.lean);
            for (size_t i = 0; i < 5; ++i) {
                leanIdx[i] = findParam(model, kLeanTrace[i]);
                if (leanIdx[i] < 0) { printf("    %-18s ABSENT\n", kLeanTrace[i]); continue; }
                printf("    %-18s range %.1f..%.1f\n", kLeanTrace[i],
                       model.getParameterMin(leanIdx[i]), model.getParameterMax(leanIdx[i]));
            }
        }

        const int frames = opt.window ? 1 << 30 : opt.frames;
        int drawn = 0;
        for (int f = 0; f < frames; ++f) {
            if (opt.window && glfwWindowShouldClose(win)) break;
            // Exactly what Pet::setLean does.
            if (opt.lean != 0.0f) model.mDragMgr.set(opt.lean, 0.0f);
            model.update();
            for (const auto& [id, value] : opt.params) model.setParameterValue(id, value);
            if (opt.lean != 0.0f && f % 4 == 0) {
                printf("   f%-3d", f);
                for (size_t i = 0; i < 5; ++i)
                    printf(" %8.3f", leanIdx[i] < 0 ? 0.0f
                                                    : model.getParameterValue(leanIdx[i]));
                printf("\n");
            }
            glBindFramebuffer(GL_FRAMEBUFFER, opt.window ? 0 : fbo);
            // Mirrors Pet::draw(): the viewport hangs `bleed` px below the
            // target so the extra body is clipped away exactly as it is on
            // screen at rest.
            glViewport(0, -opt.bleed, opt.width, opt.height + opt.bleed);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // fully transparent
            glClear(GL_COLOR_BUFFER_BIT);
            model.draw();
            ++drawn;
            if (!opt.motionName.empty() && !opt.window) {
                std::vector<unsigned char> rgba(
                    static_cast<size_t>(opt.width) * (opt.height) * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE,
                             rgba.data());
                int lo = opt.width, hi = -1;
                for (int y = 0; y < opt.height; ++y) {
                    const unsigned char* row =
                        rgba.data() + static_cast<size_t>(y) * opt.width * 4;
                    for (int x = 0; x < lo; ++x)
                        if (row[x * 4 + 3] > 32) { lo = x; break; }
                    for (int x = opt.width - 1; x > hi; --x)
                        if (row[x * 4 + 3] > 32) { hi = x; break; }
                }
                if (hi - lo > widestSpan) {
                    widestSpan = hi - lo;
                    widest = std::move(rgba);
                    printf("   f%-3d widest so far: alpha spans x %d..%d of %d\n",
                           f, lo, hi, opt.width);
                }
            }
            if (opt.window) {
                glfwSwapBuffers(win);
                glfwPollEvents();
            }
            // Let wall-clock-driven motion and physics actually advance.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        // The framing solver runs off these; print them so a new outfit can be
        // eyeballed without instrumenting the app. Must come after at least one
        // update+draw, or the deformed vertex arrays are still zero.
        float fb[4];
        if (model.getFigureBounds(fb))
            printf("  figure bounds: l=%.0f t=%.0f r=%.0f b=%.0f\n", fb[0], fb[1], fb[2], fb[3]);
        for (const char* d : {"D_REF.PT_HEAD", "D_REF.PT_CHEST", "D_REF.PT_BODY", "D_REF.PT_FOOT"}) {
            float b[4];
            if (model.getDrawableBounds(d, b))
                printf("  %-14s l=%.0f t=%.0f r=%.0f b=%.0f\n", d, b[0], b[1], b[2], b[3]);
        }

        printf("  frames drawn: %d\n", drawn);

        if (!opt.window) {
            // The clipping manager binds its own FBOs; rebind before reading.
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            std::vector<unsigned char> rgba(
                static_cast<size_t>(opt.width) * opt.height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, opt.width, opt.height, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba.data());
            const GLenum err = glGetError();
            if (err != GL_NO_ERROR)
                fprintf(stderr, "WARNING: glGetError() = 0x%x\n", err);
            if (!widest.empty()) rgba = std::move(widest);
            reportPixels(rgba, opt.width, opt.height);
            if (!writePam(opt.out, rgba, opt.width, opt.height)) rc = 1;
            else printf("  wrote %s\n", opt.out.c_str());
        }

        // Costume switching: load each outfit into the same GL context the way
        // Pet::load does - build the replacement while the old one is still
        // alive, then release. Coverage per outfit is the check that the new
        // textures really bound and nothing was left over from the last one.
        if (!opt.switchTo.empty()) {
            std::unique_ptr<live2d::LAppModel> current;
            size_t start = 0;
            while (start <= opt.switchTo.size()) {
                const size_t comma = opt.switchTo.find(',', start);
                const std::string id = opt.switchTo.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                start = (comma == std::string::npos) ? opt.switchTo.size() + 1 : comma + 1;
                if (id.empty()) continue;

                const std::string path = "models/asuna_" + id + "/index.json";
                auto next = std::make_unique<live2d::LAppModel>();
                if (!next->loadModelJson(path)) {
                    fprintf(stderr, "ERROR: switch to %s failed\n", path.c_str());
                    rc = 1;
                    continue;
                }
                current = std::move(next);

                const asuna::Framing f = asuna::framingFromModelJson(path.c_str());
                current->refreshGeometry();
                asuna::Fit fit;
                if (!asuna::solveFit(*current, f, opt.height, opt.pad, opt.bleed, opt.side, opt.drop, &fit)) {
                    fprintf(stderr, "ERROR: switch to %s has no geometry\n", path.c_str());
                    rc = 1;
                    continue;
                }
                current->resize(fit.boxWidth, fit.boxHeight + fit.bleed);
                current->setScale(fit.scale);
                current->setOffset(fit.offsetX, fit.offsetY);
                current->startRandomMotion(opt.motion, 3);

                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fit.boxWidth, fit.boxHeight,
                             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                glBindTexture(GL_TEXTURE_2D, 0);

                for (int f2 = 0; f2 < 3; ++f2) {
                    current->update();
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    glViewport(0, -fit.bleed, fit.boxWidth, fit.boxHeight + fit.bleed);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    current->draw();
                }

                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                std::vector<unsigned char> rgba(
                    static_cast<size_t>(fit.boxWidth) * fit.boxHeight * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fit.boxWidth, fit.boxHeight, GL_RGBA,
                             GL_UNSIGNED_BYTE, rgba.data());
                const GLenum serr = glGetError();
                if (serr != GL_NO_ERROR) {
                    fprintf(stderr, "ERROR: glGetError() = 0x%x after switch to %s\n",
                            serr, id.c_str());
                    rc = 1;
                }
                printf("  switched to asuna_%s (%s, %dx%d)\n", id.c_str(),
                       asuna::framingName(f), fit.boxWidth, fit.boxHeight);
                reportPixels(rgba, fit.boxWidth, fit.boxHeight);
            }
        }
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}
