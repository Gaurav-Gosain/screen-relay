#include "wayland.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace relay::wayland {
struct OutputInfo {
  wl_output* proxy = nullptr;
  std::string name;
  uint32_t version = 0;
  int width = 0;
  int height = 0;
  int x = 0, y = 0, transform = 0, scale = 1;
  // From xdg-output, when the compositor has it.
  int logicalX = 0, logicalY = 0, logicalWidth = 0, logicalHeight = 0;
  bool logical = false;

  ~OutputInfo() {
    if (proxy) wl_output_destroy(proxy);
  }
};

namespace {
std::string selectedOutputName;

struct RegistryState {
  Connection* connection;
  std::vector<std::unique_ptr<OutputInfo>> outputs;
  bool outputListenerFailed = false;
};

void outputGeometry(void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                    const char*, const char*, int32_t transform) {
  auto* output = static_cast<OutputInfo*>(data);
  output->x = x; output->y = y; output->transform = transform;
}
void outputNoop(void*, wl_output*) {}
void outputScale(void* data, wl_output*, int32_t scale) { static_cast<OutputInfo*>(data)->scale = scale; }
void outputNoop(void*, wl_output*, const char*) {}

void outputMode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t) {
  auto* output = static_cast<OutputInfo*>(data);
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    output->width = width;
    output->height = height;
  }
}

void outputName(void* data, wl_output*, const char* name) {
  static_cast<OutputInfo*>(data)->name = name ? name : "";
}

const wl_output_listener outputListener = {
  outputGeometry, outputMode, outputNoop, outputScale, outputName, outputNoop
};

void xdgPosition(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
  auto* output = static_cast<OutputInfo*>(data);
  output->logicalX = x; output->logicalY = y; output->logical = true;
}
void xdgSize(void* data, zxdg_output_v1*, int32_t width, int32_t height) {
  auto* output = static_cast<OutputInfo*>(data);
  output->logicalWidth = width; output->logicalHeight = height; output->logical = true;
}
void xdgNoop(void*, zxdg_output_v1*) {}
void xdgNoop(void*, zxdg_output_v1*, const char*) {}
const zxdg_output_v1_listener xdgListener = {xdgPosition, xdgSize, xdgNoop, xdgNoop, xdgNoop};

void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
  auto* state = static_cast<RegistryState*>(data);
  auto& connection = *state->connection;
  if (state->outputListenerFailed) return;
  if (std::strcmp(interface, wl_shm_interface.name) == 0 && !connection.shm) {
    connection.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, std::min(version, 1u)));
  } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
    auto output = std::make_unique<OutputInfo>();
    output->version = version;
    output->proxy = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4u)));
    if (wl_output_add_listener(output->proxy, &outputListener, output.get()) < 0) {
      state->outputListenerFailed = true;
      return;
    }
    state->outputs.push_back(std::move(output));
  } else if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0 && !connection.screencopyManager) {
    connection.screencopyManager = static_cast<zwlr_screencopy_manager_v1*>(wl_registry_bind(
      registry, name, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
  } else if (std::strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0 && !connection.virtualKeyboardManager) {
    connection.virtualKeyboardManager = static_cast<zwp_virtual_keyboard_manager_v1*>(wl_registry_bind(
      registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
  } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 &&
             version >= 2 && !connection.virtualPointerManager) {
    connection.virtualPointerManager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(
      registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2));
  } else if (std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0 && !connection.imageCopyManager) {
    connection.imageCopyManager = static_cast<ext_image_copy_capture_manager_v1*>(wl_registry_bind(
      registry, name, &ext_image_copy_capture_manager_v1_interface, 1));
  } else if (std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0 &&
             !connection.outputSourceManager) {
    connection.outputSourceManager = static_cast<ext_output_image_capture_source_manager_v1*>(wl_registry_bind(
      registry, name, &ext_output_image_capture_source_manager_v1_interface, 1));
  } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0 && !connection.xdgOutputManager) {
    connection.xdgOutputManager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(
      registry, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
  }
}

void globalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener registryListener = {global, globalRemove};

std::string availableOutputs(const std::vector<std::unique_ptr<OutputInfo>>& outputs) {
  std::string names;
  for (const auto& output : outputs) {
    if (!names.empty()) names += ", ";
    names += output->name.empty() ? "<unnamed>" : output->name;
  }
  return names.empty() ? "(none)" : names;
}

/// Reads every wl_output global on display, with the globals Connection binds once.
std::vector<std::unique_ptr<OutputInfo>> enumerate(Connection& connection, wl_registry*& registry) {
  registry = wl_display_get_registry(connection.display);
  if (!registry) throw std::runtime_error("Could not get Wayland registry");
  RegistryState state{&connection, {}, false};
  if (wl_registry_add_listener(registry, &registryListener, &state) < 0)
    throw std::runtime_error("Could not listen to Wayland registry");
  if (wl_display_roundtrip(connection.display) < 0) throw std::runtime_error("Wayland registry roundtrip failed");
  if (state.outputListenerFailed) throw std::runtime_error("Could not listen to wl_output");
  if (wl_display_roundtrip(connection.display) < 0) throw std::runtime_error("Wayland output roundtrip failed");
  if (state.outputListenerFailed) throw std::runtime_error("Could not listen to wl_output");
  wl_registry_destroy(registry);
  registry = nullptr;
  return std::move(state.outputs);
}

/// The output a name selects: empty is --output, or else the first output.
std::vector<std::unique_ptr<OutputInfo>>::iterator find(std::vector<std::unique_ptr<OutputInfo>>& outputs,
                                                         const std::string& wanted) {
  const std::string& name = wanted.empty() ? selectedOutputName : wanted;
  if (name.empty()) return outputs.begin();
  return std::find_if(outputs.begin(), outputs.end(), [&](const auto& output) { return output->name == name; });
}
}

Connection::Connection() : Connection(std::string()) {}

Connection::Connection(const std::string& output) {
  display = wl_display_connect(nullptr);
  if (!display) {
    const char* name = std::getenv("WAYLAND_DISPLAY");
    throw std::runtime_error("Cannot connect to Wayland display '" + std::string(name ? name : "wayland-0") +
                             "'; check WAYLAND_DISPLAY and XDG_RUNTIME_DIR");
  }
  try {
    initialize(output);
  } catch (...) {
    cleanup();
    throw;
  }
}

Connection::~Connection() { cleanup(); }

void Connection::initialize(const std::string& wanted) {
  auto outputs = enumerate(*this, registry_);
  auto selected = find(outputs, wanted);
  if (selected == outputs.end()) {
    if (outputs.empty()) throw std::runtime_error("No Wayland outputs available; available outputs: (none)");
    const std::string& name = wanted.empty() ? selectedOutputName : wanted;
    std::string message = "Unknown Wayland output '" + name + "'; available outputs: " + availableOutputs(outputs);
    if (std::any_of(outputs.begin(), outputs.end(), [](const auto& output) { return output->version < 4; }))
      message += "; output names require wl_output version 4";
    throw std::runtime_error(message);
  }
  outputInfo_ = std::move(*selected);
  output = outputInfo_->proxy;
  outputName = outputInfo_->name;
  width = outputInfo_->width;
  height = outputInfo_->height;
}

bool Connection::rebind(const std::string& wanted) {
  auto outputs = enumerate(*this, registry_);
  auto selected = find(outputs, wanted);
  if (selected == outputs.end()) return false;
  outputInfo_ = std::move(*selected);
  output = outputInfo_->proxy;
  outputName = outputInfo_->name;
  width = outputInfo_->width;
  height = outputInfo_->height;
  return true;
}

std::vector<OutputDesc> Connection::outputs() {
  auto outputs = enumerate(*this, registry_);
  std::vector<zxdg_output_v1*> extras;
  if (xdgOutputManager) {
    for (auto& info : outputs) {
      auto* extra = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager, info->proxy);
      if (extra && zxdg_output_v1_add_listener(extra, &xdgListener, info.get()) == 0) extras.push_back(extra);
      else if (extra) zxdg_output_v1_destroy(extra);
    }
    const bool ok = wl_display_roundtrip(display) >= 0;
    for (auto* extra : extras) zxdg_output_v1_destroy(extra);
    if (!ok) throw std::runtime_error("Wayland xdg-output roundtrip failed");
  }
  std::vector<OutputDesc> result;
  for (const auto& info : outputs) {
    OutputDesc desc;
    desc.name = info->name;
    desc.width = info->width; desc.height = info->height;
    desc.x = info->logical ? info->logicalX : info->x;
    desc.y = info->logical ? info->logicalY : info->y;
    // A 90 or 270 degree transform swaps the logical axes against the mode.
    const bool turned = info->transform & 1;
    const int across = turned ? info->height : info->width;
    desc.scale = info->logical && info->logicalWidth > 0 ? double(across) / info->logicalWidth : double(info->scale);
    desc.scale = std::round(desc.scale * 1000) / 1000;
    desc.primary = info->name == outputName;
    result.push_back(std::move(desc));
  }
  return result;
}

void Connection::cleanup() {
  if (outputSourceManager) ext_output_image_capture_source_manager_v1_destroy(outputSourceManager);
  if (imageCopyManager) ext_image_copy_capture_manager_v1_destroy(imageCopyManager);
  if (xdgOutputManager) zxdg_output_manager_v1_destroy(xdgOutputManager);
  if (virtualPointerManager) zwlr_virtual_pointer_manager_v1_destroy(virtualPointerManager);
  if (virtualKeyboardManager) zwp_virtual_keyboard_manager_v1_destroy(virtualKeyboardManager);
  if (screencopyManager) zwlr_screencopy_manager_v1_destroy(screencopyManager);
  if (shm) wl_shm_destroy(shm);
  if (registry_) wl_registry_destroy(registry_);
  outputInfo_.reset();
  if (display) wl_display_disconnect(display);
  xdgOutputManager = nullptr;
  imageCopyManager = nullptr;
  outputSourceManager = nullptr;
  virtualPointerManager = nullptr;
  virtualKeyboardManager = nullptr;
  screencopyManager = nullptr;
  shm = nullptr;
  registry_ = nullptr;
  output = nullptr;
  display = nullptr;
}

bool initializeNativeBackend(const std::string& output) {
  selectedOutputName = output;
  return true;
}

std::vector<OutputDesc> listOutputs() {
  Connection connection;
  return connection.outputs();
}
}
