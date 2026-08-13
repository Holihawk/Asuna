#pragma once

#include <string>

#include "app/cli.hpp"
#include "json.hpp"

namespace asuna {
namespace cli {

// Private coordination surface for the CLI implementation units. Keep only
// declarations and constants that genuinely have more than one owner here;
// the public command boundary remains app/cli.hpp.
inline constexpr int kTermTimeoutMs = 5000;
inline constexpr int kKillTimeoutMs = 2000;

extern bool gJson;

int nowMs();
int complain(const std::string& message, int status = kError);
std::string firstLine(const std::string& text);
void usage();

bool send(const std::string& cmd, const std::string& args, Json* data, int* status,
          bool quietWhenAbsent = false);
int simple(const std::string& cmd, const std::string& args = "");

int applyConfig(ShellOptions* opt);
int parseOptions(int argc, char** argv, int from, ShellOptions* opt, bool* foreground);
int refuseIfRunning(const ShellOptions& opt);
int cmdStart(int argc, char** argv, int from, RunShell runShell);
int cmdExit();
int cmdStatus();
int cmdModel(int argc, char** argv, int from);
int cmdNamed(const std::string& cmd, int argc, char** argv, int from);
int cmdOutput(int argc, char** argv, int from);
int cmdConfig(int argc, char** argv, int from);
int cmdExt(int argc, char** argv, int from);
int cmdSubscribe();
int cmdAutostart(int argc, char** argv, int from);

}  // namespace cli
}  // namespace asuna
