#include "core/tiles.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using relay::DamageRect;
using relay::FrameView;
using relay::PixelFormat;
namespace tiles = relay::tiles;

namespace {
struct Desk {
  int w, h;
  std::vector<uint8_t> bgra;
  Desk(int w_, int h_) : w(w_), h(h_), bgra(size_t(w_) * h_ * 4, 0) {}
  void set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = &bgra[(size_t(y) * w + x) * 4]; p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
  }
  FrameView view() const { return {bgra.data(), w * 4, w, h, PixelFormat::BGRA}; }
  std::vector<uint8_t> rgb() const {
    std::vector<uint8_t> out(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i) { out[i*3] = bgra[i*4+2]; out[i*3+1] = bgra[i*4+1]; out[i*3+2] = bgra[i*4]; }
    return out;
  }
};
// Text-like content: lines of short coloured runs on a dark background.
void paintText(Desk& d, int top, int bottom, int first_line, int line_h) {
  for (int y = top; y < bottom; ++y) {
    const int line = first_line + (y - top) / line_h, row = (y - top) % line_h;
    std::mt19937 rng(uint32_t(line) * 7919u);
    for (int x = 0; x < d.w; ++x) d.set(x, y, 30, 30, 36);
    if (row < 3 || row > line_h - 4) continue;
    int x = 40 + int(rng() % 40);
    while (x < d.w - 20) {
      const int len = 6 + int(rng() % 60);
      const uint8_t r = uint8_t(120 + rng() % 120), g = uint8_t(120 + rng() % 120), b = uint8_t(120 + rng() % 120);
      for (int i = 0; i < len && x + i < d.w; ++i) if ((x + i + row * 3 + line) % 5 < 3) d.set(x + i, y, r, g, b);
      x += len + 8 + int(rng() % 30);
    }
  }
}
struct Client {
  std::vector<uint8_t> rgb; int w = 0, h = 0;
  void take(const std::vector<uint8_t>& payload) {
    std::string error;
    if (!tiles::apply(rgb, w, h, payload.data(), payload.size(), &error)) { std::cerr << "apply: " << error << '\n'; assert(false); }
  }
};
}  // namespace

int main() {
  // Keyframe round trip and idle.
  {
    Desk d(640, 360);
    paintText(d, 0, d.h, 0, 20);
    tiles::Encoder enc(d.w, d.h);
    Client c;
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr);
    assert(enc.encode(true, 1, out));
    c.take(out);
    assert(c.rgb == d.rgb());
    assert(!enc.encode(false, 2, out));
    assert(out.empty());
  }
  // Scroll both ways with a still header and footer, damage on the scrolled band only.
  {
    Desk d(1000, 700);
    tiles::Encoder enc(d.w, d.h);
    Client c;
    std::vector<uint8_t> out;
    // Scroll by whole pixels: render the band from a pixel offset into one tall text.
    auto band = [&](int pos) {
      for (int y = 30; y < d.h - 22; ++y) {
        const int src = y - 30 + pos + 4000;
        const int line = src / 20, row = src % 20;
        Desk one(d.w, 20);
        paintText(one, 0, 20, line, 20);
        std::memcpy(&d.bgra[size_t(y) * d.w * 4], &one.bgra[size_t(row) * d.w * 4], size_t(d.w) * 4);
      }
    };
    paintText(d, 0, 30, 1000, 30);
    paintText(d, d.h - 22, d.h, 2000, 22);
    band(0);
    enc.update(d.view(), nullptr);
    enc.encode(true, 1, out); c.take(out);
    int pos = 0;
    for (int step : {17, 23, 60, -40, -7, 120, 3}) {
      pos += step;
      band(pos);
      std::vector<DamageRect> dmg{{0, 30, d.w, d.h - 52}};
      enc.update(d.view(), &dmg);
      tiles::Stats st;
      assert(enc.encode(false, 2, out, &st));
      c.take(out);
      assert(c.rgb == d.rgb());
      assert(st.copies >= 1);
      assert(st.scroll_dy == step);
      // A strip of |step| rows plus tile edges, never the whole band.
      assert(st.raw_bytes <= size_t(d.w) * 3 * size_t(std::abs(step) + 2 * tiles::tile));
    }
  }
  // Typing: one glyph changes, one small rectangle goes out.
  {
    Desk d(800, 600);
    paintText(d, 0, d.h, 0, 20);
    tiles::Encoder enc(d.w, d.h);
    Client c;
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr); enc.encode(true, 1, out); c.take(out);
    for (int y = 300; y < 316; ++y) for (int x = 401; x < 410; ++x) d.set(x, y, (x + y) % 2 ? 250 : 10, 250, 250);
    std::vector<DamageRect> dmg{{400, 300, 12, 20}};
    enc.update(d.view(), &dmg);
    tiles::Stats st;
    assert(enc.encode(false, 2, out, &st));
    c.take(out);
    assert(c.rgb == d.rgb());
    assert(st.copies == 0 && st.pixel_rects + st.fills == 1);
    assert(st.raw_bytes == 9 * 16 * 3);
  }
  // Damage that reports more than changed, and unknown damage, still converge.
  {
    Desk d(300, 300);
    tiles::Encoder enc(d.w, d.h);
    Client c;
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr); enc.encode(true, 1, out); c.take(out);
    for (int y = 10; y < 20; ++y) for (int x = 10; x < 20; ++x) d.set(x, y, 200, 10, 10);
    enc.update(d.view(), nullptr);
    tiles::Stats st;
    assert(enc.encode(false, 2, out, &st)); c.take(out);
    assert(c.rgb == d.rgb());
    assert(st.fills == 1 && st.pixel_rects == 0);
  }
  // A small message cap splits incompressible damage over several frames.
  {
    Desk d(512, 512);
    std::mt19937 rng(5);
    for (auto& b : d.bgra) b = uint8_t(rng());
    tiles::Encoder enc(d.w, d.h, 1, 64 * 1024);
    Client c;
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr);
    int frames = 0;
    bool key = true;
    while (key || enc.dirty()) {
      tiles::Stats st;
      assert(enc.encode(key, 1, out, &st));
      assert(out.size() <= 64 * 1024 || st.pixel_rects == 1);
      assert(bool(out[1] & tiles::flag_partial) == enc.dirty());  // partial until the last frame
      c.take(out);
      key = false;
      ++frames;
      assert(frames < 200);
    }
    assert(frames > 1);
    assert(c.rgb == d.rgb());
  }
  // ARGB input and a view that covers part of the desktop.
  {
    tiles::Encoder enc(200, 100);
    std::vector<uint8_t> argb(size_t(50) * 20 * 4);
    for (size_t i = 0; i < argb.size(); i += 4) { argb[i] = 255; argb[i+1] = 1; argb[i+2] = 2; argb[i+3] = 3; }
    FrameView v{argb.data(), 50 * 4, 50, 20, PixelFormat::ARGB, 100, 40};
    enc.update(v, nullptr);
    std::vector<uint8_t> out;
    Client c;
    enc.encode(true, 1, out); c.take(out);
    const size_t at = (size_t(45) * 200 + 120) * 3;
    assert(c.rgb[at] == 1 && c.rgb[at+1] == 2 && c.rgb[at+2] == 3);
    assert(c.rgb[0] == 0);
  }
  // Malformed payloads are refused.
  {
    Desk d(100, 100);
    paintText(d, 0, d.h, 0, 20);
    tiles::Encoder enc(d.w, d.h);
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr); enc.encode(true, 1, out);
    std::vector<uint8_t> rgb; int w = 0, h = 0;
    std::string error;
    auto bad = [&](std::vector<uint8_t> p) { std::vector<uint8_t> b = rgb; int bw = w, bh = h; return !tiles::apply(b, bw, bh, p.data(), p.size(), &error); };
    assert(bad({}));
    auto v = out; v[0] = 2; assert(bad(v));
    v = out; v.resize(v.size() - 1); assert(bad(v));
    v = out; v[1] = 2; assert(bad(v));
    v = out; v[20] = 9; assert(bad(v));
    v = out; v[21 + 4] = 0xff; assert(bad(v));  // width of the first op
    assert(!bad(out));
    std::vector<uint8_t> delta(out); delta[1] = 0;  // not a keyframe on an empty client
    assert(bad(delta));
  }
  // Whole-factor scaling: the client gets the k x k block means, and even scrolls stay copies at k = 2.
  {
    assert(tiles::scaleFor(3024, 1964, 1320, 858) == 2 && tiles::scaleFor(1320, 858, 1320, 858) == 1);
    assert(tiles::scaleFor(1000, 700, 2000, 2000) == 1 && tiles::scaleFor(5120, 2880, 1280, 700) == 4 && tiles::scaleFor(100, 100, 0, 0) == 1);
    Desk d(1001, 701);  // odd sizes: partial blocks on the right and bottom
    auto reference = [&](int k) {
      const int w = (d.w + k - 1) / k, h = (d.h + k - 1) / k;
      std::vector<uint8_t> out(size_t(w) * h * 3);
      for (int by = 0; by < h; ++by) for (int bx = 0; bx < w; ++bx) {
        unsigned s[3] = {0, 0, 0}, n = 0;
        for (int y = by * k; y < std::min(d.h, by * k + k); ++y) for (int x = bx * k; x < std::min(d.w, bx * k + k); ++x, ++n) {
          const uint8_t* p = &d.bgra[(size_t(y) * d.w + x) * 4];
          s[0] += p[2]; s[1] += p[1]; s[2] += p[0];
        }
        for (int c = 0; c < 3; ++c) out[(size_t(by) * w + bx) * 3 + c] = uint8_t((s[c] + n / 2) / n);
      }
      return out;
    };
    auto band = [&](int pos) {
      for (int y = 0; y < d.h; ++y) {
        const int src = y + pos + 4000, line = src / 24, row = src % 24;
        Desk one(d.w, 24);
        paintText(one, 0, 24, line, 24);
        std::memcpy(&d.bgra[size_t(y) * d.w * 4], &one.bgra[size_t(row) * d.w * 4], size_t(d.w) * 4);
      }
    };
    for (int k : {2, 3}) {
      tiles::Encoder enc(d.w, d.h, 1, 8 * 1024 * 1024 - 64, k);
      assert(enc.width() == (d.w + k - 1) / k && enc.height() == (d.h + k - 1) / k);
      Client c;
      std::vector<uint8_t> out;
      band(0);
      enc.update(d.view(), nullptr); enc.encode(true, 1, out); c.take(out);
      assert(c.w == enc.width() && c.rgb == reference(k));
      int pos = 0;
      size_t copies = 0;
      for (int step : {2 * k, 6 * k, -4 * k, 5}) {
        pos += step;
        band(pos);
        std::vector<DamageRect> dmg{{0, 0, d.w, d.h}};
        enc.update(d.view(), &dmg);
        tiles::Stats st;
        assert(enc.encode(false, 2, out, &st));
        c.take(out);
        assert(c.rgb == reference(k));
        if (step % k == 0) { assert(st.scroll_dy == step / k); copies += st.copies; }
      }
      assert(copies >= 3);
    }
    // Damage that covers part of a block still updates the whole block.
    tiles::Encoder enc(d.w, d.h, 1, 8 * 1024 * 1024 - 64, 2);
    Client c;
    std::vector<uint8_t> out;
    enc.update(d.view(), nullptr); enc.encode(true, 1, out); c.take(out);
    d.set(501, 301, 255, 0, 0);
    std::vector<DamageRect> one{{501, 301, 1, 1}};
    FrameView part{&d.bgra[(size_t(301) * d.w + 501) * 4], d.w * 4, 1, 1, PixelFormat::BGRA, 501, 301};
    enc.update(part, &one);
    assert(enc.encode(false, 2, out)); c.take(out);
    assert(c.rgb == reference(2));
  }
  std::cout << "tiles checks passed\n";
}
