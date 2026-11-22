#pragma once

#include "core/capture-source.hpp"
#include "core/clipboard.hpp"
#include "core/input.hpp"
#include "core/output.hpp"
#include <memory>
#include <string>
#include <vector>

namespace relay {
struct Backend {
  const char* name;
  bool (*initialize)(const std::string& output);
  std::unique_ptr<CaptureSource> (*capture)();
  std::unique_ptr<InputBackend> (*input)();
  /// The desktop clipboard, or null when this desktop has none the engine can use.
  std::unique_ptr<ClipboardBackend> (*clipboard)(ClipboardChanged changed);
  /// Captures the named output (empty is the default one), or null when the backend has one output only.
  std::unique_ptr<CaptureSource> (*captureOutput)(const std::string& output);
  /// The outputs captureOutput accepts, or null with captureOutput.
  std::vector<OutputDesc> (*outputs)();
};
const Backend& defaultBackend();
const Backend& backendFor(const std::string& name);
std::string backendNames();
}
