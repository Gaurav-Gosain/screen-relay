#pragma once

#include "core/capture.hpp"
#include "core/input.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace relay {
constexpr size_t max_input_batch = 256;
uint16_t u16(const uint8_t* p);
uint32_t u32(const uint8_t* p);
void put(uint8_t* p, uint64_t value, unsigned count);
InputEvent inputEvent(const uint8_t* p);
/** Validates an /input Content-Length header and returns 0 or the HTTP error status. */
unsigned inputLength(const std::string& text, uint64_t& length);
/** The WebSocket a request target opens. */
enum class Socket : uint8_t { none, watch, stream, take, control };
/** Classifies /stream?view=1, /stream, /stream?take=1, and /control; any other target is Socket::none. */
Socket viewerTarget(const std::string& target);
/** What opening a socket does to the sockets already open. */
struct ConnectionPlan {
  bool open = false, closeStream = false, closeControl = false;
};
constexpr size_t max_viewers = 4;
constexpr uint16_t close_taken = 4001;
constexpr uint16_t close_busy = 4003;
/// Plans a connection without changing the current input owner or any active stream.
ConnectionPlan connectionPlan(Socket socket, size_t viewerCount, bool streamOwnsInput, bool controlOpen);
/**
 * The builders below fill out with pre zero bytes of caller padding followed by one NSL1 message.
 * frameMessage and cursorMessage return false when the payload must not be sent.
 */
bool frameMessage(std::vector<uint8_t>& out, size_t pre, uint32_t generation, uint32_t lastSequence, const EncodedFrame& frame,
                  uint16_t width, uint16_t height);
bool cursorMessage(std::vector<uint8_t>& out, size_t pre, uint32_t generation, const CursorImage& cursor);
void controlReady(std::vector<uint8_t>& out, size_t pre, uint32_t generation, uint16_t width, uint16_t height);
void inputReply(std::vector<uint8_t>& out, size_t pre, uint32_t generation, uint32_t sequence, InputStatus status);
}  // namespace relay
