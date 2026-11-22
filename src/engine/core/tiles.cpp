#include "tiles.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace relay::tiles {
namespace {
void put16(std::vector<uint8_t>& out, unsigned v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& out, uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); }
void put64(std::vector<uint8_t>& out, uint64_t v) { for (int s = 56; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); }
unsigned get16(const uint8_t* p) { return unsigned(p[0]) << 8 | p[1]; }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline uint64_t mix(uint64_t h) { h ^= h >> 31; h *= 0x9E3779B97F4A7C15ull; h ^= h >> 29; return h; }
// Rows copy with memmove semantics: src and dst can overlap.
void copyRect(std::vector<uint8_t>& img, int width, int px, int sx, int sy, int dx, int dy, int w, int h) {
  const size_t stride = size_t(width) * px, bytes = size_t(w) * px;
  if (sy >= dy) {
    for (int r = 0; r < h; ++r)
      std::memmove(&img[(dy + r) * stride + size_t(dx) * px], &img[(sy + r) * stride + size_t(sx) * px], bytes);
  } else {
    for (int r = h - 1; r >= 0; --r)
      std::memmove(&img[(dy + r) * stride + size_t(dx) * px], &img[(sy + r) * stride + size_t(sx) * px], bytes);
  }
}
}  // namespace

int scaleFor(int width, int height, int max_width, int max_height) {
  if (max_width <= 0 || max_height <= 0) return 1;
  return std::max(1, std::min(width / max_width, height / max_height));
}

Encoder::Encoder(int width, int height, int level, size_t max_payload, int scale)
    : scale_(std::max(1, scale)), srcWidth_(width), srcHeight_(height),
      width_((width + scale_ - 1) / scale_), height_((height + scale_ - 1) / scale_), level_(level), max_payload_(max_payload),
      tiles_x_((width_ + tile - 1) / tile), tiles_y_((height_ + tile - 1) / tile),
      cur_(size_t(width_) * height_ * 4), sent_(size_t(width_) * height_ * 4),
      dirty_(size_t(tiles_x_) * tiles_y_, 1), clean_(size_t(height_), 0),
      sentHash_(size_t(height_), 0), hashOk_(size_t(height_), 0) {
  if (width <= 0 || height <= 0 || width_ > 65535 || height_ > 65535) throw std::runtime_error("tiles: bad size");
  if (scale_ > 1) src_.assign(size_t(width) * height * 4, 0);
  cctx_ = ZSTD_createCCtx();
  if (!cctx_) throw std::runtime_error("tiles: ZSTD_createCCtx failed");
  ZSTD_CCtx_setParameter(static_cast<ZSTD_CCtx*>(cctx_), ZSTD_c_compressionLevel, level_);
}
Encoder::~Encoder() { ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(cctx_)); }

void Encoder::markDirty(int x, int y, int w, int h) {
  const int x0 = std::max(0, x) / tile, y0 = std::max(0, y) / tile;
  const int x1 = std::min(width_, x + w), y1 = std::min(height_, y + h);
  if (x1 <= std::max(0, x) || y1 <= std::max(0, y)) return;
  for (int ty = y0; ty <= (y1 - 1) / tile; ++ty)
    for (int tx = x0; tx <= (x1 - 1) / tile; ++tx) dirty_[size_t(ty) * tiles_x_ + tx] = 1;
}

bool Encoder::dirty() const { return std::find(dirty_.begin(), dirty_.end(), 1) != dirty_.end(); }

void Encoder::update(const FrameView& view, const std::vector<DamageRect>* damage) {
  std::vector<DamageRect> whole{{view.x, view.y, view.width, view.height}};
  const auto& rects = damage && !damage->empty() ? *damage : whole;
  for (const auto& d : rects) {
    const int x0 = std::max({d.x, view.x, 0}), y0 = std::max({d.y, view.y, 0});
    const int x1 = std::min({d.x + d.width, view.x + view.width, srcWidth_});
    const int y1 = std::min({d.y + d.height, view.y + view.height, srcHeight_});
    if (x1 <= x0 || y1 <= y0) continue;
    if (view.format != format_) {
      if (!formatKnown_) { format_ = view.format; formatKnown_ = true; }
      else throw std::runtime_error("tiles: pixel format changed");
    }
    formatKnown_ = true;
    if (scale_ == 1) {
      for (int y = y0; y < y1; ++y)
        std::memcpy(&cur_[(size_t(y) * width_ + x0) * 4], view.data + size_t(y - view.y) * view.stride + size_t(x0 - view.x) * 4,
                    size_t(x1 - x0) * 4);
      markDirty(x0, y0, x1 - x0, y1 - y0);
      continue;
    }
    // Keep the desktop at full size, then average whole k x k blocks into cur_.
    for (int y = y0; y < y1; ++y)
      std::memcpy(&src_[(size_t(y) * srcWidth_ + x0) * 4], view.data + size_t(y - view.y) * view.stride + size_t(x0 - view.x) * 4,
                  size_t(x1 - x0) * 4);
    const int k = scale_, bx0 = x0 / k, by0 = y0 / k, bx1 = (x1 + k - 1) / k, by1 = (y1 + k - 1) / k;
    filterBlocks(bx0, by0, bx1, by1);
    markDirty(bx0, by0, bx1 - bx0, by1 - by0);
  }
}

// Hash of the colour bytes of one row span; the alpha byte is ignored.
// Averages the k x k blocks [bx0, bx1) x [by0, by1) of src_ into cur_.
// Rounding is (sum + n / 2) / n for every byte, partial edge blocks included.
void Encoder::filterBlocks(int bx0, int by0, int bx1, int by1) {
  const int k = scale_;
  const size_t sstride = size_t(srcWidth_) * 4;
  // Whole blocks: the columns before a partial block at the right edge.
  const int full_bx1 = std::min(bx1, srcWidth_ / k);
  if (k == 2) {
    for (int by = by0; by < by1; ++by) {
      const int sy = by * 2;
      const uint8_t* r0 = &src_[size_t(sy) * sstride];
      const uint8_t* r1 = sy + 1 < srcHeight_ ? r0 + sstride : r0;  // a last odd row counts twice: same mean
      uint8_t* t = &cur_[size_t(by) * width_ * 4];
      for (int bx = bx0; bx < full_bx1; ++bx) {
        const uint8_t* p = r0 + size_t(bx) * 8;
        const uint8_t* q = r1 + size_t(bx) * 8;
        uint8_t* o = t + size_t(bx) * 4;
        for (int c = 0; c < 4; ++c) o[c] = uint8_t((unsigned(p[c]) + p[c + 4] + q[c] + q[c + 4] + 2) >> 2);
      }
      for (int bx = full_bx1; bx < bx1; ++bx) {  // right edge, one column
        const uint8_t* p = r0 + size_t(bx) * 8;
        const uint8_t* q = r1 + size_t(bx) * 8;
        uint8_t* o = t + size_t(bx) * 4;
        for (int c = 0; c < 4; ++c) o[c] = uint8_t((unsigned(p[c]) + q[c] + 1) >> 1);
      }
    }
    return;
  }
  std::vector<uint32_t> sum(size_t(bx1 - bx0) * 4);
  for (int by = by0; by < by1; ++by) {
    std::fill(sum.begin(), sum.end(), 0);
    const int sy0 = by * k, sy1 = std::min(srcHeight_, sy0 + k);
    for (int sy = sy0; sy < sy1; ++sy) {
      const uint8_t* s = &src_[size_t(sy) * sstride];
      for (int bx = bx0; bx < bx1; ++bx) {
        uint32_t* acc = &sum[size_t(bx - bx0) * 4];
        const int sx1 = std::min(srcWidth_, bx * k + k);
        for (int sx = bx * k; sx < sx1; ++sx) {
          const uint8_t* p = s + size_t(sx) * 4;
          acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
        }
      }
    }
    uint8_t* t = &cur_[(size_t(by) * width_ + bx0) * 4];
    for (int bx = bx0; bx < bx1; ++bx, t += 4) {
      const uint32_t n = uint32_t(sy1 - sy0) * uint32_t(std::min(srcWidth_, bx * k + k) - bx * k);
      const uint32_t* acc = &sum[size_t(bx - bx0) * 4];
      for (int c = 0; c < 4; ++c) t[c] = uint8_t((acc[c] + n / 2) / n);
    }
  }
}

uint64_t Encoder::rowHash(const std::vector<uint8_t>& image, int y, int x0, int x1) const {
  const uint8_t* p = &image[(size_t(y) * width_ + x0) * 4];
  size_t n = size_t(x1 - x0) * 4;
  const uint64_t m = format_ == PixelFormat::BGRA ? 0x00FFFFFF00FFFFFFull : 0xFFFFFF00FFFFFF00ull;
  uint64_t a = 0x243F6A8885A308D3ull ^ n, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
  constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
  while (n >= 32) {
    uint64_t w[4]; std::memcpy(w, p, 32);
    a = (a ^ (w[0] & m)) * k; b = (b ^ (w[1] & m)) * k; c = (c ^ (w[2] & m)) * k; d = (d ^ (w[3] & m)) * k;
    a ^= a >> 32; b ^= b >> 32; c ^= c >> 32; d ^= d >> 32;
    p += 32; n -= 32;
  }
  uint64_t h = mix(a ^ mix(b ^ mix(c ^ mix(d))));
  while (n >= 8) { uint64_t w; std::memcpy(&w, p, 8); h = mix(h ^ (w & m)); p += 8; n -= 8; }
  if (n) { uint32_t w; std::memcpy(&w, p, 4); h = mix(h ^ (w & uint32_t(m)) ^ (1ull << 63)); }
  return h;
}

// Finds one vertical shift for the area: rows of the current frame that equal
// rows of the sent frame dy rows lower (dy > 0: content moved up). Adds copy
// ops for runs of at least min_run rows and applies them to sent_.
int Encoder::detectScroll(const Rect& a, std::vector<Op>& ops) {
  constexpr int min_run = 8;
  if (a.h < 4 * min_run || a.w < 128) return 0;
  if (a.x != hashX0_ || a.x + a.w != hashX1_) {
    std::fill(hashOk_.begin(), hashOk_.end(), 0);
    hashX0_ = a.x; hashX1_ = a.x + a.w;
  }
  std::vector<uint64_t> hs(a.h);
  auto& hc = curHash_;
  hc.assign(size_t(a.h), 0);
  hashArea_ = a;
  // A row that repeats (blank lines, rows inside a glyph) votes for each of
  // its first few positions; only the true shift collects votes from most rows.
  constexpr size_t max_candidates = 8;
  // Sent rows sorted by hash, then by row: the first max_candidates rows of a hash are its candidates.
  // A sorted array and a flat vote table: no allocation for each row.
  auto& where = scrollRows_;
  where.resize(size_t(a.h));
  for (int i = 0; i < a.h; ++i) {
    hs[i] = hashOk_[a.y + i] ? sentHash_[a.y + i] : rowHash(sent_, a.y + i, a.x, a.x + a.w);
    hc[i] = rowHash(cur_, a.y + i, a.x, a.x + a.w);
    where[size_t(i)] = {hs[i], i};
  }
  std::sort(where.begin(), where.end());
  auto& votes = scrollVotes_;  // index dy + a.h
  votes.assign(size_t(2 * a.h + 1), 0);
  for (int i = 0; i < a.h; ++i) {
    if (hc[i] == hs[i]) continue;
    auto it = std::lower_bound(where.begin(), where.end(), std::pair<uint64_t, int>{hc[i], 0});
    for (size_t n = 0; n < max_candidates && it != where.end() && it->first == hc[i]; ++n, ++it) ++votes[size_t(it->second - i + a.h)];
  }
  int best = 0, count = 0;
  for (int dy = -a.h; dy <= a.h; ++dy) {
    const int n = votes[size_t(dy + a.h)];
    if (n > count || (n && n == count && std::abs(dy) < std::abs(best))) { best = dy; count = n; }
  }
  if (!best || count < std::max(min_run, a.h / 16)) return 0;
  // Runs of rows i where cur[i] == sent[i + best], checked byte for byte.
  std::vector<Rect> runs;
  const size_t bytes = size_t(a.w) * 4;
  int start = -1;
  for (int i = 0; i <= a.h; ++i) {
    const int j = i + best;
    bool same = i < a.h && j >= 0 && j < a.h && hc[i] == hs[j] && hc[i] != hs[i] &&
                std::memcmp(&cur_[(size_t(a.y + i) * width_ + a.x) * 4], &sent_[(size_t(a.y + j) * width_ + a.x) * 4], bytes) == 0;
    // Rows that already match in place may sit inside a moved band (blank lines); keep them in the run.
    if (!same && i < a.h && j >= 0 && j < a.h && start >= 0 && hc[i] == hs[i] && hs[i] == hs[j]) same = true;
    if (same && start < 0) start = i;
    if (!same && start >= 0) { if (i - start >= min_run) runs.push_back({a.x, a.y + start, a.w, i - start}); start = -1; }
  }
  if (runs.empty()) return 0;
  if (best < 0) std::reverse(runs.begin(), runs.end());
  for (const auto& r : runs) {
    Op op{op_copy, r, r.x, r.y + best, {0, 0, 0}};
    applyCopy(op);
    ops.push_back(op);
    markDirty(r.x, r.y, r.w, r.h);
    // These rows now equal the current frame across the area; the diff skips them.
    for (int y = r.y; y < r.y + r.h; ++y) clean_[y] = 1;
  }
  cleanX0_ = a.x; cleanX1_ = a.x + a.w;
  return best;
}

bool Encoder::detectMove(const Rect& a, std::vector<Op>& ops, Stats& st) {
  constexpr int max_shift = 600, min_tiles = 4;
  if (a.w < 2 * tile || a.h < 2 * tile) return false;
  // Column sums do not change when content moves up or down inside the area, and row sums do not
  // change when it moves sideways, so each gives one axis of the shift. Compare their slopes,
  // which follow the edges of the moved window and not the flat colours.
  auto profile = [&](const std::vector<uint8_t>& img, bool columns, std::vector<int64_t>& p) {
    const int n = columns ? a.w : a.h;
    p.assign(size_t(n), 0);
    for (int y = a.y; y < a.y + a.h; ++y) {
      const uint8_t* q = &img[(size_t(y) * width_ + a.x) * 4];
      if (columns) {
        for (int x = 0; x < a.w; ++x, q += 4) p[size_t(x)] += q[0] + q[1] + q[2];
      } else {
        int64_t s = 0;
        for (int x = 0; x < a.w; ++x, q += 4) s += q[0] + q[1] + q[2];
        p[size_t(y - a.y)] = s;
      }
    }
  };
  auto best = [&](const std::vector<int64_t>& c, const std::vector<int64_t>& s) {
    const int n = int(c.size()), range = std::min(max_shift, n - tile);
    int64_t best_cost = -1; int best_d = 0;
    for (int d = -range; d <= range; ++d) {
      int64_t cost = 0; int count = 0;
      for (int i = std::max(1, d + 1); i < std::min(n, n + d); ++i) {
        const int64_t dc = c[size_t(i)] - c[size_t(i - 1)], ds = s[size_t(i - d)] - s[size_t(i - d - 1)];
        cost += dc > ds ? dc - ds : ds - dc; ++count;
      }
      if (count < n / 4) continue;
      cost = cost * 1024 / count;
      if (best_cost < 0 || cost < best_cost) { best_cost = cost; best_d = d; }
    }
    return best_d;
  };
  // Whole tiles of the area whose pixels equal sent_ shifted by (dx, dy). Every match is exact.
  std::vector<std::pair<int, int>> matched;
  auto verify = [&](int dx, int dy) {
    matched.clear();
    if (!dx && !dy) return false;
    for (int ty = a.y / tile; ty * tile < a.y + a.h; ++ty)
      for (int tx = a.x / tile; tx * tile < a.x + a.w; ++tx) {
        if (!dirty_[size_t(ty) * tiles_x_ + tx]) continue;
        const int x0 = tx * tile, y0 = ty * tile, x1 = std::min(width_, x0 + tile), y1 = std::min(height_, y0 + tile);
        if (x0 - dx < 0 || y0 - dy < 0 || x1 - dx > width_ || y1 - dy > height_) continue;
        const size_t n = size_t(x1 - x0) * 4;
        bool same = true, unchanged = true;
        for (int y = y0; y < y1 && same; ++y) {
          const uint8_t* c = &cur_[(size_t(y) * width_ + x0) * 4];
          same = std::memcmp(c, &sent_[(size_t(y - dy) * width_ + x0 - dx) * 4], n) == 0;
          if (unchanged) unchanged = std::memcmp(c, &sent_[(size_t(y) * width_ + x0) * 4], n) == 0;
        }
        if (same && !unchanged) matched.push_back({tx, ty});
      }
    return int(matched.size()) >= min_tiles;
  };
  // Sample pixels of changed tiles that are not flat colour (flat colour matches anywhere), taken
  // only when a search needs them.
  std::vector<std::pair<int, int>> samples;
  bool sampled = false;
  auto sample = [&]() {
    if (sampled) return samples.size() >= 16;
    sampled = true;
    for (int ty = a.y / tile; ty * tile < a.y + a.h; ++ty)
      for (int tx = a.x / tile; tx * tile < a.x + a.w; ++tx) {
        if (!dirty_[size_t(ty) * tiles_x_ + tx]) continue;
        for (int sy = 4; sy < tile; sy += 8)
          for (int sx = 4 + (sy & 8); sx < tile; sx += 16) {
            const int x = tx * tile + sx, y = ty * tile + sy;
            if (x + 1 >= width_ || y + 1 >= height_) continue;
            const uint8_t* p = &cur_[(size_t(y) * width_ + x) * 4];
            if (std::memcmp(p, &sent_[(size_t(y) * width_ + x) * 4], 3) != 0 &&
                (std::memcmp(p, p + 4, 3) != 0 || std::memcmp(p, p + size_t(width_) * 4, 3) != 0)) samples.push_back({x, y});
          }
      }
    if (samples.size() > 256) {  // an even spread over the whole area, not its first tiles
      std::vector<std::pair<int, int>> spread;
      for (size_t i = 0; i < 256; ++i) spread.push_back(samples[i * samples.size() / 256]);
      samples.swap(spread);
    }
    return samples.size() >= 16;
  };
  const uint32_t mask = format_ == PixelFormat::BGRA ? 0x00FFFFFFu : 0xFFFFFF00u;
  auto pixel = [&](const std::vector<uint8_t>& img, int x, int y) { uint32_t v; std::memcpy(&v, &img[(size_t(y) * width_ + x) * 4], 4); return v & mask; };
  // Exact votes along one axis, the other fixed: the shift that maps the most samples onto equal
  // pixels of sent_ wins. votes gets the count of the winner.
  auto vote = [&](bool vertical, int fixed, int center, int reach, int& votes) {
    const int limit = (vertical ? height_ : width_) - 1;
    int best_d = center, best_n = -1;
    for (int d = std::max(-limit, center - reach); d <= std::min(limit, center + reach); ++d) {
      if (!d && !fixed) continue;
      int n = 0;
      for (const auto& [x, y] : samples) {
        const int sx = x - (vertical ? fixed : d), sy = y - (vertical ? d : fixed);
        if (sx < 0 || sy < 0 || sx >= width_ || sy >= height_) continue;
        n += pixel(cur_, x, y) == pixel(sent_, sx, sy);
      }
      if (n > best_n) { best_n = n; best_d = d; }
    }
    votes = best_n;
    return best_d;
  };
  // Refine a guess: alternate the axes inside reach of it. False when few samples agree.
  auto refine = [&](int& ex, int& ey, int reach) {
    int votes = 0;
    for (int round = 0; round < 2; ++round) {
      ey = vote(true, ex, ey, reach, votes);
      ex = vote(false, ey, ex, reach, votes);
    }
    return votes * 4 >= int(samples.size());
  };
  // 1. The previous shift: a drag keeps moving the same way, often by the same step.
  int dx = lastMoveDx_, dy = lastMoveDy_;
  bool found = verify(dx, dy);
  // 2. Near the previous shift: a drag that the ACK pace coalesces changes its step a little.
  if (!found && (lastMoveDx_ || lastMoveDy_) && sample()) {
    // Both axes at once near the last shift, on 64 of the samples: within 12 px, then 32.
    const size_t step = std::max<size_t>(1, samples.size() / 64);
    int used = 0;
    for (size_t i = 0; i < samples.size(); i += step) ++used;
    for (const int reach : {12, 32}) {
      int best_n = -1, bx = lastMoveDx_, by = lastMoveDy_;
      for (int cy = lastMoveDy_ - reach; cy <= lastMoveDy_ + reach; ++cy)
        for (int cx = lastMoveDx_ - reach; cx <= lastMoveDx_ + reach; ++cx) {
          if (!cx && !cy) continue;
          int n = 0;
          for (size_t i = 0; i < samples.size(); i += step) {
            const auto [x, y] = samples[i];
            const int sx = x - cx, sy = y - cy;
            if (sx < 0 || sy < 0 || sx >= width_ || sy >= height_) continue;
            n += pixel(cur_, x, y) == pixel(sent_, sx, sy);
          }
          if (n > best_n) { best_n = n; bx = cx; by = cy; }
        }
      dx = bx; dy = by;
      found = best_n * 4 >= used && verify(dx, dy);
      if (found) break;
    }
  }
  // 3. A search over the area, unless the last ones found nothing: then it waits 0, 1, 2, ... 64 frames.
  if (!found) {
    if (moveWait_ > 0) { --moveWait_; lastMoveDx_ = lastMoveDy_ = 0; return false; }
    // A moved window brings its colours along; new content (video, noise) mostly does not.
    // Look for the colours of 64 samples in sent_ over the area, every second row and column.
    auto coloursKnown = [&]() {
      std::array<uint32_t, 128> set{};
      std::array<uint8_t, 128> hit{};
      int count = 0;
      const size_t step = std::max<size_t>(1, samples.size() / 64);
      for (size_t i = 0; i < samples.size(); i += step) {
        const uint32_t c = pixel(cur_, samples[i].first, samples[i].second) | 1u;
        size_t k = (c * 2654435761u) >> 25;
        while (set[k] && set[k] != c) k = (k + 1) & 127;
        if (!set[k]) { set[k] = c; ++count; }
      }
      int found = 0;
      for (int y = a.y; y < a.y + a.h && found * 2 < count; y += 2)
        for (int x = a.x; x < a.x + a.w; x += 2) {
          const uint32_t c = pixel(sent_, x, y) | 1u;
          size_t k = (c * 2654435761u) >> 25;
          while (set[k] && set[k] != c) k = (k + 1) & 127;
          if (set[k] == c && !hit[k]) { hit[k] = 1; ++found; }
        }
      return found * 2 >= count;
    };
    if (sample() && coloursKnown()) {
      profile(cur_, true, profCur_); profile(sent_, true, profSent_);
      dx = best(profCur_, profSent_); dy = 0;
      found = refine(dx, dy, max_shift) && verify(dx, dy);
    }
    if (!found) {
      // The first failure waits 0 frames, the next ones 1, 2, 4, ... 64.
      moveWait_ = moveBackoff_;
      moveBackoff_ = std::clamp(moveBackoff_ * 2, 1, 64);
      lastMoveDx_ = lastMoveDy_ = 0;
      return false;
    }
  }
  moveBackoff_ = 0;
  // Usually the matched tiles are the inside of one window: then grow their box to the pixel edges
  // of the window and send one copy, which has memmove semantics on its own.
  {
    int x0 = width_, y0 = height_, x1 = 0, y1 = 0;
    for (const auto& [tx, ty] : matched) {
      x0 = std::min(x0, tx * tile); y0 = std::min(y0, ty * tile);
      x1 = std::max(x1, std::min(width_, tx * tile + tile)); y1 = std::max(y1, std::min(height_, ty * tile + tile));
    }
    auto rowMatches = [&](int y, int l, int r) {
      return y - dy >= 0 && y - dy < height_ && l - dx >= 0 && r - dx <= width_ &&
             std::memcmp(&cur_[(size_t(y) * width_ + l) * 4], &sent_[(size_t(y - dy) * width_ + l - dx) * 4], size_t(r - l) * 4) == 0;
    };
    auto columnMatches = [&](int x, int t, int b) {
      if (x - dx < 0 || x - dx >= width_ || t - dy < 0 || b - dy > height_) return false;
      for (int y = t; y < b; ++y)
        if (std::memcmp(&cur_[(size_t(y) * width_ + x) * 4], &sent_[(size_t(y - dy) * width_ + x - dx) * 4], 3) != 0) return false;
      return true;
    };
    bool whole = true;
    for (int y = y0; y < y1 && whole; ++y) whole = rowMatches(y, x0, x1);
    if (whole) {
      // The window edge lies in the tiles around the box, so the box grows by less than a tile on
      // each side. A wider growth would follow flat background, which matches under any shift.
      const int lx = std::max(0, x0 - (tile - 1)), rx = std::min(width_, x1 + (tile - 1));
      const int ty = std::max(0, y0 - (tile - 1)), by = std::min(height_, y1 + (tile - 1));
      for (bool grew = true; grew;) {
        grew = false;
        if (x0 > lx && columnMatches(x0 - 1, y0, y1)) { --x0; grew = true; }
        if (x1 < rx && columnMatches(x1, y0, y1)) { ++x1; grew = true; }
        if (y0 > ty && rowMatches(y0 - 1, x0, x1)) { --y0; grew = true; }
        if (y1 < by && rowMatches(y1, x0, x1)) { ++y1; grew = true; }
      }
      const Rect r{x0, y0, x1 - x0, y1 - y0};
      Op op{op_copy, r, r.x - dx, r.y - dy, {0, 0, 0}};
      applyCopy(op);
      ops.push_back(op);
      for (int y = r.y; y < r.y + r.h; ++y) hashOk_[size_t(y)] = 0;
      markDirty(r.x, r.y, r.w, r.h);
      lastMoveDx_ = dx; lastMoveDy_ = dy;
      st.move_dx = dx; st.move_dy = dy;
      return true;
    }
  }
  // Copies run so that no copy reads pixels an earlier copy of this frame wrote: tile rows against
  // the vertical shift, and within a row against the horizontal shift. Side by side tiles merge.
  std::sort(matched.begin(), matched.end(), [&](const auto& p, const auto& q) {
    if (p.second != q.second) return dy > 0 ? p.second > q.second : p.second < q.second;
    return dx > 0 ? p.first > q.first : p.first < q.first;
  });
  std::vector<Rect> runs;
  for (const auto& [tx, ty] : matched) {
    const int x0 = tx * tile, y0 = ty * tile, w = std::min(width_, x0 + tile) - x0, h = std::min(height_, y0 + tile) - y0;
    if (!runs.empty()) {
      auto& r = runs.back();
      if (r.y == y0 && r.h == h && (r.x + r.w == x0 || x0 + w == r.x)) { r = {std::min(r.x, x0), y0, r.w + w, h}; continue; }
    }
    runs.push_back({x0, y0, w, h});
  }
  for (const auto& r : runs) {
    Op op{op_copy, r, r.x - dx, r.y - dy, {0, 0, 0}};
    applyCopy(op);
    ops.push_back(op);
    for (int y = r.y; y < r.y + r.h; ++y) hashOk_[size_t(y)] = 0;
  }
  lastMoveDx_ = dx; lastMoveDy_ = dy;
  st.move_dx = dx; st.move_dy = dy;
  return true;
}

void Encoder::applyCopy(const Op& op) { copyRect(sent_, width_, 4, op.sx, op.sy, op.r.x, op.r.y, op.r.w, op.r.h); }

bool Encoder::solid(const Rect& r, uint8_t rgb[3]) const {
  const uint32_t m = format_ == PixelFormat::BGRA ? 0x00FFFFFFu : 0xFFFFFF00u;  // little-endian words
  uint32_t first; std::memcpy(&first, &cur_[(size_t(r.y) * width_ + r.x) * 4], 4);
  first &= m;
  for (int y = r.y; y < r.y + r.h; ++y) {
    const uint8_t* p = &cur_[(size_t(y) * width_ + r.x) * 4];
    for (int x = 0; x < r.w; ++x, p += 4) { uint32_t v; std::memcpy(&v, p, 4); if ((v & m) != first) return false; }
  }
  toRgb(&cur_[(size_t(r.y) * width_ + r.x) * 4], rgb, 1);
  return true;
}

void Encoder::toRgb(const uint8_t* s, uint8_t* t, int n) const {
  if (format_ == PixelFormat::BGRA) for (int i = 0; i < n; ++i, s += 4, t += 3) { t[0] = s[2]; t[1] = s[1]; t[2] = s[0]; }
  else for (int i = 0; i < n; ++i, s += 4, t += 3) { t[0] = s[1]; t[1] = s[2]; t[2] = s[3]; }
}

void Encoder::diffTiles(std::vector<Op>& ops) {
  std::vector<Rect> found;  // tight changed box per dirty tile, in tile order
  std::vector<int> tile_of;
  for (int ty = 0; ty < tiles_y_; ++ty) {
    for (int tx = 0; tx < tiles_x_; ++tx) {
      if (!dirty_[size_t(ty) * tiles_x_ + tx]) continue;
      const int x0 = tx * tile, y0 = ty * tile, x1 = std::min(width_, x0 + tile), y1 = std::min(height_, y0 + tile);
      int bx0 = x1, bx1 = x0, by0 = y1, by1 = y0;
      const bool inside = x0 >= cleanX0_ && x1 <= cleanX1_;
      for (int y = y0; y < y1; ++y) {
        if (inside && clean_[y]) continue;
        const uint8_t* c = &cur_[(size_t(y) * width_ + x0) * 4];
        const uint8_t* s = &sent_[(size_t(y) * width_ + x0) * 4];
        const size_t n = size_t(x1 - x0) * 4;
        if (std::memcmp(c, s, n) == 0) continue;
        by0 = std::min(by0, y); by1 = y + 1;
        // Trim equal bytes from both ends, 8 bytes at a time, then byte by byte.
        size_t l = 0, r = n;
        while (l + 8 <= n && load64(c + l) == load64(s + l)) l += 8;
        while (l < n && c[l] == s[l]) ++l;
        while (r >= l + 8 && load64(c + r - 8) == load64(s + r - 8)) r -= 8;
        while (r > l && c[r - 1] == s[r - 1]) --r;
        bx0 = std::min(bx0, x0 + int(l / 4)); bx1 = std::max(bx1, x0 + int((r + 3) / 4));
      }
      if (by1 > by0) found.push_back({bx0, by0, bx1 - bx0, by1 - by0});
    }
  }
  // Merge boxes in the same tile row when they touch tiles side by side.
  std::vector<Rect> merged;
  for (const auto& r : found) {
    if (!merged.empty()) {
      auto& m = merged.back();
      const bool same_row = m.y / tile == r.y / tile;
      const int gap = r.x - (m.x + m.w);
      if (same_row && gap >= 0 && gap < tile) {
        const int y0 = std::min(m.y, r.y), y1 = std::max(m.y + m.h, r.y + r.h);
        m = {m.x, y0, r.x + r.w - m.x, y1 - y0};
        continue;
      }
    }
    merged.push_back(r);
  }
  // Merge vertically adjacent boxes with the same columns (full-width bands, keyframes).
  std::vector<Rect> bands;
  for (const auto& r : merged) {
    bool done = false;
    for (auto& b : bands) {
      if (b.x == r.x && b.w == r.w && b.y + b.h == r.y) { b.h += r.h; done = true; break; }
    }
    if (!done) bands.push_back(r);
  }
  for (const auto& r : bands) {
    Op op{op_pixels, r, 0, 0, {0, 0, 0}};
    if (solid(r, op.rgb)) op.kind = op_fill;
    ops.push_back(op);
  }
}

void Encoder::commit(const Op& op) {
  for (int y = op.r.y; y < op.r.y + op.r.h; ++y)
    std::memcpy(&sent_[(size_t(y) * width_ + op.r.x) * 4], &cur_[(size_t(y) * width_ + op.r.x) * 4], size_t(op.r.w) * 4);
}

bool Encoder::encode(bool keyframe, uint64_t pts_us, std::vector<uint8_t>& out, Stats* stats) {
  out.clear();
  Stats st;
  if (keyframe) { std::fill(sent_.begin(), sent_.end(), 0); std::fill(dirty_.begin(), dirty_.end(), 1); }
  if (!keyframe && !dirty()) return false;
  std::vector<Op> ops;
  hashArea_ = {0, 0, 0, 0};
  if (keyframe) std::fill(hashOk_.begin(), hashOk_.end(), 0);
  std::fill(clean_.begin(), clean_.end(), 0);
  cleanX0_ = cleanX1_ = 0;
  if (!keyframe) {
    // Scroll detection on the bounding box of the dirty tiles.
    int tx0 = tiles_x_, tx1 = -1, ty0 = tiles_y_, ty1 = -1;
    for (int ty = 0; ty < tiles_y_; ++ty)
      for (int tx = 0; tx < tiles_x_; ++tx)
        if (dirty_[size_t(ty) * tiles_x_ + tx]) { tx0 = std::min(tx0, tx); tx1 = std::max(tx1, tx); ty0 = std::min(ty0, ty); ty1 = std::max(ty1, ty); }
    const Rect area{tx0 * tile, ty0 * tile, std::min(width_, (tx1 + 1) * tile) - tx0 * tile, std::min(height_, (ty1 + 1) * tile) - ty0 * tile};
    // A scroll first. When its copies cover under half of the area, look for a moved window too;
    // its copies then work on the buffer after the scroll copies, as the client applies them.
    st.scroll_dy = detectScroll(area, ops);
    int scrolled = 0;
    for (const auto& op : ops) if (op.kind == op_copy) scrolled += op.r.h;
    if (scrolled * 2 < area.h) detectMove(area, ops, st);
  }
  const size_t copies = ops.size();
  diffTiles(ops);
  if (!keyframe && ops.empty()) {
    // The damage did not change a pixel (Hyprland reports the whole output on every frame).
    std::fill(dirty_.begin(), dirty_.end(), 0);
    return false;
  }
  // Size limit: drop pixel ops from the end until the message fits. Their tiles stay dirty.
  std::fill(dirty_.begin(), dirty_.end(), 0);
  size_t keep = ops.size();
  std::vector<Rect> rest;  // parts cut off by the size limit; they stay dirty
  for (;;) {
    raw_.clear();
    for (size_t i = copies; i < keep; ++i) {
      const auto& op = ops[i];
      if (op.kind != op_pixels) continue;
      for (int y = op.r.y; y < op.r.y + op.r.h; ++y) {
        const size_t at = raw_.size();
        raw_.resize(at + size_t(op.r.w) * 3);
        toRgb(&cur_[(size_t(y) * width_ + op.r.x) * 4], &raw_[at], op.r.w);
      }
    }
    blob_.resize(raw_.empty() ? 0 : ZSTD_compressBound(raw_.size()));
    size_t n = 0;
    if (!raw_.empty()) {
      n = ZSTD_compress2(static_cast<ZSTD_CCtx*>(cctx_), blob_.data(), blob_.size(), raw_.data(), raw_.size());
      if (ZSTD_isError(n)) throw std::runtime_error(std::string("tiles: zstd: ") + ZSTD_getErrorName(n));
    }
    blob_.resize(n);
    const size_t total = header_bytes + keep * 17 + n;
    if (total <= max_payload_) break;
    st.split = true;
    if (keep > copies + 1) { keep = copies + (keep - copies) / 2; continue; }
    auto& last = ops[keep - 1];
    if (last.kind != op_pixels || last.r.h < 2) break;
    // One rectangle left: send its top half now.
    const int top = last.r.h / 2;
    rest.push_back({last.r.x, last.r.y + top, last.r.w, last.r.h - top});
    last.r.h = top;
  }
  for (size_t i = keep; i < ops.size(); ++i) markDirty(ops[i].r.x, ops[i].r.y, ops[i].r.w, ops[i].r.h);
  for (const auto& r : rest) markDirty(r.x, r.y, r.w, r.h);
  ops.resize(keep);
  for (size_t i = copies; i < ops.size(); ++i) commit(ops[i]);
  // Every changed row lies in the area. With nothing left over, sent_ equals
  // cur_ there, so the current hashes are next frame's sent hashes.
  const bool whole = keep == ops.size() && rest.empty();
  for (const auto& op : ops) for (int y = op.r.y; y < op.r.y + op.r.h; ++y) hashOk_[y] = 0;
  if (whole && hashArea_.h)
    for (int i = 0; i < hashArea_.h; ++i) { sentHash_[hashArea_.y + i] = curHash_[i]; hashOk_[hashArea_.y + i] = 1; }

  out.reserve(header_bytes + ops.size() * 13 + blob_.size());
  out.push_back(format_version);
  // Bit 2 (partial): the size limit left damage, and the next frames finish this capture.
  out.push_back(uint8_t((keyframe ? flag_keyframe : 0) | (dirty() ? flag_partial : 0)));
  put16(out, unsigned(width_)); put16(out, unsigned(height_));
  put64(out, pts_us);
  put16(out, unsigned(ops.size()));
  put32(out, uint32_t(blob_.size()));
  for (const auto& op : ops) {
    out.push_back(op.kind);
    if (op.kind == op_copy) { put16(out, unsigned(op.sx)); put16(out, unsigned(op.sy)); }
    put16(out, unsigned(op.r.x)); put16(out, unsigned(op.r.y)); put16(out, unsigned(op.r.w)); put16(out, unsigned(op.r.h));
    if (op.kind == op_fill) { out.push_back(op.rgb[0]); out.push_back(op.rgb[1]); out.push_back(op.rgb[2]); }
    if (op.kind == op_copy) ++st.copies; else if (op.kind == op_fill) ++st.fills; else ++st.pixel_rects;
  }
  out.insert(out.end(), blob_.begin(), blob_.end());
  st.raw_bytes = raw_.size(); st.blob_bytes = blob_.size(); st.payload_bytes = out.size();
  if (stats) *stats = st;
  return true;
}

bool apply(std::vector<uint8_t>& rgb, int& width, int& height, const uint8_t* p, size_t size, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  if (size < header_bytes) return fail("short payload");
  if (p[0] != format_version) return fail("unknown version");
  if (p[1] & ~unsigned(flag_keyframe | flag_partial)) return fail("unknown flags");
  const bool key = p[1] & 1;
  const int w = int(get16(p + 2)), h = int(get16(p + 4));
  if (!w || !h) return fail("zero size");
  const unsigned count = get16(p + 14);
  const uint32_t blob = get32(p + 16);
  if (key) { width = w; height = h; rgb.assign(size_t(w) * h * 3, 0); }
  else if (w != width || h != height || rgb.size() != size_t(w) * h * 3) return fail("size changed without a keyframe");
  if (blob > size - header_bytes) return fail("blob length");
  const uint8_t* q = p + header_bytes;
  const uint8_t* end = p + size - blob;
  // First pass: parse ops and count pixel bytes.
  struct Parsed { uint8_t kind; int sx, sy, x, y, w, h; uint8_t c[3]; };
  std::vector<Parsed> ops;
  ops.reserve(count);
  size_t need = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (q >= end) return fail("truncated ops");
    Parsed o{*q++, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
    const size_t len = o.kind == op_copy ? 12 : o.kind == op_pixels ? 8 : o.kind == op_fill ? 11 : 0;
    if (!len) return fail("unknown op");
    if (size_t(end - q) < len) return fail("truncated ops");
    if (o.kind == op_copy) { o.sx = int(get16(q)); o.sy = int(get16(q + 2)); q += 4; }
    o.x = int(get16(q)); o.y = int(get16(q + 2)); o.w = int(get16(q + 4)); o.h = int(get16(q + 6)); q += 8;
    if (o.kind == op_fill) { std::memcpy(o.c, q, 3); q += 3; }
    if (!o.w || !o.h || o.x + o.w > w || o.y + o.h > h) return fail("rectangle out of bounds");
    if (o.kind == op_copy && (o.sx + o.w > w || o.sy + o.h > h)) return fail("copy source out of bounds");
    if (o.kind == op_pixels) need += size_t(o.w) * o.h * 3;
    ops.push_back(o);
  }
  if (q != end) return fail("trailing bytes after ops");
  std::vector<uint8_t> pixels(need);
  if (need) {
    if (!blob) return fail("missing pixel blob");
    const size_t got = ZSTD_decompress(pixels.data(), pixels.size(), end, blob);
    if (ZSTD_isError(got) || got != need) return fail("pixel blob does not decode to the expected size");
  } else if (blob) return fail("pixel blob without pixel ops");
  size_t off = 0;
  const size_t stride = size_t(w) * 3;
  for (const auto& o : ops) {
    if (o.kind == op_copy) { copyRect(rgb, w, 3, o.sx, o.sy, o.x, o.y, o.w, o.h); continue; }
    for (int y = o.y; y < o.y + o.h; ++y) {
      uint8_t* t = &rgb[y * stride + size_t(o.x) * 3];
      if (o.kind == op_pixels) { std::memcpy(t, &pixels[off], size_t(o.w) * 3); off += size_t(o.w) * 3; }
      else for (int x = 0; x < o.w; ++x, t += 3) std::memcpy(t, o.c, 3);
    }
  }
  return true;
}

}  // namespace relay::tiles
