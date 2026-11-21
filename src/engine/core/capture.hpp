#pragma once

#include "capture-source.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace relay {
struct EncodedFrame {
  uint32_t generation = 0;
  uint32_t sequence = 0;
  uint64_t pts_us = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool keyframe = false;
  /// NSL1 message type: 1 carries an H.264 access unit in annex_b, 8 a tiles payload (see tiles.hpp).
  uint8_t type = 1;
  std::vector<uint8_t> annex_b;
};

struct CaptureStats {
  bool cursor_supported = false;
  uint64_t captures = 0;
  uint64_t encoded = 0;
  uint64_t capture_ns = 0;
  uint64_t convert_ns = 0;
  uint64_t encode_ns = 0;
};

struct CaptureSettings {
  unsigned fps;
  uint64_t min_bitrate, start_bitrate, max_bitrate;
};

class CaptureEncoder {
 public:
  using Publish = std::function<bool(EncodedFrame&&)>;
  using PublishCursor = std::function<void(uint32_t, CursorImage&&)>;
  explicit CaptureEncoder(Publish publish, CaptureSettings settings,
                          CaptureSourceFactory factory,
                          PublishCursor cursor = {});
  ~CaptureEncoder();
  CaptureEncoder(const CaptureEncoder&) = delete;
  CaptureEncoder& operator=(const CaptureEncoder&) = delete;
  bool start(std::string& error);
  /** Starts a session; tiles selects the lossless tiles codec in place of H.264. */
  void beginSession(uint32_t generation, bool tiles = false, unsigned tiles_inflight = 1);
  /** Makes the next tiles frame of this session a keyframe. */
  void requestKeyframe(uint32_t generation);
  void endSession(uint32_t generation);
  void acknowledge(uint32_t generation, uint32_t sequence);
  void setFlowControl(uint32_t generation, unsigned window, uint64_t bitrate, bool paused, uint64_t admit_until_us);
  CaptureStats stats() const;
  std::string error() const;
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace relay
