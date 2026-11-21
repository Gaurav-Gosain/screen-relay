# Benchmarks

## tiles-bench

`make bench-tiles TILES_FRAMES=DIR` replays recorded frames through the tiles encoder and decoder.

- Each file in DIR is `NAME.WIDTHxHEIGHT.rgb`: raw RGB frames, one after the other.
- The damage of each frame is the bounding box of the pixels that changed since the frame before.
- The first frame is a keyframe. The other columns count the frames after it.
- `lossless` is `yes` when every decoded frame equals the recorded frame byte for byte.
- A second argument sets the zstd level: `./build/tiles-bench DIR 3`.
