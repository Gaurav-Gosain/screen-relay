// Replays recorded desktop frames through the tiles encoder and decoder.
// Each input file is NAME.WIDTHxHEIGHT.rgb: raw RGB frames back to back.
// Damage for each frame is the bounding box of the pixels that changed,
// the way a compositor reports one damaged window band.
#include "core/tiles.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using relay::DamageRect;
using relay::FrameView;
using relay::PixelFormat;
namespace tiles = relay::tiles;

static double cpuMs() { return 1000.0 * double(std::clock()) / CLOCKS_PER_SEC; }

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: tiles-bench DIR [zstd-level]\n"; return 2; }
  const int level = argc > 2 ? std::stoi(argv[2]) : 1;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(argv[1])) if (e.path().extension() == ".rgb") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  std::printf("%-22s %5s %9s %9s %9s %9s %8s %8s %6s %s\n", "case", "frames", "key KB", "avg B", "max B", "MB/s@60", "enc ms", "dec ms", "copies", "lossless");
  bool all_ok = true;
  for (const auto& path : files) {
    const std::string stem = path.stem().string();
    const auto dot = stem.rfind('.');
    int w = 0, h = 0;
    if (dot == std::string::npos || std::sscanf(stem.c_str() + dot + 1, "%dx%d", &w, &h) != 2) continue;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> rgb((std::istreambuf_iterator<char>(in)), {});
    const size_t fsz = size_t(w) * h * 3;
    const size_t n = rgb.size() / fsz;
    if (n < 2) continue;
    tiles::Encoder enc(w, h, level);
    std::vector<uint8_t> bgra(size_t(w) * h * 4, 255), out, client;
    int cw = 0, ch = 0;
    size_t total = 0, maxb = 0, copies = 0, keyb = 0, sends = 0;
    double enc_ms = 0, dec_ms = 0;
    bool ok = true;
    for (size_t f = 0; f < n; ++f) {
      const uint8_t* fr = &rgb[f * fsz];
      std::vector<DamageRect> dmg;
      if (f) {
        const uint8_t* pv = &rgb[(f - 1) * fsz];
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        for (int y = 0; y < h; ++y) {
          const uint8_t* a = fr + size_t(y) * w * 3; const uint8_t* b = pv + size_t(y) * w * 3;
          if (!std::memcmp(a, b, size_t(w) * 3)) continue;
          y0 = std::min(y0, y); y1 = y;
          int l = 0, r = w - 1;
          while (!std::memcmp(a + l * 3, b + l * 3, 3)) ++l;
          while (!std::memcmp(a + r * 3, b + r * 3, 3)) --r;
          x0 = std::min(x0, l); x1 = std::max(x1, r);
        }
        if (y1 >= 0) dmg.push_back({x0, y0, x1 - x0 + 1, y1 - y0 + 1});
      }
      for (size_t i = 0; i < size_t(w) * h; ++i) { bgra[i*4] = fr[i*3+2]; bgra[i*4+1] = fr[i*3+1]; bgra[i*4+2] = fr[i*3]; }
      const FrameView view{bgra.data(), w * 4, w, h, PixelFormat::BGRA};
      if (f && dmg.empty()) continue;
      const double c0 = cpuMs();
      enc.update(view, f ? &dmg : nullptr);
      tiles::Stats st;
      const bool sent = enc.encode(f == 0, f, out, &st);
      const double c1 = cpuMs();
      if (!sent) continue;
      std::string error;
      if (!tiles::apply(client, cw, ch, out.data(), out.size(), &error)) { std::cerr << stem << ": " << error << '\n'; ok = false; break; }
      const double c2 = cpuMs();
      if (f == 0) { keyb = out.size(); continue; }
      ++sends; total += out.size(); maxb = std::max(maxb, out.size()); copies += st.copies;
      enc_ms += c1 - c0; dec_ms += c2 - c1;
      if (std::memcmp(client.data(), fr, fsz) != 0) { std::cerr << stem << ": frame " << f << " differs\n"; ok = false; break; }
    }
    all_ok &= ok;
    const double frames = double(n - 1);
    std::printf("%-22s %5zu %9.1f %9.0f %9zu %9.2f %8.2f %8.2f %6zu %s\n", stem.c_str(), n, keyb / 1024.0,
                frames ? total / frames : 0, maxb, total / frames * 60 / 1e6, sends ? enc_ms / sends : 0, sends ? dec_ms / sends : 0, copies,
                ok ? "yes" : "NO");
  }
  return all_ok ? 0 : 1;
}
