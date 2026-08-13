#include "app/cli/internal.hpp"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "app/argparse.hpp"
#include "app/ipc.hpp"

namespace asuna {
namespace cli {

int dispatch(int argc, char** argv, RunShell runShell) {
    // --json anywhere, since it says how to print rather than what to do.
    std::vector<char*> args;
    args.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) gJson = true;
        else args.push_back(argv[i]);
    }
    argc = static_cast<int>(args.size());
    argv = args.data();

    // No subcommand, or a leading flag: the bare form, which runs her here.
    // Kept because it is what every phase before this one did, and because
    // `asuna --model 31 --no-persist` is still the fastest way to try something.
    if (argc < 2 || argv[1][0] == '-') {
        ShellOptions opt;
        if (const int bad = applyConfig(&opt); bad != kOk) return bad;
        const int parsed = parseOptions(argc, argv, 1, &opt, nullptr);
        if (parsed != kOk) return parsed < 0 ? kOk : parsed;
        if (const int busy = refuseIfRunning(opt); busy != kOk) return busy;
        return runShell(std::move(opt));
    }

    const std::string cmd = argv[1];
    const int rest = 2;

    if (cmd == "help") { usage(); return kOk; }
    if (cmd == "start") return cmdStart(argc, argv, rest, runShell);
    if (cmd == "exit" || cmd == "stop" || cmd == "quit") return cmdExit();
    if (cmd == "restart") {
        // Not an error if she was not running: restart means "be running".
        const int left = cmdExit();
        if (left != kOk && left != kNotRunning) return left;
        return cmdStart(argc, argv, rest, runShell);
    }
    if (cmd == "status") return cmdStatus();
    if (cmd == "ping") {
        int status = kError;
        Json d;
        if (!send("ping", "", &d, &status)) return status;
        if (!gJson) printf("asuna: running (pid %d)\n", static_cast<int>(d["pid"].asNumber()));
        return kOk;
    }
    if (cmd == "hide" || cmd == "show" || cmd == "toggle") return simple(cmd);
    if (cmd == "say") {
        ipc::Out args;
        // The four streaming flags may come with no text at all - `--release`
        // and `--clear` are about the line that is already up - so the text is
        // whatever is not a flag rather than a required first argument.
        bool haveText = false;
        for (int i = rest; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--for") {
                // Its own branch rather than `--for && i + 1 < argc`, which fell
                // through to the catch-all below and reported a missing value as
                // "say does not take '--for'".
                if (i + 1 >= argc) return complain("say --for needs a value", kUsage);
                double seconds = 0;
                std::string problem;
                // Strictly positive, and with no floor of its own: the daemon
                // reads 0 as "no timer at all", which is what leaving --for off
                // already means, but anything above it is a duration this end
                // has no business second-guessing.
                if (!argparse::realAbove("say --for", argv[++i], 0.0, &seconds, &problem))
                    return complain(problem, kUsage);
                args.num("seconds", seconds);
            }
            else if (a == "--hold") args.boolean("hold", true);
            else if (a == "--append") args.boolean("append", true);
            else if (a == "--release") args.boolean("release", true);
            else if (a == "--clear") args.boolean("clear", true);
            else if (a.compare(0, 2, "--") == 0)
                return complain("say does not take '" + a + "'", kUsage);
            else if (!haveText) {
                args.str("text", a);
                haveText = true;
            } else {
                return complain("say takes one line of text", kUsage);
            }
        }
        if (args.empty()) return complain("say needs some text", kUsage);
        return simple("say", args.done());
    }
    if (cmd == "think") {
        const std::string state = rest < argc ? argv[rest] : "on";
        if (state != "on" && state != "off") return complain("think takes on or off", kUsage);
        return simple("think", ipc::Out().boolean("on", state == "on").done());
    }
    if (cmd == "chat")
        return simple("chat", ipc::Out().str("text", rest < argc ? argv[rest] : "").done());
    if (cmd == "ext") return cmdExt(argc, argv, rest);
    if (cmd == "subscribe") return cmdSubscribe();
    if (cmd == "motion" || cmd == "expression") return cmdNamed(cmd, argc, argv, rest);
    if (cmd == "model") return cmdModel(argc, argv, rest);
    if (cmd == "move") {
        if (rest >= argc) return complain("move needs a position", kUsage);
        const std::string where = argv[rest];
        // A number is pixels; anything else is one of the three anchors, and
        // the daemon is the one that knows how wide the screen is. Something
        // made only of digits and dots but not actually a number - "1.2.3", or
        // a bare "." - is neither, and used to arrive as a partially parsed
        // 1.2 or a silent 0. No range: the daemon clamps x to the screen it
        // can see, and only it knows how wide that is.
        const bool numeric = where.find_first_not_of("-+0123456789.eE") == std::string::npos;
        if (numeric) {
            double x = 0;
            std::string problem;
            if (!argparse::realAny("move", where, &x, &problem)) return complain(problem, kUsage);
            return simple("move", ipc::Out().num("x", x).done());
        }
        return simple("move", ipc::Out().str("where", where).done());
    }
    if (cmd == "scale") {
        if (rest >= argc) return complain("scale needs a number", kUsage);
        double value = 0;
        std::string problem;
        // Also unranged, and for the same reason: setUserScale clamps to
        // 0.5-2.5 *and* to what the screen has room for, which is often the
        // tighter of the two. Refusing 3.0 here would refuse something that
        // currently works and lands at the ceiling.
        if (!argparse::realAny("scale", argv[rest], &value, &problem))
            return complain(problem, kUsage);
        return simple("scale", ipc::Out().num("value", value).done());
    }
    if (cmd == "layer") {
        if (rest >= argc) return complain("layer needs a name", kUsage);
        return simple("layer", ipc::Out().str("name", argv[rest]).done());
    }
    if (cmd == "menu")
        return simple("menu", ipc::Out().str("action", rest < argc ? argv[rest] : "toggle").done());
    if (cmd == "output") return cmdOutput(argc, argv, rest);
    if (cmd == "config") return cmdConfig(argc, argv, rest);
    if (cmd == "autostart") return cmdAutostart(argc, argv, rest);

    fprintf(stderr, "asuna: unknown command '%s'\n", cmd.c_str());
    usage();
    return kUsage;
}

}  // namespace cli
}  // namespace asuna
