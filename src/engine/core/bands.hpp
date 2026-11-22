#pragma once
#include "capture-source.hpp"
#include <cstdint>
#include <vector>

namespace relay {
/// Rows and columns of one compare step in changedBands.
constexpr int band_rows = 64, band_columns = 64;
/// Appends the parts of now that differ from before: one box per band of 64 rows, from the first to the
/// last changed 64-pixel column. Both images are 4 bytes a pixel with the same size and stride. Each
/// row is compared once, so an unchanged band costs one memcmp a row. For sources whose damage is
/// the whole output on every frame (Hyprland).
void changedBands(const uint8_t* now, const uint8_t* before, int width, int height, int stride,
                  std::vector<DamageRect>& out);
/// True when a row y with y % step == phase differs between now and before. Comparing against the
/// last sent frame with a new phase each time finds any change within step calls, and a change of
/// step rows or more at once.
bool sampledRowsDiffer(const uint8_t* now, const uint8_t* before, int width, int height, int stride, int phase, int step);
}  // namespace relay
