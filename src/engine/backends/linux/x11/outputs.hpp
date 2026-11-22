#pragma once
#include "core/output.hpp"
#include <X11/Xlib.h>
#include <optional>
#include <string>
#include <vector>

namespace relay::x11 {
/// The RandR monitors of the default screen, in root window coordinates.
std::vector<OutputDesc> monitors(Display* display);
/// The monitor with this RandR name, if there is one.
std::optional<OutputDesc> findMonitor(Display* display, const std::string& name);
std::vector<OutputDesc> listOutputs();
}  // namespace relay::x11
