#include "wayland.hpp"
#include "core/bands.hpp"
#include "core/capture-source.hpp"
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace relay::wayland {
namespace {
constexpr size_t max_damage_rects = 64;
void checkSize(int width, int height) {
  try {
    requireSurfaceSize(width, height);
  } catch (const std::runtime_error& error) {
    throw std::runtime_error(std::string(error.what()) +
      "; create a headless output of a supported size (for example, swaymsg create_output or "
      "hyprctl output create headless <name>) and select it with --output");
  }
}

struct Buffer {
  wl_buffer* proxy = nullptr;
  uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t format = 0;
  int width = 0, height = 0, stride = 0;

  ~Buffer() {
    if (proxy) wl_buffer_destroy(proxy);
    if (data) munmap(data, size);
  }

  void init(wl_shm* shm, uint32_t offeredFormat, int offeredWidth, int offeredHeight, int offeredStride) {
    format = offeredFormat; width = offeredWidth; height = offeredHeight; stride = offeredStride;
    size = size_t(stride) * size_t(height);
    FileDescriptor fd = createMemfd("relay-wayland-capture");
    require(fd.get() >= 0, "creating Wayland shared memory failed");
    require(ftruncate(fd.get(), static_cast<off_t>(size)) == 0, "sizing Wayland shared memory failed");
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    require(mapping != MAP_FAILED, "mapping Wayland shared memory failed");
    data = static_cast<uint8_t*>(mapping);
    std::unique_ptr<wl_shm_pool, decltype(&wl_shm_pool_destroy)> pool(
      wl_shm_create_pool(shm, fd.get(), static_cast<int32_t>(size)), &wl_shm_pool_destroy);
    require(bool(pool), "creating Wayland shared memory pool failed");
    proxy = wl_shm_pool_create_buffer(pool.get(), 0, width, height, stride, format);
    require(proxy, "creating Wayland shared memory buffer failed");
  }

  void orient(bool inverted) {
    if (inverted) {
      for (int row = 0; row < height / 2; ++row)
        std::swap_ranges(data + size_t(row) * stride, data + size_t(row + 1) * stride,
                         data + size_t(height - row - 1) * stride);
    }
  }

  FrameView view() const { return {data, stride, width, height, PixelFormat::BGRA}; }
};

struct Frame {
  zwlr_screencopy_frame_v1* proxy = nullptr;
  ext_image_copy_capture_frame_v1* image = nullptr;  // ext-image-copy-capture in place of proxy
  int target = 0;
  bool damageCopy = false, hasShm = false, hasDmabuf = false;
  bool catchUp = false;  // asked with buffer damage after a set-aside frame, so it is never set aside
  bool bufferDone = false, copied = false, ready = false, failed = false;
  uint32_t format = 0, width = 0, height = 0, stride = 0, dmabufFormat = 0, flags = 0;
  std::vector<DamageRect> damage;
  std::exception_ptr error;

  ~Frame() {
    if (proxy) zwlr_screencopy_frame_v1_destroy(proxy);
    if (image) ext_image_copy_capture_frame_v1_destroy(image);
  }
};

void frameBuffer(void* data, zwlr_screencopy_frame_v1*, uint32_t format,
                 uint32_t width, uint32_t height, uint32_t stride) {
  auto& frame = *static_cast<Frame*>(data);
  frame.hasShm = true; frame.format = format; frame.width = width; frame.height = height; frame.stride = stride;
}
void frameFlags(void* data, zwlr_screencopy_frame_v1*, uint32_t flags) {
  static_cast<Frame*>(data)->flags = flags;
}
void frameReady(void* data, zwlr_screencopy_frame_v1*, uint32_t, uint32_t, uint32_t) {
  static_cast<Frame*>(data)->ready = true;
}
void frameFailed(void* data, zwlr_screencopy_frame_v1*) {
  static_cast<Frame*>(data)->failed = true;
}
void frameDamage(void* data, zwlr_screencopy_frame_v1*, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height) {
  auto& frame = *static_cast<Frame*>(data);
  if (x >= frame.width || y >= frame.height) return;
  width = std::min(width, frame.width - x);
  height = std::min(height, frame.height - y);
  if (!width || !height) return;
  try {
    frame.damage.push_back({static_cast<int>(x), static_cast<int>(y),
                            static_cast<int>(width), static_cast<int>(height)});
    if (frame.damage.size() > max_damage_rects)
      frame.damage = {{0, 0, static_cast<int>(frame.width), static_cast<int>(frame.height)}};
  } catch (...) {
    frame.error = std::current_exception();
  }
}
void frameDmabuf(void* data, zwlr_screencopy_frame_v1*, uint32_t format, uint32_t, uint32_t) {
  auto& frame = *static_cast<Frame*>(data);
  frame.hasDmabuf = true; frame.dmabufFormat = format;
}
void frameBufferDone(void* data, zwlr_screencopy_frame_v1*) {
  static_cast<Frame*>(data)->bufferDone = true;
}

const zwlr_screencopy_frame_v1_listener frameListener = {
  frameBuffer, frameFlags, frameReady, frameFailed, frameDamage, frameDmabuf, frameBufferDone
};

// ext-image-copy-capture: one session for the output; each frame copies into a buffer the source owns.
struct Session {
  ext_image_capture_source_v1* source = nullptr;
  ext_image_copy_capture_session_v1* proxy = nullptr;
  uint32_t width = 0, height = 0, format = 0;
  bool hasFormat = false, done = false, stopped = false;
  ~Session() {
    if (proxy) ext_image_copy_capture_session_v1_destroy(proxy);
    if (source) ext_image_capture_source_v1_destroy(source);
  }
};
void sessionSize(void* data, ext_image_copy_capture_session_v1*, uint32_t width, uint32_t height) {
  auto& session = *static_cast<Session*>(data);
  session.width = width; session.height = height;
}
void sessionShm(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
  auto& session = *static_cast<Session*>(data);
  if (format == WL_SHM_FORMAT_XRGB8888 || (format == WL_SHM_FORMAT_ARGB8888 && !session.hasFormat)) {
    session.format = format; session.hasFormat = true;
  }
}
void sessionDevice(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
void sessionDmabuf(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
void sessionDone(void* data, ext_image_copy_capture_session_v1*) { static_cast<Session*>(data)->done = true; }
void sessionStopped(void* data, ext_image_copy_capture_session_v1*) { static_cast<Session*>(data)->stopped = true; }
const ext_image_copy_capture_session_v1_listener sessionListener = {
  sessionSize, sessionShm, sessionDevice, sessionDmabuf, sessionDone, sessionStopped
};
void imageTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
void imageDamage(void* data, ext_image_copy_capture_frame_v1*, int32_t x, int32_t y, int32_t width, int32_t height) {
  if (x < 0 || y < 0 || width <= 0 || height <= 0) return;
  frameDamage(data, nullptr, uint32_t(x), uint32_t(y), uint32_t(width), uint32_t(height));
}
void imageTime(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
void imageReady(void* data, ext_image_copy_capture_frame_v1*) { static_cast<Frame*>(data)->ready = true; }
void imageFailed(void* data, ext_image_copy_capture_frame_v1*, uint32_t) { static_cast<Frame*>(data)->failed = true; }
const ext_image_copy_capture_frame_v1_listener imageListener = {
  imageTransform, imageDamage, imageTime, imageReady, imageFailed
};

class WaylandCaptureSource final : public CaptureSource {
  Connection connection;
  std::array<std::unique_ptr<Buffer>, 3> buffers;
  std::unique_ptr<Frame> frame;
  std::unique_ptr<Session> session;  // set when the compositor has ext-image-copy-capture
  std::array<bool, 3> fresh{};        // the buffer was never copied into, so the whole of it is damage
  // Hyprland renders some outputs at the refresh rate without a pixel change. A frame that comes
  // sooner than 1/fps after the last compare is set aside; at the end of the interval the source asks
  // again with buffer damage, which the compositor answers at once with the latest picture.
  using Clock = std::chrono::steady_clock;
  Clock::time_point lastCompare{}, setAsideUntil{};
  int setAside = -1;  // buffer of the set-aside frame, -1 for none
  int phase = 0;      // rows compared first: y % 8 == phase, a new phase each time
  std::vector<DamageRect> damage;
  const int width, height;
  int current = -1;
  /// Buffer the next damage request copies into, sent once the encoder took the current frame. -1: none waits.
  int deferred = -1;
  unsigned failures = 0;

  Clock::duration interval() const {
    const unsigned fps = capture_frame_rate.load();
    return std::chrono::nanoseconds(1000000000LL / (fps ? fps : 60));
  }

  void request(int target, bool damageCopy, bool catchUp = false) {
    auto next = std::make_unique<Frame>();
    next->target = target; next->damageCopy = damageCopy; next->catchUp = catchUp;
    if (session) {
      // ext-image-copy-capture answers when the output renders new content. A frame with buffer
      // damage is answered at once, so a full capture and the first use of a buffer damage all of it.
      auto& buffer = buffers[target];
      if (!buffer) {
        buffer = std::make_unique<Buffer>();
        buffer->init(connection.shm, session->format, width, height, width * 4);
        fresh[target] = true;
      }
      next->width = uint32_t(width); next->height = uint32_t(height);
      next->image = ext_image_copy_capture_session_v1_create_frame(session->proxy);
      require(next->image, "requesting Wayland image copy frame failed");
      require(ext_image_copy_capture_frame_v1_add_listener(next->image, &imageListener, next.get()) == 0,
              "listening for Wayland image copy frame failed");
      ext_image_copy_capture_frame_v1_attach_buffer(next->image, buffer->proxy);
      if (!damageCopy || fresh[target] || catchUp) ext_image_copy_capture_frame_v1_damage_buffer(next->image, 0, 0, width, height);
      fresh[target] = false;
      ext_image_copy_capture_frame_v1_capture(next->image);
      next->copied = true;
      frame = std::move(next);
      return;
    }
    next->proxy = zwlr_screencopy_manager_v1_capture_output(connection.screencopyManager, 0, connection.output);
    require(next->proxy, "requesting Wayland screencopy frame failed");
    require(zwlr_screencopy_frame_v1_add_listener(next->proxy, &frameListener, next.get()) == 0,
            "listening for Wayland screencopy frame failed");
    frame = std::move(next);
  }

  void copy() {
    auto& pending = *frame;
    if (!pending.hasShm || (pending.format != WL_SHM_FORMAT_XRGB8888 && pending.format != WL_SHM_FORMAT_ARGB8888)) {
      const std::string shm = pending.hasShm ? std::to_string(pending.format) : "none";
      const std::string dmabuf = pending.hasDmabuf ? std::to_string(pending.dmabufFormat) : "none";
      throw std::runtime_error("Unsupported Wayland screencopy formats (wl_shm: " + shm +
                               ", linux-dmabuf: " + dmabuf + ")");
    }
    if (pending.width > unsigned(std::numeric_limits<int>::max()) ||
        pending.height > unsigned(std::numeric_limits<int>::max()))
      throw std::runtime_error("Unsupported Wayland screencopy dimensions");
    const int frameWidth = static_cast<int>(pending.width), frameHeight = static_cast<int>(pending.height);
    checkSize(frameWidth, frameHeight);
    require(pending.stride >= pending.width * 4 && pending.stride <= pending.width * 8,
            "Unsupported Wayland screencopy stride");
    auto& buffer = buffers[pending.target];
    if (!buffer || buffer->format != pending.format || buffer->stride != static_cast<int>(pending.stride)) {
      auto replacement = std::make_unique<Buffer>();
      replacement->init(connection.shm, pending.format, frameWidth, frameHeight, static_cast<int>(pending.stride));
      buffer = std::move(replacement);
    }
    if (pending.damageCopy) zwlr_screencopy_frame_v1_copy_with_damage(pending.proxy, buffer->proxy);
    else zwlr_screencopy_frame_v1_copy(pending.proxy, buffer->proxy);
    pending.copied = true;
  }

  bool process() {
    if (session && session->stopped) throw std::runtime_error("Wayland image copy session stopped");
    if (!frame) return false;
    if (frame->hasShm && (frame->width != static_cast<uint32_t>(width) ||
                          frame->height != static_cast<uint32_t>(height)))
      throw std::runtime_error("Wayland output size changed from " + std::to_string(width) + "x" +
                               std::to_string(height) + " to " + std::to_string(frame->width) + "x" +
                               std::to_string(frame->height) +
                               "; restart relay and, in attach mode, keep the output mode fixed");
    if (frame->error) std::rethrow_exception(frame->error);
    if (frame->failed) {
      const int target = frame->target;
      const bool damageCopy = frame->damageCopy;
      frame.reset();
      if (!damageCopy) throw std::runtime_error("Wayland screencopy failed");
      // A failed image copy frame can mean new buffer constraints; the session then says so.
      if (session) fresh[target] = true;
      if (++failures >= 3) throw std::runtime_error("Wayland screencopy failed three consecutive times");
      request(target, true);
      return false;
    }
    if (frame->ready) {
      const int target = frame->target;
      const bool damageCopy = frame->damageCopy;
      auto& buffer = *buffers[target];
      buffer.orient(frame->flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
      failures = 0;
      if (damageCopy && session && !frame->catchUp && Clock::now() - lastCompare < interval()) {
        frame.reset();
        setAside = target;
        setAsideUntil = lastCompare + interval();
        return false;
      }
      if (damageCopy) {
        // Hyprland reports the whole output on every frame. Then compare with the previous frame,
        // which the other buffer holds, so the encoder reads only what changed.
        auto& before = buffers[1 - target];
        if (wholeOutput(frame->damage) && current == 1 - target && before && before->format == buffer.format &&
            before->stride == buffer.stride && before->height == buffer.height) {
          frame->damage.clear();
          // Every 8th row first, against the last frame sent: a change shows within 8 frames even
          // when it is too small to hit these rows, then the full compare gives the exact boxes.
          phase = (phase + 1) % 8;
          if (sampledRowsDiffer(buffer.data, before->data, buffer.width, buffer.height, buffer.stride, phase, 8))
            changedBands(buffer.data, before->data, buffer.width, buffer.height, buffer.stride, frame->damage);
          lastCompare = Clock::now();
          if (frame->damage.empty()) {
            // Not a pixel changed. Keep the current frame and ask again into the same buffer.
            frame.reset();
            request(target, true);
            return false;
          }
        }
        damage.insert(damage.end(), frame->damage.begin(), frame->damage.end());
        if (damage.size() > max_damage_rects) damage = {{0, 0, width, height}};
        current = target;
      }
      frame.reset();
      // Some compositors (Hyprland) answer every request at the output refresh rate with the whole
      // output as damage. Ask again only after the encoder took this frame, so captures follow --fps.
      if (damageCopy) deferred = 1 - target;
      return damageCopy;
    }
    if (frame->bufferDone && !frame->copied) copy();
    return false;
  }

  bool wholeOutput(const std::vector<DamageRect>& rects) const {
    int64_t area = 0;
    for (const auto& r : rects) area += int64_t(r.width) * r.height;
    return area * 10 >= int64_t(width) * height * 9;
  }

  void flush() {
    require(wl_display_flush(connection.display) >= 0, "flushing Wayland requests failed");
  }

  bool dispatch(int timeout) {
    if (setAside >= 0 && !frame && Clock::now() >= setAsideUntil) {
      const int target = setAside;
      setAside = -1;
      request(target, true, true);
      flush();
    }
    const bool hadFrame = bool(frame);
    require(wl_display_dispatch_pending(connection.display) >= 0, "dispatching Wayland events failed");
    bool dirty = process();
    if (dirty || (hadFrame && !frame)) { flush(); return dirty; }
    while (wl_display_prepare_read(connection.display) != 0) {
      require(wl_display_dispatch_pending(connection.display) >= 0, "dispatching Wayland events failed");
      dirty = process() || dirty;
      if (dirty || (hadFrame && !frame)) { flush(); return dirty; }
    }
    if (wl_display_flush(connection.display) < 0) {
      wl_display_cancel_read(connection.display);
      throw std::runtime_error("flushing Wayland requests failed");
    }
    pollfd descriptor{fd(), POLLIN, 0};
    const int result = poll(&descriptor, 1, timeout);
    const int pollError = errno;
    if (result <= 0 || !(descriptor.revents & POLLIN)) {
      wl_display_cancel_read(connection.display);
      if (result < 0 && pollError != EINTR) throw std::runtime_error("polling Wayland display failed");
      if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
        throw std::runtime_error("Wayland display connection closed");
      return dirty;
    }
    require(wl_display_read_events(connection.display) >= 0, "reading Wayland events failed");
    require(wl_display_dispatch_pending(connection.display) >= 0, "dispatching Wayland events failed");
    dirty = process() || dirty;
    flush();
    return dirty;
  }

  void captureBlocking(int target) {
    require(!frame, "Wayland screencopy frame already in flight");
    request(target, false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (frame) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        frame.reset();
        throw std::runtime_error("Wayland screencopy timed out");
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      dispatch(static_cast<int>(remaining));
    }
  }

 public:
  explicit WaylandCaptureSource(const std::string& output)
      : connection(output), width(connection.width), height(connection.height) {
    require(connection.screencopyManager, "Wayland compositor does not offer zwlr_screencopy_manager_v1");
    require(wl_proxy_get_version(reinterpret_cast<wl_proxy*>(connection.screencopyManager)) >= 3,
            "Wayland compositor must offer zwlr_screencopy_manager_v1 version 3");
    require(connection.shm, "Wayland compositor does not offer wl_shm");
    checkSize(width, height);
    if (connection.imageCopyManager && connection.outputSourceManager) openSession();
  }

  /// Uses ext-image-copy-capture when the compositor has it. It waits for new content where
  /// wlr-screencopy on Hyprland answers at the refresh rate. Falls back to wlr-screencopy.
  void openSession() {
    auto next = std::make_unique<Session>();
    next->source = ext_output_image_capture_source_manager_v1_create_source(connection.outputSourceManager, connection.output);
    if (!next->source) return;
    next->proxy = ext_image_copy_capture_manager_v1_create_session(connection.imageCopyManager, next->source, 0);
    if (!next->proxy || ext_image_copy_capture_session_v1_add_listener(next->proxy, &sessionListener, next.get()) != 0) return;
    for (int i = 0; i < 4 && !next->done && !next->stopped; ++i)
      if (wl_display_roundtrip(connection.display) < 0) return;
    if (!next->done || next->stopped || !next->hasFormat || next->width != uint32_t(width) || next->height != uint32_t(height)) return;
    session = std::move(next);
  }

  int fd() const override { return wl_display_get_fd(connection.display); }
  std::pair<int, int> dimensions() const override { return {width, height}; }
  PixelFormat pixelFormat() const override { return PixelFormat::BGRA; }
  bool cursorSupported() const override { return false; }
  int64_t timeoutNs() const override {
    if (setAside < 0) return -1;
    return std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(setAsideUntil - Clock::now()).count());
  }
  bool drainDamage() override { return dispatch(0); }

  FrameView capture() override {
    captureBlocking(2);
    return buffers[2]->view();
  }

  FrameView captureChanged(bool) override {
    if (current < 0) {
      captureBlocking(0);
      current = 0;
      request(1, true);
      flush();
    } else if (!frame && deferred >= 0) {
      // The new request writes the other buffer, so the view returned here stays valid.
      request(deferred, true);
      deferred = -1;
      flush();
    }
    return buffers[current]->view();
  }

  bool changedRects(std::vector<DamageRect>& out) override {
    out = std::move(damage);
    damage.clear();
    return true;
  }
};
}

std::unique_ptr<CaptureSource> makeCaptureSource() {
  return std::make_unique<WaylandCaptureSource>(std::string());
}

std::unique_ptr<CaptureSource> makeOutputCaptureSource(const std::string& output) {
  return std::make_unique<WaylandCaptureSource>(output);
}
}
