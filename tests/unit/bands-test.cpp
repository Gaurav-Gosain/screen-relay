// changedBands must cover every changed pixel and report nothing for an unchanged band.
#include "core/bands.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

using relay::DamageRect;

static bool covered(const std::vector<DamageRect>& rects, int x, int y) {
  for (const auto& r : rects)
    if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) return true;
  return false;
}

int main() {
  std::mt19937 random(7);
  for (const auto size : {std::pair{1920, 1080}, std::pair{333, 250}, std::pair{64, 64}, std::pair{1000, 65}}) {
    const int w = size.first, h = size.second, stride = w * 4 + 32;  // a stride wider than the row
    std::vector<uint8_t> before(size_t(stride) * h), now;
    for (auto& b : before) b = uint8_t(random());
    for (int round = 0; round < 200; ++round) {
      now = before;
      const int changes = round % 7;  // round 0, 7, ...: no change at all
      for (int i = 0; i < changes; ++i) {
        const int x = int(random() % w), y = int(random() % h), byte = int(random() % 4);
        now[size_t(y) * stride + size_t(x) * 4 + byte] ^= uint8_t(1 + random() % 255);
      }
      // Bytes past the row end are padding and must not count.
      now[size_t(stride) - 1] ^= 0xff;
      std::vector<DamageRect> rects;
      relay::changedBands(now.data(), before.data(), w, h, stride, rects);
      if (!changes) assert(rects.empty());
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          for (int byte = 0; byte < 4; ++byte)
            if (now[size_t(y) * stride + size_t(x) * 4 + byte] != before[size_t(y) * stride + size_t(x) * 4 + byte])
              assert(covered(rects, x, y));
      for (const auto& r : rects) {
        assert(r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 && r.x + r.width <= w && r.y + r.height <= h);
        assert(r.y % relay::band_rows == 0 && r.x % relay::band_columns == 0);
      }
    }
  }
  // A one-pixel change shows within 8 phases, and in the phase of its row.
  {
    const int w = 200, h = 100, stride = w * 4;
    std::vector<uint8_t> a(size_t(stride) * h, 1), b = a;
    b[size_t(37) * stride + 10 * 4] = 9;
    int found = 0;
    for (int phase = 0; phase < 8; ++phase)
      if (relay::sampledRowsDiffer(b.data(), a.data(), w, h, stride, phase, 8)) { ++found; assert(phase == 37 % 8); }
    assert(found == 1);
    assert(!relay::sampledRowsDiffer(a.data(), a.data(), w, h, stride, 3, 8));
  }
  std::printf("bands: every changed pixel covered, no box for no change, padding ignored, sampled rows find a change\n");
}
