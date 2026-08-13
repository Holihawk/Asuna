#pragma once

#include <gtk4-layer-shell.h>

#include <string>

namespace asuna {

inline bool knownLayer(const std::string& name) {
    return name == "top" || name == "bottom" || name == "overlay" || name == "background";
}

inline GtkLayerShellLayer parseLayer(const std::string& name) {
    if (name == "bottom") return GTK_LAYER_SHELL_LAYER_BOTTOM;
    if (name == "overlay") return GTK_LAYER_SHELL_LAYER_OVERLAY;
    if (name == "background") return GTK_LAYER_SHELL_LAYER_BACKGROUND;
    return GTK_LAYER_SHELL_LAYER_TOP;
}

}  // namespace asuna
