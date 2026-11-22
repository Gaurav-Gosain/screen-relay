#include "core/capture-source.hpp"
#include "core/wait.hpp"
#include "backends/macos/display.hpp"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace relay {
namespace macos {
struct CaptureState;
}
}

@interface RelayCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
 @public
  std::weak_ptr<relay::macos::CaptureState> state;
}
@end

namespace relay {
namespace macos {
namespace {
constexpr size_t max_pending_rects = 256;

std::string errorMessage(NSError* error, const char* action) {
  if (error.code == SCStreamErrorUserDeclined)
    return "Screen Recording permission is missing. In System Settings > Privacy & Security > Screen & System Audio Recording, enable the app that started relay, then restart it";
  const char* description = error.localizedDescription.UTF8String;
  return std::string(action) + ": " + (description ? description : "unknown error") + " (" + std::to_string(error.code) + ")";
}

struct ContentResult {
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
  SCShareableContent* content = nil;
  NSError* error = nil;
};

struct StartResult {
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false, abandoned = false;
  NSError* error = nil;
};

class PixelLock {
 public:
  explicit PixelLock(CVPixelBufferRef value) : buffer(value) {
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
      throw std::runtime_error("Locking the screen capture image failed");
  }
  ~PixelLock() { unlock(); }
  void unlock() {
    if (buffer) { CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly); buffer = nullptr; }
  }
 private:
  CVPixelBufferRef buffer;
};

class MacCapture;
}

/// Holds the latest screen image, capture failure, and subscribed sources.
struct CaptureState {
  /// outWidth and outHeight, when nonzero, ask ScreenCaptureKit to scale the display to that size.
  explicit CaptureState(Display selected, int outWidth = 0, int outHeight = 0)
      : display(selected), outWidth(outWidth ? outWidth : selected.width), outHeight(outHeight ? outHeight : selected.height) {
    requireSurfaceSize(display.width, display.height);
    requireSurfaceSize(this->outWidth, this->outHeight);
    image.resize(size_t(this->outWidth) * this->outHeight * 4);
  }
  Display display;
  int outWidth, outHeight;
  std::mutex mutex;
  std::condition_variable arrived;
  std::vector<uint8_t> image;
  std::vector<MacCapture*> sources;
  bool haveFrame = false;
  /// True after a scaled frame reported dirty rects outside its own size, so they were mapped from display space.
  bool dirtyMapped = false;
  std::string failure;

  void fail(const std::string& message);
  void checkDisplay();
  void frame(CMSampleBufferRef sample);
  bool scaledDirtyRects(id value, std::vector<DamageRect>& rects);
};

namespace {
/// Shares one screen capture stream across capture sources.
class SharedCapture {
 public:
  /// One stream for each output size; 0 x 0 is the display size.
  static std::shared_ptr<SharedCapture> shared(int outWidth = 0, int outHeight = 0) {
    static std::mutex mutex;
    static std::map<std::pair<int, int>, std::weak_ptr<SharedCapture>> streams;
    std::lock_guard<std::mutex> lock(mutex);
    auto& current = streams[{outWidth, outHeight}];
    auto existing = current.lock();
    if (!existing || existing->failed()) {
      if (existing) existing->stop();
      existing = std::make_shared<SharedCapture>(outWidth, outHeight);
      existing->start();
      current = existing;
    }
    return existing;
  }

  SharedCapture(int outWidth, int outHeight) : state(std::make_shared<CaptureState>(findDisplay(), outWidth, outHeight)) {}
  ~SharedCapture() { stop(); }
  void stop() {
    @autoreleasepool {
      if (timer) { dispatch_source_cancel(timer); timer = nil; }
      if (started) { started = false; [stream stopCaptureWithCompletionHandler:nil]; }
    }
  }
  bool failed() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return !state->failure.empty();
  }
  void start() {
    @autoreleasepool {
      auto contentResult = std::make_shared<ContentResult>();
      [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content, NSError* error) {
          std::lock_guard<std::mutex> lock(contentResult->mutex);
          contentResult->content = content;
          contentResult->error = error;
          contentResult->done = true;
          contentResult->ready.notify_one();
        }];
      std::unique_lock<std::mutex> lock(contentResult->mutex);
      if (!contentResult->ready.wait_for(lock, std::chrono::seconds(10), [&] { return contentResult->done; }))
        throw std::runtime_error("Timed out listing displays for screen capture");
      if (contentResult->error) throw std::runtime_error(errorMessage(contentResult->error, "Listing displays for screen capture"));
      SCDisplay* selected = nil;
      for (SCDisplay* candidate in contentResult->content.displays)
        if (candidate.displayID == state->display.id) { selected = candidate; break; }
      if (!selected) throw std::runtime_error("The selected display is not available for screen capture");
      lock.unlock();
      SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:selected excludingWindows:@[]];
      SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
      config.width = state->outWidth;
      config.height = state->outHeight;
      config.pixelFormat = kCVPixelFormatType_32BGRA;
      config.colorSpaceName = kCGColorSpaceSRGB;
      config.showsCursor = NO;
      config.queueDepth = 3;
      // Deliver no more frames than the encoder sends: a 120 Hz display otherwise doubles the copies.
      const unsigned fps = capture_frame_rate.load();
      config.minimumFrameInterval = fps ? CMTimeMake(1, int32_t(fps)) : kCMTimeZero;
      output = [[RelayCaptureOutput alloc] init];
      output->state = state;
      queue = dispatch_queue_create("relay.macos.capture", DISPATCH_QUEUE_SERIAL);
      stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:output];
      NSError* error = nil;
      if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&error])
        throw std::runtime_error(errorMessage(error, "Adding screen capture output"));
      auto result = std::make_shared<StartResult>();
      SCStream* starting = stream;
      [stream startCaptureWithCompletionHandler:^(NSError* startError) {
        std::lock_guard<std::mutex> guard(result->mutex);
        if (result->abandoned) {
          if (!startError) [starting stopCaptureWithCompletionHandler:nil];
          return;
        }
        result->error = startError;
        result->done = true;
        result->ready.notify_one();
      }];
      std::unique_lock<std::mutex> startLock(result->mutex);
      if (!result->ready.wait_for(startLock, std::chrono::seconds(10), [&] { return result->done; })) {
        result->abandoned = true;
        throw std::runtime_error("Timed out starting screen capture");
      }
      if (result->error) throw std::runtime_error(errorMessage(result->error, "Starting screen capture"));
      started = true;
      timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
      std::weak_ptr<CaptureState> weak = state;
      dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
      dispatch_source_set_event_handler(timer, ^{ if (auto current = weak.lock()) current->checkDisplay(); });
      dispatch_resume(timer);
    }
  }
  std::shared_ptr<CaptureState> state;
 private:
  SCStream* stream = nil;
  RelayCaptureOutput* output = nil;
  dispatch_queue_t queue = nil;
  dispatch_source_t timer = nil;
  bool started = false;
};

/// Provides frames and damage notifications to one capture consumer.
class MacCapture final : public CaptureSource {
 public:
  MacCapture() : shared(SharedCapture::shared()), state(shared->state),
                 width(state->outWidth), height(state->outHeight), front(size_t(width) * height * 4) {
    if (!ready.valid()) throw std::runtime_error("Creating the capture pipe failed");
    ready.signal();
    subscribe(state);
  }
  /// Switches to a stream that ScreenCaptureKit scales to width x height (0 x 0 or the display size: unscaled).
  bool scaleOutput(int w, int h) override {
    const int fullWidth = state->display.width, fullHeight = state->display.height;
    if (!w || !h || (w >= fullWidth && h >= fullHeight)) { w = 0; h = 0; }
    if ((w ? w : fullWidth) == width && (h ? h : fullHeight) == height) return true;
    try {
      auto next = SharedCapture::shared(w, h);
      subscribe(next->state);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->sources.erase(std::remove(state->sources.begin(), state->sources.end(), this), state->sources.end());
      }
      std::lock_guard<std::mutex> lock(mutex);
      shared = std::move(next);
      state = shared->state;
      width = state->outWidth; height = state->outHeight;
      front.assign(size_t(width) * height * 4, 0);
      pending.clear(); last.clear();
      pendingFull = true; lastFull = true;
      ready.signal();
      return true;
    } catch (const std::exception& error) {
      std::fprintf(stderr, "capture: ScreenCaptureKit scaling to %dx%d failed: %s\n", w, h, error.what());
      return false;
    }
  }
  /// Adds this source to a capture state and waits for its first frame.
  void subscribe(const std::shared_ptr<CaptureState>& target) {
    std::unique_lock<std::mutex> lock(target->mutex);
    target->sources.push_back(this);
    if (!target->arrived.wait_for(lock, std::chrono::seconds(2), [&] { return target->haveFrame || !target->failure.empty(); })) {
      target->sources.erase(std::remove(target->sources.begin(), target->sources.end(), this), target->sources.end());
      throw std::runtime_error("Timed out waiting for the first screen capture frame");
    }
    if (!target->failure.empty()) {
      const std::string error = target->failure;
      target->sources.erase(std::remove(target->sources.begin(), target->sources.end(), this), target->sources.end());
      throw std::runtime_error(error);
    }
  }
  ~MacCapture() override {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->sources.erase(std::remove(state->sources.begin(), state->sources.end(), this), state->sources.end());
  }
  WaitHandle fd() const override { return ready.handle(); }
  std::pair<int, int> dimensions() const override { return {width, height}; }
  PixelFormat pixelFormat() const override { return PixelFormat::BGRA; }
  bool drainDamage() override {
    std::lock_guard<std::mutex> lock(mutex);
    ready.consume();
    return pendingFull || !pending.empty();
  }
  FrameView capture() override { return captureChanged(true); }
  FrameView captureChanged(bool full) override {
    std::vector<DamageRect> rects;
    {
      std::lock_guard<std::mutex> lock(mutex);
      lastFull = full || pendingFull;
      rects.swap(pending);
      pendingFull = false;
      last = lastFull ? std::vector<DamageRect>{} : rects;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->failure.empty()) throw std::runtime_error(state->failure);
    if (lastFull) std::memcpy(front.data(), state->image.data(), front.size());
    else for (const auto& rect : rects)
      for (int y = rect.y; y < rect.y + rect.height; ++y)
        std::memcpy(front.data() + (size_t(y) * width + rect.x) * 4,
                    state->image.data() + (size_t(y) * width + rect.x) * 4, size_t(rect.width) * 4);
    return {front.data(), width * 4, width, height, PixelFormat::BGRA};
  }
  bool changedRects(std::vector<DamageRect>& rects) override {
    std::lock_guard<std::mutex> lock(mutex);
    if (lastFull) return false;
    rects = last;
    return true;
  }
  void damaged(const std::vector<DamageRect>& rects, bool full) {
    std::lock_guard<std::mutex> lock(mutex);
    if (full || (!pendingFull && rects.size() > max_pending_rects - pending.size())) {
      pendingFull = true; pending.clear();
    } else if (!pendingFull) pending.insert(pending.end(), rects.begin(), rects.end());
    ready.signal();
  }
 private:
  std::shared_ptr<SharedCapture> shared;
  std::shared_ptr<CaptureState> state;
  int width, height;
  Wake ready;
  std::mutex mutex;
  std::vector<uint8_t> front;
  std::vector<DamageRect> pending, last;
  bool pendingFull = true, lastFull = true;
};

/// Converts ScreenCaptureKit dirty rectangles to clipped pixel damage, or returns false when unavailable.
bool dirtyRects(id value, int width, int height, std::vector<DamageRect>& rects) {
  if (![value isKindOfClass:[NSArray class]] || [value count] == 0) return false;
  for (id item in (NSArray*)value) {
    CGRect rect;
    if ([item isKindOfClass:[NSDictionary class]]) {
      if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)item, &rect)) return false;
    } else if ([item isKindOfClass:[NSValue class]] && std::strcmp([(NSValue*)item objCType], @encode(CGRect)) == 0) {
      [(NSValue*)item getValue:&rect size:sizeof(rect)];
    } else return false;
    const double x0 = CGRectGetMinX(rect), y0 = CGRectGetMinY(rect);
    const double x1 = CGRectGetMaxX(rect), y1 = CGRectGetMaxY(rect);
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return false;
    const int left = int(std::floor(std::clamp(x0, 0.0, double(width))));
    const int top = int(std::floor(std::clamp(y0, 0.0, double(height))));
    const int right = int(std::ceil(std::clamp(x1, 0.0, double(width))));
    const int bottom = int(std::ceil(std::clamp(y1, 0.0, double(height))));
    if (right > left && bottom > top) rects.push_back({left, top, right - left, bottom - top});
  }
  return !rects.empty();
}
}

/// Dirty rects of a scaled frame in output pixels. On the Mac they were measured in output space;
/// a rect past the output size means display space, so all rects are then scaled down and rounded outward.
bool CaptureState::scaledDirtyRects(id value, std::vector<DamageRect>& rects) {
  std::vector<DamageRect> raw;
  if (!dirtyRects(value, 1 << 20, 1 << 20, raw)) return false;
  int right = 0, bottom = 0;
  for (const auto& rect : raw) right = std::max(right, rect.x + rect.width), bottom = std::max(bottom, rect.y + rect.height);
  const bool mapped = right > outWidth || bottom > outHeight;
  if (mapped && !dirtyMapped) {
    dirtyMapped = true;
    std::fprintf(stderr, "capture: dirty rects reach %dx%d on a %dx%d frame; scaling them from %dx%d\n",
                 right, bottom, outWidth, outHeight, display.width, display.height);
  }
  const double sx = mapped ? double(outWidth) / display.width : 1, sy = mapped ? double(outHeight) / display.height : 1;
  for (const auto& rect : raw) {
    const int left = std::clamp(int(std::floor(rect.x * sx)), 0, outWidth);
    const int top = std::clamp(int(std::floor(rect.y * sy)), 0, outHeight);
    const int r = std::clamp(int(std::ceil((rect.x + rect.width) * sx)), 0, outWidth);
    const int b = std::clamp(int(std::ceil((rect.y + rect.height) * sy)), 0, outHeight);
    if (r > left && b > top) rects.push_back({left, top, r - left, b - top});
  }
  return !rects.empty();
}

void CaptureState::fail(const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!failure.empty()) return;
  failure = message;
  arrived.notify_all();
  for (auto* source : sources) source->damaged({}, true);
}

void CaptureState::checkDisplay() {
  try {
    const auto now = findDisplay();
    if (now.id != display.id || now.width != display.width || now.height != display.height ||
        !CGRectEqualToRect(now.bounds, display.bounds))
      fail("The display changed from " + std::to_string(display.width) + "x" + std::to_string(display.height) + " to " +
           std::to_string(now.width) + "x" + std::to_string(now.height) + " or moved; restart relay to stream it");
  } catch (const std::exception& error) { fail(error.what()); }
}

void CaptureState::frame(CMSampleBufferRef sample) {
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0) return;
  NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
  NSNumber* status = info[SCStreamFrameInfoStatus];
  if (!status || (status.integerValue != SCFrameStatusComplete && status.integerValue != SCFrameStatusStarted)) return;
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer || CVPixelBufferGetWidth(buffer) != size_t(outWidth) ||
      CVPixelBufferGetHeight(buffer) != size_t(outHeight) || CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA) {
    checkDisplay();
    throw std::runtime_error("Screen capture returned an invalid image");
  }
  // A complete frame with an empty dirty list changed nothing: do no work.
  id dirtyValue = info[SCStreamFrameInfoDirtyRects];
  if (haveFrame && [dirtyValue isKindOfClass:[NSArray class]] && [dirtyValue count] == 0) return;
  std::vector<DamageRect> rects;
  const bool scaled = outWidth != display.width || outHeight != display.height;
  bool full = !(scaled ? scaledDirtyRects(dirtyValue, rects) : dirtyRects(dirtyValue, outWidth, outHeight, rects));
  for (const auto& rect : rects)
    if (rect.x == 0 && rect.y == 0 && rect.width == outWidth && rect.height == outHeight) full = true;
  PixelLock pixelsLock(buffer);
  const auto* pixels = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  if (!pixels || stride < size_t(outWidth) * 4)
    throw std::runtime_error("Screen capture returned an invalid image stride");
  std::lock_guard<std::mutex> lock(mutex);
  if (!failure.empty()) return;
  full = full || !haveFrame;
  if (full) rects = {{0, 0, outWidth, outHeight}};
  for (const auto& rect : rects)
    for (int y = rect.y; y < rect.y + rect.height; ++y)
      std::memcpy(image.data() + (size_t(y) * outWidth + rect.x) * 4,
                  pixels + size_t(y) * stride + size_t(rect.x) * 4, size_t(rect.width) * 4);
  pixelsLock.unlock();
  haveFrame = true;
  arrived.notify_all();
  for (auto* source : sources) source->damaged(rects, full);
}

std::unique_ptr<CaptureSource> makeCaptureSource() { return std::make_unique<MacCapture>(); }
}
}

@implementation RelayCaptureOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
  (void)stream;
  if (type != SCStreamOutputTypeScreen) return;
  @autoreleasepool {
    if (auto current = state.lock()) {
      try { current->frame(sample); }
      catch (const std::exception& error) { current->fail(error.what()); }
    }
  }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  (void)stream;
  @autoreleasepool {
    if (auto current = state.lock()) {
      current->checkDisplay();
      current->fail(relay::macos::errorMessage(error, "Screen capture stopped"));
    }
  }
}
@end
