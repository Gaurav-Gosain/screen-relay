# screen-relay wire protocol

Use this reference when writing your own viewer or client.

## Input records

`POST /input` accepts 1 to 256 consecutive 16-byte records, not JSON. Send `Content-Length` and omit the 20-byte WebSocket header. All multibyte fields are big-endian. Set unused fields to zero.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 1 byte | Type | Event type from the table below. |
| 1 | 1 byte | Reserved | Zero. |
| 2 | 2 bytes | Code | Key, button, or wheel axis. |
| 4 | 4 bytes | Value | Signed wheel delta or Unicode scalar. |
| 8, 10 | 2 bytes each | X, Y | Coordinates from 0 to 65535 across the desktop. |
| 12 | 2 bytes | Button mask | Low 5 bits only. |
| 14 | 2 bytes | Modifier mask | Low 8 bits only. |

| Type | Event | Fields |
| ---: | --- | --- |
| 1, 2 | Key down, key up | Code is a HID usage from 0 to 231. Keys are physical; the desktop layout determines the character. |
| 3 | Pointer move | X and Y are the target position. |
| 4, 5 | Button down, button up | Code is 1 to 3; X and Y are the target position. |
| 6 | Wheel | Code is 0 for vertical or 1 for horizontal; value is a multiple of 120 from -1200 to 1200. X and Y are the target position. |
| 7 | Release all | Code, value, and both masks must be zero. |
| 8 | Text | Value is one Unicode scalar from 1 to `0x10FFFF`, excluding `0xD800` to `0xDFFF`. Code and both masks must be zero. |

Text events type a character without changing held keys. Held modifiers are lifted and restored around the character. Values `0x0A` and `0x09` type Return and Tab. X11 uses up to 16 spare keys for characters absent from the layout; Wayland uses up to 32. On X11, those characters can be missed if an application freezes for over 50 ms and spare keys are reused. On X11, an absent character is unsupported if no spare key is available.

The server processes records in order and stops at the first record rejected during validation or enqueueing. A successful response reports the number of records applied, for example `{"applied":1}`. The response waits for the desktop to apply them; it does not guarantee the application reacted. Keys and buttons held by a batch are released when it ends or fails. Masks do not press or release keys or buttons: send explicit down and up events.

`POST /input` rejects a batch if the pending input plus the batch exceeds 1024 events. On an input error with `index`, records before that index were applied. A `busy` error can include `applied`, the number of records queued minus those dropped; a backend failure can make this count overstate applied input, and dropped records need not be consecutive. Do not use `applied` as a retry offset; check the desktop before retrying.

## WebSocket protocol

| Connection | Rule |
| --- | --- |
| `/stream` | Receives video and, when supported, cursor messages. Sends ACKs and input. |
| `/stream?view=1` | Receives the same messages, but does not own input. |
| `/stream?take=1` | Works like `/stream`, and takes input from the current owner. |
| `/stream?clipboard=1` | Works like `/stream` and also receives a clipboard notice when the desktop clipboard changes. |
| `/stream?take=1&clipboard=1` | Works like `/stream?take=1` and also receives clipboard notices. The bundled viewer uses it when it takes control. |
| `/control` | Receives a control-ready message, then sends input and receives one response per request. Up to 32 replies can be queued. |
| All | Use the `nsl1` subprotocol and binary messages. One WebSocket message contains one protocol message. A `/stream` connection without `?view=1`, or a `/control` connection, owns input and blocks `POST /input`. |

A `/stream?view=1` connection watches without owning input, so `POST /input` and the CLI keep working while it is connected. The server ignores input messages sent on it. To take input, close the connection and open `/stream?take=1`.

The engine admits up to four `/stream` connections, with one input owner at a time.

| Connection | Result |
| --- | --- |
| `/stream?view=1` | Opens if a viewer slot is available. No change to input ownership. |
| `/stream` | Opens and owns input if a viewer slot is available and input has no owner. |
| `/stream?take=1` | Opens and owns input if a viewer slot is available or the current owner is a viewer it can replace. The previous input owner closes with code 4001 and reason `taken`. |
| `/control` | Opens and owns input if input has no owner. Otherwise it opens and closes with code 4003 and reason `busy`. |

Adding `clipboard=1` to a supported stream URL does not change these rules. A refused `/stream` connection fails its WebSocket handshake, while a refused `/control` connection opens and closes with code 4003 and reason `busy` so a client can tell it apart from an unreachable engine. Held keys and buttons are released when a connection loses input. A browser that loses input to a takeover reconnects with `?view=1`.

When the session has a token, offer both `nsl1` and `bearer.TOKEN` as subprotocols; the server selects `nsl1`. Do not put the token in a request URL.

### Outputs

The Wayland and X11 backends can capture one display of several. On X11 a display is a RandR monitor.

- `GET /capabilities` lists the displays in `"outputs"`, for example `[{"name":"DP-2","x":0,"y":0,"width":1920,"height":1080,"scale":1,"primary":true}]`. `x` and `y` give the position in the desktop layout, in logical pixels. `width` and `height` give the capture size in physical pixels. `scale` gives the physical pixels for each logical pixel.
- `primary` marks the display that a URL without `output=NAME` captures. On Wayland this is the `--output` display, or the first one. On X11 a URL without `output=NAME` captures the whole screen, so no display is primary when there are several.
- Add `output=NAME` to a `/stream` or `/control` URL to use that display. Put it before `codec=tiles`, for example `/stream?view=1&output=DP-3&codec=tiles`. It can come once. A name has 1 to 63 of `A-Z a-z 0-9 . _ - :`.
- Pointer input from that connection goes to that display: 0 to 65535 covers the display, not the desktop.
- `POST /input?output=NAME` and `GET /screenshot.png?output=NAME` use that display. An unknown name gives `404` with `"error":"unknown_output"`.
- A `/stream` URL with an unknown name fails its first capture, and the connection closes. A `/control` URL with an unknown name opens and closes with code 1008 and reason `unknown output`.
- `output=all` captures every display in one picture, each placed by its `x` and `y`. Pointer input from that connection then covers the whole layout, so a drag can cross displays. On X11 `all` is the whole screen.
- Other backends have no `"outputs"`. Their handshake fails for a URL with `output=NAME`.

### Grants

A grant is a second credential with a narrower scope and a lifetime, for a viewer or client that should not hold the session token. Send `POST /grants/watch` or `POST /grants/control` with the session token as `Authorization: Bearer TOKEN`. Add `?ttl=SECONDS` to set the lifetime; the default and the maximum are 3600. Add `connections=1` for a single-connection grant, such as `?ttl=120&connections=1`. The response is `{"id":"…","token":"…","expires_in":3600,"scope":"watch"}`, with `"connections":1` for a single-connection grant. A client offers the grant's `token` as `bearer.TOKEN`, the same way as the session token.

A WebSocket that offers a grant connects from any page origin, `Origin: null` included, because apps such as MCP App hosts frame viewers on opaque or per-load origins. The grant is the scoped credential. A WebSocket or HTTP request with the session token still needs an allowed origin; see [Browser origins](https://screen-relay.dev/docs/remote-access#browser-origins).

A single-connection grant opens one WebSocket. That connection stays open until the grant is revoked or expires, or the connection closes. Every later connection with the grant fails its WebSocket handshake: the engine closes it without an HTTP response or an upgrade, the same as other refused `/stream` connections. A connection the engine refuses for another reason, such as the wrong scope or a busy `/control`, doesn't use the grant, but a connection that opened and then closed does. A copied link with a used grant no longer works.

| Scope | WebSocket connections it opens |
| --- | --- |
| `watch` | `/stream?view=1` |
| `control` | `/stream`, `/stream?take=1`, `/stream?view=1`, and `/control` |

A grant never opens a URL with `clipboard=1`, and it does not authorize HTTP requests, so `/clipboard`, `/input`, `/screenshot.png`, `/capabilities`, `/metrics`, and `/grants` still need the session token. Grant connections share up to three browser slots. One of the four browser slots is reserved for a connection that uses the session token.

A grant can open connections until it ends. It ends when its lifetime runs out, or when you send `DELETE /grants/ID` with the session token, which returns 200. The engine then closes the connections opened with that grant and refuses new ones. Connections opened with the session token or another grant stay open. The engine keeps at most 32 active grants in memory. A request to create a grant returns HTTP 503 `grant_limit` when that limit is reached, HTTP 400 `invalid_ttl` when the requested lifetime is outside 1 to 3600 whole seconds, HTTP 400 `invalid_connections` when `connections` is anything but `1` or appears twice, or HTTP 400 `invalid_query` when a query parameter is 64 characters or longer. If two handshakes with an unused single-connection grant arrive at once, the one that opens second is closed with code `1008` and reason `grant used`. Stopping the session ends them all. A live connection closed because its grant ended receives WebSocket close code `1008` with reason `grant ended`.

The bundled viewer accepts a grant in its link. `#token=GRANT_TOKEN&scope=watch` opens a watch grant: the viewer connects only to `/stream?view=1` and offers no way to take control. `#token=GRANT_TOKEN&scope=control` opens a control grant: the viewer connects to `/stream`, `/stream?take=1`, or `/stream?view=1` without `clipboard=1`. With either scope the viewer sends no HTTP request. A link with only `#token=` is treated as the session token.

### Messages

Every message starts with this 20-byte header.

| Header offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 bytes | ASCII `NSL1`. |
| 4 | 1 byte | Version: `1`. |
| 5 | 1 byte | Message type from the table below. |
| 6 | 2 bytes | Flags: zero except frame bit 0, which marks a keyframe. |
| 8 | 4 bytes | Nonzero generation, incremented for each connection. |
| 12 | 4 bytes | Sequence; meaning depends on message type. |
| 16 | 4 bytes | Payload length in bytes. |

| Type | Direction | Payload and sequence |
| ---: | --- | --- |
| 1 | Server to `/stream` | Width and height (2 bytes each), capture time in microseconds (8 bytes), and an H.264 Annex B access unit. Sequence starts at 1 and increases per frame. The first and recovery frames are keyframes with SPS and PPS. |
| 2 | `/stream` to server | Optional 4-byte payload: how long the frame waited inside the viewer, from its arrival to the draw or drop, as big-endian microseconds that saturate at 4294967295. An empty payload counts as no wait, and the server counts a wait longer than the frame's whole capture-to-ACK time as no wait. ACK each frame in order after drawing it or dropping it for a newer frame; use its generation and sequence. Only the oldest outstanding frame earns credit. |
| 3 | Client to either endpoint | One 16-byte input record. Sequence is zero on `/stream` or a nonzero request ID on `/control`. Ignored on `/stream?view=1`. |
| 4 | Server to `/control` | Desktop width and height, 2 bytes each. Sequence is zero. |
| 5 | Server to `/control` | One status byte: 0 accepted, 1 invalid, 2 unsupported. Sequence echoes the request ID. A status of 0 does not report a later backend failure. |
| 6 | Server to `/stream` | Width, height, hotspot X, and hotspot Y (2 bytes each), followed by straight-alpha RGBA pixels row by row. Sequence is zero. No ACK is needed. |
| 7 | Server to a `/stream` opened with `clipboard=1` | Clipboard notice: a desktop application changed the clipboard. Changes made through `POST /clipboard` do not send a notice. Empty payload, sequence zero, no ACK. Read the text with `GET /clipboard`. |
| 8 | Server to a `/stream` opened with `codec=tiles` | One tiles frame, in place of type 1. See [Tiles codec](#tiles-codec). Flags bit 0 marks a keyframe. Sequence starts at 1 and increases per frame. ACK it like type 1. |
| 9 | `/stream` with `codec=tiles` to server | Keyframe request. Empty payload, sequence zero. The next type 8 frame is a keyframe. |

| Rule | Detail |
| --- | --- |
| Encoding | All integers are unsigned big-endian except the signed input value. Reject malformed messages and unknown types or versions. |
| Flow control | At most 3 seconds of frames at the configured frame rate, and 8 MiB, can be in flight. With frames outstanding, the stream closes after about 3 seconds without ACK progress or when the oldest frame reaches 3 seconds. |
| Frame size | An access unit is at most 8 MiB; the complete message is at most 8,388,640 bytes. Decode frames in order. |
| Counters | Reconnect when the frame sequence reaches 4294967295. If the generation reaches that value, restart the session before connecting again. |
| Cursor | At most 256 by 256 pixels, with the hotspot inside the image. Eight zero bytes restore the local arrow; a transparent image hides the cursor. The cursor does not follow pointer jumps made by the desktop. |
| Disconnect | Held input is released. Reconnecting does not replay requests. |
| Clipboard | Only a `/stream` opened with `clipboard=1` receives type 7, so a client that rejects unknown types keeps working. Several changes close together can arrive as one notice, and one change can arrive as more than one. |

## Tiles codec

The tiles codec sends exact pixels. It suits text, terminals and code editors, where H.264 blurs.

- Start the engine with `--tiles`. Then `GET /capabilities` has `"codecs":["h264","tiles"]` and `"tiles_version":1`.
- Add `codec=tiles` as the last query parameter of a `/stream` URL, for example `/stream?view=1&codec=tiles`. Without `--tiles`, the handshake fails.
- Other connections keep H.264.
- An engine built with `make X264=0` has no H.264 encoder. `--tiles` is then always on, `/capabilities` shows `"codecs":["tiles"]`, and a `/stream` URL without `codec=tiles` fails its handshake.
- Add `&tiles_inflight=K` after `codec=tiles` to allow K frames in flight, K from 1 to 4. The default is 1. `/capabilities` shows `"tiles_max_inflight":4`.
- Add `&size=WxH` after `codec=tiles` to get a smaller picture. The engine divides the desktop by the largest whole factor k that still gives at least W x H, at least 1. Each pixel is the mean of a k x k block. A whole factor keeps a scroll of a multiple of k pixels an exact copy. The keyframe gives the scaled size. `/capabilities` shows `"tiles_scale":"integer"`. Input coordinates stay 0 to 65535 across the whole desktop.
- `relay-engine --tiles --scale gpu` lets the capture system scale for `size=WxH`. On macOS, ScreenCaptureKit then delivers frames that fit inside W x H, keep the aspect ratio, and have even sizes of at least 320x240. Such a size is not a whole factor, so scroll copies mostly miss. `/capabilities` shows `"tiles_scale_gpu":true`. Other backends use the box filter.
- `tiles_inflight` and `size` can come in any order after `codec=tiles`, each once.
- Clipboard sync works on a tiles stream. Open the control stream with `clipboard=1` before `codec=tiles`, for example `/stream?take=1&clipboard=1&codec=tiles`. Type 7 notices then arrive between type 8 frames. Read the text with `GET /clipboard` and write it with `POST /clipboard`.
- The engine keeps the allowed number of tiles frames in flight. It collects damage until the ACK arrives, then sends one frame for all of it. A slow client gets fewer frames, not older frames.

A type 8 payload, all integers big-endian:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Format version: 1 |
| 1 | 1 | Flags: bit 0 is a keyframe, bit 2 is partial |
| 2 | 4 | Desktop width and height, 2 bytes each |
| 6 | 8 | Capture time in microseconds |
| 14 | 2 | Op count |
| 16 | 4 | Length of the pixel blob |
| 20 | | Ops, then the pixel blob |

| Op | Fields after the op byte | Effect on the RGB buffer |
| ---: | --- | --- |
| 1 copy | sx, sy, x, y, w, h (2 bytes each) | Copies the w x h rectangle at (sx, sy) to (x, y). The rectangles can overlap. The result is as if the source was read first. |
| 2 pixels | x, y, w, h | Replaces the rectangle with the next w x h x 3 bytes (R, G, B) of the decoded blob |
| 3 fill | x, y, w, h, then r, g, b (1 byte each) | Fills the rectangle with one colour |

- The pixel blob is one zstd frame. It decodes to the pixel bytes of all pixels ops, in op order. It is empty when there are no pixels ops.
- Apply the ops in order. On a keyframe, first make a black buffer of the given size.
- Each rectangle is inside the desktop and is not empty. Reject a frame that breaks a rule and send type 9.
- When the damage of one capture does not fit in one 8 MiB message, the engine sends it in several frames at once. Every frame except the last has the partial bit. Apply a partial frame, and draw after the next frame without the bit.

The reference decoder is `relay::tiles::apply` in `src/engine/core/tiles.cpp`. A test client is `tests/e2e/tiles-client.mjs`.
