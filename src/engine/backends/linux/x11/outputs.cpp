#include "outputs.hpp"
#include <X11/extensions/Xrandr.h>
#include <memory>
#include <stdexcept>

namespace relay::x11 {
std::vector<OutputDesc> monitors(Display* display) {
  std::vector<OutputDesc> result;
  const int screen = DefaultScreen(display);
  int count = 0;
  std::unique_ptr<XRRMonitorInfo, decltype(&XRRFreeMonitors)> list(
    XRRGetMonitors(display, RootWindow(display, screen), True, &count), &XRRFreeMonitors);
  if (!list) return result;
  const int rootWidth = DisplayWidth(display, screen), rootHeight = DisplayHeight(display, screen);
  for (int i = 0; i < count; ++i) {
    const auto& monitor = list.get()[i];
    char* name = XGetAtomName(display, monitor.name);
    OutputDesc desc;
    desc.name = name ? name : "";
    if (name) XFree(name);
    desc.x = monitor.x; desc.y = monitor.y; desc.width = monitor.width; desc.height = monitor.height;
    // A stream without output=NAME captures the whole screen, which is one monitor only when it covers it.
    desc.primary = monitor.x == 0 && monitor.y == 0 && monitor.width == rootWidth && monitor.height == rootHeight;
    result.push_back(std::move(desc));
  }
  return result;
}

std::optional<OutputDesc> findMonitor(Display* display, const std::string& name) {
  for (auto& monitor : monitors(display))
    if (monitor.name == name) return monitor;
  return std::nullopt;
}

std::vector<OutputDesc> listOutputs() {
  Display* display = XOpenDisplay(nullptr);
  if (!display) throw std::runtime_error("opening X display failed");
  auto result = monitors(display);
  XCloseDisplay(display);
  return result;
}
}  // namespace relay::x11
