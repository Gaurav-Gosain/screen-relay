// Measures the engine work for a terminal that scrolls, when the compositor reports the whole
// output as damage on every frame (Hyprland): changedBands, then the tiles encoder.
// Usage: scroll-bench [frames] [lines per frame]. Prints ms per frame and the CPU share at 60 fps.
#include "core/bands.hpp"
#include "core/tiles.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using relay::DamageRect;
using relay::FrameView;
using relay::PixelFormat;
namespace tiles = relay::tiles;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int cell_w = 10, cell_h = 20;
struct Screen {
  int w, h, x0, x1;  // the terminal covers columns [x0, x1)
  std::vector<uint8_t> px;
  std::vector<std::vector<uint8_t>> glyphs;
  std::mt19937 random{3};
  Screen(int width, int height, int left, int right) : w(width), h(height), x0(left), x1(right), px(size_t(width) * height * 4) {
    for (size_t i = 0; i < px.size(); i += 4) { px[i] = 40; px[i + 1] = 30; px[i + 2] = 30; px[i + 3] = 255; }
    for (int g = 0; g < 96; ++g) {
      std::vector<uint8_t> bits(cell_w * cell_h);
      for (auto& b : bits) b = (random() % 5) == 0;
      glyphs.push_back(bits);
    }
    for (int y = 0; y + cell_h <= h; y += cell_h) line(y);
  }
  /// Writes one line of text at row y: random glyphs, then blank to the right edge.
  void line(int y) {
    const int used = int(random() % ((x1 - x0) / cell_w));
    for (int c = 0; c < (x1 - x0) / cell_w; ++c) {
      const auto& g = glyphs[random() % glyphs.size()];
      for (int dy = 0; dy < cell_h; ++dy)
        for (int dx = 0; dx < cell_w; ++dx) {
          uint8_t* p = &px[(size_t(y + dy) * w + x0 + c * cell_w + dx) * 4];
          const bool on = c < used && g[dy * cell_w + dx];
          p[0] = on ? 220 : 40; p[1] = on ? 220 : 30; p[2] = on ? 200 : 30;
        }
    }
  }
  /// Moves the terminal up by n lines and writes n new lines at the bottom.
  void scroll(int n) {
    const int rows = (h / cell_h) * cell_h, shift = n * cell_h;
    for (int y = 0; y + shift < rows; ++y)
      std::memmove(&px[(size_t(y) * w + x0) * 4], &px[(size_t(y + shift) * w + x0) * 4], size_t(x1 - x0) * 4);
    for (int k = n; k > 0; --k) line(rows - k * cell_h);
  }
};

void run(const char* name, int w, int h, int x0, int x1, int frames, int lines) {
  Screen screen(w, h, x0, x1);
  std::vector<uint8_t> before = screen.px;
  tiles::Encoder encoder(w, h);
  std::vector<uint8_t> out;
  encoder.update(FrameView{screen.px.data(), w * 4, w, h, PixelFormat::BGRA}, nullptr);
  encoder.encode(true, 0, out);
  double bands_ms = 0, still_ms = 0, encode_ms = 0;
  size_t bytes = 0, copies = 0;
  std::vector<DamageRect> rects;
  for (int f = 1; f <= frames; ++f) {
    // A still frame first: the compositor rendered, no pixel changed.
    auto t0 = Clock::now();
    rects.clear();
    relay::changedBands(screen.px.data(), before.data(), w, h, w * 4, rects);
    still_ms += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!rects.empty()) { std::fprintf(stderr, "still frame has damage\n"); std::exit(1); }
    screen.scroll(lines);
    t0 = Clock::now();
    rects.clear();
    relay::changedBands(screen.px.data(), before.data(), w, h, w * 4, rects);
    const auto t1 = Clock::now();
    encoder.update(FrameView{screen.px.data(), w * 4, w, h, PixelFormat::BGRA}, &rects);
    tiles::Stats stats;
    encoder.encode(false, uint64_t(f), out, &stats);
    const auto t2 = Clock::now();
    bands_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
    encode_ms += std::chrono::duration<double, std::milli>(t2 - t1).count();
    bytes += out.size(); copies += stats.copies;
    before = screen.px;
  }
  const double per = (bands_ms + encode_ms) / frames;
  std::printf("%-24s bands %.2f ms + encoder %.2f ms = %.2f ms a frame, %4.1f %% of a core at 60 fps; still frame %.2f ms; %zu B a frame, %zu copies\n",
              name, bands_ms / frames, encode_ms / frames, per, per * 6, still_ms / frames, bytes / frames, copies);
}
}  // namespace

int main(int argc, char** argv) {
  const int frames = argc > 1 ? std::atoi(argv[1]) : 120, lines = argc > 2 ? std::atoi(argv[2]) : 1;
  run("1920x1080 full terminal", 1920, 1080, 0, 1920, frames, lines);
  run("1920x1080 half terminal", 1920, 1080, 960, 1920, frames, lines);
}
