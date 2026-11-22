#include "bands.hpp"
#include <algorithm>
#include <cstring>

namespace relay {
void changedBands(const uint8_t* now, const uint8_t* before, int width, int height, int stride,
                  std::vector<DamageRect>& out) {
  const size_t chunk = size_t(band_columns) * 4, row = size_t(width) * 4;
  const int columns = (width + band_columns - 1) / band_columns;
  const auto differs = [&](const uint8_t* a, const uint8_t* b, int column) {
    const size_t at = size_t(column) * chunk;
    return std::memcmp(a + at, b + at, std::min(chunk, row - at)) != 0;
  };
  for (int y0 = 0; y0 < height; y0 += band_rows) {
    const int y1 = std::min(height, y0 + band_rows);
    int first = columns, last = -1;  // changed columns of this band so far
    for (int y = y0; y < y1 && !(first == 0 && last == columns - 1); ++y) {
      const uint8_t* a = now + size_t(y) * size_t(stride);
      const uint8_t* b = before + size_t(y) * size_t(stride);
      // Only the parts outside the box found so far can widen it.
      const size_t left = first == columns ? row : size_t(first) * chunk;
      if (left && std::memcmp(a, b, left) != 0) {
        int c = 0;
        while (!differs(a, b, c)) ++c;
        first = std::min(first, c);
        last = std::max(last, c);
      }
      if (last < 0) continue;
      const size_t right = size_t(last + 1) * chunk;
      if (right < row && std::memcmp(a + right, b + right, row - right) != 0) {
        int c = columns - 1;
        while (!differs(a, b, c)) --c;
        last = std::max(last, c);
      }
    }
    if (last < 0) continue;
    const int x0 = first * band_columns, x1 = std::min(width, (last + 1) * band_columns);
    out.push_back({x0, y0, x1 - x0, y1 - y0});
  }
}
bool sampledRowsDiffer(const uint8_t* now, const uint8_t* before, int width, int height, int stride, int phase, int step) {
  const size_t row = size_t(width) * 4;
  for (int y = phase; y < height; y += step)
    if (std::memcmp(now + size_t(y) * size_t(stride), before + size_t(y) * size_t(stride), row) != 0) return true;
  return false;
}
}  // namespace relay
