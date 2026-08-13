#include "app/cli/internal.hpp"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "app/ipc.hpp"

namespace fs = std::filesystem;

namespace asuna {
namespace cli {

// --- the config file --------------------------------------------------------

int cmdConfig(int argc, char** argv, int from) {
    const std::string verb = from < argc ? argv[from] : "path";
    const std::string path = Config::path();
    std::error_code ec;

    if (verb == "path") {
        printf("%s%s\n", path.c_str(), fs::exists(path, ec) ? "" : "   (does not exist)");
        return kOk;
    }
    if (verb == "init") {
        const bool force = from + 1 < argc && strcmp(argv[from + 1], "--force") == 0;
        if (fs::exists(path, ec) && !force)
            return complain(path + " already exists (--force overwrites it)");
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        if (!f) return complain("could not write " + path);
        f << Config::defaultText();
        if (!f) return complain("could not write " + path);
        f.close();
        // 0600, because this file can now legitimately hold an API key -
        // `[ext.provider.<name>] api_key`. It did not need protecting when the
        // worst it held was how often she blinks.
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        printf("asuna: wrote %s - every setting at its default, so it changes nothing\n",
               path.c_str());
        return kOk;
    }
    if (verb == "show") {
        std::ifstream f(path);
        if (!f) {
            printf("# no config file at %s; these are the defaults\n%s", path.c_str(),
                   Config::defaultText().c_str());
            return kOk;
        }
        printf("%s", std::string(std::istreambuf_iterator<char>(f), {}).c_str());
        return kOk;
    }
    if (verb == "check") {
        Config cfg;
        cfg.load(path);

        // Everything this verb has to say that is not fatal, worked out once
        // and then printed in whichever form was asked for. Two branches each
        // computing their own diagnostics is exactly how the JSON form came to
        // be missing the one below about file permissions, and how the two
        // forms came to disagree about a security-relevant fact.
        //
        // A superset of Config::warnings, which is only ever about the file's
        // *contents*. This is `config check`'s whole answer, and the mode of
        // the file is part of it.
        std::vector<std::string> warnings = cfg.warnings;
        bool literalKey = false;
        for (const auto& p : cfg.ext.providers) literalKey |= !p.apiKey.empty();
        if (literalKey && !cfg.source.empty()) {
            // Worth saying whether or not the file also has a problem in it -
            // and it used to be skipped entirely when it did. A key in a
            // world-readable file is a key anyone with an account on the
            // machine can read, and a typo on line 12 does not make that
            // wait its turn.
            const fs::perms mode = fs::status(path, ec).permissions();
            if ((mode & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none)
                warnings.push_back("it holds an API key and is readable by others -"
                                   " `chmod 600 " + path + "`");
        }

        // The only verdict here that is not the daemon's, so the only one that
        // has to build its own reply line. Same envelope as every other --json
        // answer, because `asuna --json <anything> | jq .ok` should mean one
        // thing: `ok` is false exactly when the exit status is non-zero, and
        // `warnings` is always present, empty or not, so a reader never has to
        // tell "no warnings" from "this build had none to give".
        if (gJson) {
            ipc::Out d;
            d.str("path", path);
            d.boolean("exists", !cfg.source.empty());
            d.raw("problems", ipc::Out::array(cfg.problems));
            d.raw("warnings", ipc::Out::array(warnings));
            const std::string body = d.done();
            if (cfg.problems.empty()) {
                printf("%s\n", ipc::ok(body).c_str());
                return kOk;
            }
            printf("%s\n", ipc::fail(std::to_string(cfg.problems.size()) +
                                     " problem(s) in " + path, body).c_str());
            return kError;
        }
        if (cfg.source.empty()) {
            printf("asuna: no config file at %s - all defaults, which is valid\n",
                   path.c_str());
            return kOk;
        }
        if (cfg.problems.empty()) {
            // "fine" only when it is. A file whose max_height does nothing is
            // not broken - it starts, and it behaves the way the warning says -
            // but telling somebody it is fine is how they stop looking.
            if (warnings.empty()) {
                printf("asuna: %s is fine\n", path.c_str());
            } else {
                printf("asuna: %s parses, with %zu thing(s) worth knowing\n",
                       path.c_str(), warnings.size());
                for (const std::string& w : warnings) printf("  %s\n", w.c_str());
            }
            return kOk;
        }
        fprintf(stderr, "asuna: %s\n", path.c_str());
        for (const std::string& p : cfg.problems) fprintf(stderr, "  %s\n", p.c_str());
        for (const std::string& w : warnings) fprintf(stderr, "  %s\n", w.c_str());
        return kError;
    }
    if (verb == "edit") {
        const char* editor = getenv("EDITOR");
        if (!editor || !*editor) editor = getenv("VISUAL");
        if (!editor || !*editor) return complain("$EDITOR is not set");
        if (!fs::exists(path, ec)) {
            fs::create_directories(fs::path(path).parent_path(), ec);
            std::ofstream f(path, std::ios::trunc);
            if (f) f << Config::defaultText();
        }
        // execlp, not system(): no shell means no quoting question about a path
        // with a space in it, and the editor inherits this terminal as it is.
        execlp(editor, editor, path.c_str(), nullptr);
        return complain(std::string("could not run ") + editor + ": " + strerror(errno));
    }
    if (verb == "reload") {
        int status = kError;
        Json d;
        if (!send("config", "", &d, &status)) {
            // The daemon sent the problem list along with the refusal, so the
            // reason is already here rather than a `config check` away. The
            // warnings come with it for the same reason - both were known at
            // the same moment, and one edit should be able to fix both.
            const Json& problems = d["problems"];
            for (size_t i = 0; i < problems.size(); ++i)
                fprintf(stderr, "  %s\n", problems[i].asString().c_str());
            const Json& warnings = d["warnings"];
            for (size_t i = 0; i < warnings.size(); ++i)
                fprintf(stderr, "  %s\n", warnings[i].asString().c_str());
            return status;
        }
        if (!gJson) {
            const std::string where = d["path"].asString();
            printf("asuna: reloaded %s\n", where.c_str());
            printf("       outfit, size, position, layer and output are not touched -\n"
                   "       those are state.json's, and it outranks the config file\n");
            const std::string note = d["note"].asString();
            if (!note.empty()) printf("       note: %s\n", note.c_str());
            // It reloaded, and some of what was reloaded does nothing. On
            // stderr rather than with the success above, because it is the one
            // part of this output somebody might want to grep for.
            const Json& warnings = d["warnings"];
            for (size_t i = 0; i < warnings.size(); ++i)
                fprintf(stderr, "  %s\n", warnings[i].asString().c_str());
        }
        return kOk;
    }
    return complain("config takes path, show, init, check, edit or reload", kUsage);
}

}  // namespace cli
}  // namespace asuna
