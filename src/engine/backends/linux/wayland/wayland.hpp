#pragma once

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "core/output.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace relay::wayland {
struct OutputInfo;

inline void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

inline FileDescriptor createMemfd(const char* name) {
  return FileDescriptor(::memfd_create(name, MFD_CLOEXEC));
}

struct Connection {
  wl_display* display = nullptr;
  wl_shm* shm = nullptr;
  wl_output* output = nullptr;
  int width = 0;
  int height = 0;
  zwlr_screencopy_manager_v1* screencopyManager = nullptr;
  zwp_virtual_keyboard_manager_v1* virtualKeyboardManager = nullptr;
  zwlr_virtual_pointer_manager_v1* virtualPointerManager = nullptr;
  zxdg_output_manager_v1* xdgOutputManager = nullptr;
  ext_image_copy_capture_manager_v1* imageCopyManager = nullptr;
  ext_output_image_capture_source_manager_v1* outputSourceManager = nullptr;
  /// Name of the bound output.
  std::string outputName;

  /// Binds the output chosen with --output, or the first one.
  Connection();
  /// Binds the named output; empty means the one Connection() binds.
  explicit Connection(const std::string& output);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  /// Binds another output by name (empty is the default) on this connection; false keeps the current one.
  bool rebind(const std::string& output);
  /// Every output with its layout position and scale; the bound one is primary.
  std::vector<OutputDesc> outputs();

 private:
  wl_registry* registry_ = nullptr;
  std::unique_ptr<OutputInfo> outputInfo_;
  void initialize(const std::string& output);
  void cleanup();
};

bool initializeNativeBackend(const std::string& output);
std::vector<OutputDesc> listOutputs();
}
