#include "paths.hpp"

#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace asuna {
namespace paths {
namespace {

fs::path envDir(const char* name) {
    const char* v = getenv(name);
    return (v && *v) ? fs::path(v) : fs::path();
}

fs::path home() {
    const char* h = getenv("HOME");
    return fs::path(h && *h ? h : ".");
}

std::string ensure(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

}  // namespace

std::string runtimeDir() {
    fs::path base = envDir("XDG_RUNTIME_DIR");
    if (base.empty()) {
        // No runtime dir means no per-session tmpfs to put a socket in. /tmp
        // with the uid in the name is the conventional stand-in; it is not as
        // safe as the runtime dir (which is 0700 and per-session), so the
        // directory is created with the same permissions explicitly below.
        base = fs::path("/tmp") / ("asuna-" + std::to_string(getuid()));
        const std::string dir = ensure(base);
        std::error_code ec;
        fs::permissions(base, fs::perms::owner_all, ec);
        return dir;
    }
    return ensure(base / "asuna");
}

std::string stateDir() {
    const fs::path base = envDir("XDG_STATE_HOME");
    return (base.empty() ? home() / ".local" / "state" / "asuna" : base / "asuna").string();
}

std::string configDir() {
    const fs::path base = envDir("XDG_CONFIG_HOME");
    return (base.empty() ? home() / ".config" / "asuna" : base / "asuna").string();
}

std::string socketPath() { return (fs::path(runtimeDir()) / "control.sock").string(); }
std::string lockPath() { return (fs::path(runtimeDir()) / "asuna.lock").string(); }
std::string logPath() { return (fs::path(stateDir()) / "asuna.log").string(); }

std::string extPidPath() { return (fs::path(runtimeDir()) / "ext.pid").string(); }
std::string extLogPath() { return (fs::path(stateDir()) / "ext.log").string(); }

std::string exeDir() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : exe.parent_path().string();
}

std::string modelsRoot() {
    // Cached: this is called from the argument parser, from the outfit registry
    // and once per outfit switch, and the answer cannot change while we run.
    static const std::string root = [] {
        std::vector<fs::path> candidates;
        if (const char* dir = getenv("ASUNA_MODEL_DIR"); dir && *dir)
            candidates.emplace_back(dir);
        // The current directory first, so `./build/asuna` from the source tree
        // keeps resolving exactly what it resolved before this function existed.
        candidates.emplace_back("models");
        if (const std::string exe = exeDir(); !exe.empty()) {
            const fs::path dir(exe);
            candidates.push_back(dir / "models");
            candidates.push_back(dir.parent_path() / "models");
            candidates.push_back(dir.parent_path() / "share" / "asuna" / "models");
        }
        const fs::path data = envDir("XDG_DATA_HOME");
        candidates.push_back((data.empty() ? home() / ".local" / "share" : data) /
                             "asuna" / "models");

        std::error_code ec;
        for (const auto& p : candidates)
            if (fs::is_directory(p, ec)) return p.string();
        return std::string("models");
    }();
    return root;
}

std::string extHelper() {
    static const std::string script = [] {
        std::vector<fs::path> candidates;
        if (const char* p = getenv("ASUNA_EXT"); p && *p) candidates.emplace_back(p);
        // The source tree first, so a helper being edited is the one that runs.
        candidates.emplace_back("tools/asuna-ext.py");
        if (const std::string exe = exeDir(); !exe.empty()) {
            const fs::path dir(exe);
            candidates.push_back(dir / "asuna-ext.py");
            candidates.push_back(dir.parent_path() / "share" / "asuna" / "asuna-ext.py");
        }
        const fs::path data = envDir("XDG_DATA_HOME");
        candidates.push_back((data.empty() ? home() / ".local" / "share" : data) / "asuna" /
                             "asuna-ext.py");

        std::error_code ec;
        for (const auto& p : candidates)
            if (fs::is_regular_file(p, ec)) return fs::absolute(p, ec).string();
        return std::string();
    }();
    return script;
}

}  // namespace paths
}  // namespace asuna
