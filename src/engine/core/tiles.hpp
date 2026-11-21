#pragma once
// Lossless "tiles" codec: damage-driven RGB rectangles, vertical scroll copies
// and one zstd frame per message. The wire format is the payload of NSL1
// message type 8; see PROTOCOL.md.
#include "capture-source.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace relay::tiles {

constexpr uint8_t format_version = 1;
constexpr size_t header_bytes = 20;
constexpr int tile = 64;

enum OpKind : uint8_t { op_copy = 1, op_pixels = 2, op_fill = 3 };

struct Stats {
  size_t copies = 0, pixel_rects = 0, fills = 0;
  size_t raw_bytes = 0, blob_bytes = 0, payload_bytes = 0;
  int scroll_dy = 0;
  bool split = false;
};

/// Holds the desktop as the client last saw it and builds type 8 payloads that
/// move the client to the current desktop.
class Encoder {
 public:
  /// level is the zstd level; max_payload caps one message.
  Encoder(int width, int height, int level = 1, size_t max_payload = 8 * 1024 * 1024 - 64);
  ~Encoder();
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  int width() const { return width_; }
  int height() const { return height_; }

  /// Copies the damaged pixels of view into the current frame. A null or empty
  /// damage list means the whole view changed. Rectangles are in desktop
  /// coordinates and are clipped to the view.
  void update(const FrameView& view, const std::vector<DamageRect>* damage);

  /// True when some damage has not been sent yet.
  bool dirty() const;

  /// Builds one type 8 payload. A keyframe resends everything onto a black
  /// buffer. Returns false, and leaves out empty, when nothing changed.
  bool encode(bool keyframe, uint64_t pts_us, std::vector<uint8_t>& out, Stats* stats = nullptr);


 private:
  struct Rect { int x, y, w, h; };
  struct Op { OpKind kind; Rect r; int sx, sy; uint8_t rgb[3]; };
  void markDirty(int x, int y, int w, int h);
  int detectScroll(const Rect& area, std::vector<Op>& ops);
  void diffTiles(std::vector<Op>& ops);
  bool solid(const Rect& r, uint8_t rgb[3]) const;
  void applyCopy(const Op& op);
  void commit(const Op& op);
  uint64_t rowHash(const std::vector<uint8_t>& image, int y, int x0, int x1) const;
  void toRgb(const uint8_t* bgra_or_argb, uint8_t* rgb, int pixels) const;

  int width_, height_, level_;
  size_t max_payload_;
  int tiles_x_, tiles_y_;
  // cur_ and sent_ hold 4-byte pixels in the capture format; the alpha byte is not compared.
  std::vector<uint8_t> cur_, sent_, dirty_;
  std::vector<uint8_t> clean_;  // rows a scroll copy made equal, within [cleanX0_, cleanX1_)
  int cleanX0_ = 0, cleanX1_ = 0;
  // Row hashes of sent_ over [hashX0_, hashX1_), kept from the last frame.
  std::vector<uint64_t> sentHash_, curHash_;
  std::vector<uint8_t> hashOk_;
  int hashX0_ = -1, hashX1_ = -1;
  Rect hashArea_{0, 0, 0, 0};
  PixelFormat format_ = PixelFormat::BGRA;
  bool formatKnown_ = false;
  std::vector<uint8_t> raw_, blob_;
  void* cctx_ = nullptr;
};

/// Applies one type 8 payload to an RGB buffer. A keyframe resizes and clears
/// the buffer first. Returns false and sets error on a malformed payload; the
/// buffer may then be partly changed and the client should ask for a keyframe.
bool apply(std::vector<uint8_t>& rgb, int& width, int& height, const uint8_t* payload, size_t size,
           std::string* error = nullptr);

}  // namespace relay::tiles
