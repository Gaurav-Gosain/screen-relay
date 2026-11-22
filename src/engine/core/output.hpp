#pragma once
#include <string>
#include <vector>

namespace relay {
/// One display a backend can capture, for /capabilities and /stream?output=NAME.
struct OutputDesc {
  std::string name;
  /// Position in the desktop layout, in logical pixels.
  int x = 0, y = 0;
  /// Capture size in physical pixels.
  int width = 0, height = 0;
  /// Physical pixels for each logical pixel.
  double scale = 1;
  /// The output a /stream without output=NAME captures.
  bool primary = false;
};
/// Output names in a /stream query: 1 to 63 of A-Z a-z 0-9 . _ - :
inline bool validOutputName(const std::string& name) {
  if (name.empty() || name.size() > 63) return false;
  for (const char c : name)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':'))
      return false;
  return true;
}
}  // namespace relay
