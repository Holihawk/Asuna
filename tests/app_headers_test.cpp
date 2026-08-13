#include "app/argparse.hpp"
#include "app/bubble_timing.hpp"
#include "app/cli.hpp"
#include "app/config.hpp"
#include "app/daemon.hpp"
#include "app/extension_options.hpp"
#include "app/options.hpp"
#include "app/state.hpp"

static_assert(sizeof(asuna::ShellOptions) > 0);
