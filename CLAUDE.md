# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status

Early stage: one C prototype (`src/capture_imx219.c`) exists. No tests or linter. The git repo is not initialized yet (intended at `workspace/` or `deepstream_node/`).

## Layout

- `deepstream_node/` — the project (own `README.md` and `.gitignore`).
- `deepstream_node/deepstream_test/` — where DeepStream pipelines are prototyped first; sources in `src/`, binaries in `bin/`, captured images in `output/`, built by the local `Makefile`.

## Build and run

Run from `deepstream_node/deepstream_test/` (the default output path `output/imx219.jpg` is relative to the cwd):

- Build: `make` (builds `bin/capture_imx219` with `pkg-config gstreamer-1.0`); `make clean` removes `bin/`.
- Run: `./bin/capture_imx219 [output.jpg]` (default `output/imx219.jpg`). Headless, no display needed.
- `make` builds every target in `all`; a pattern rule (`bin/%: src/%.c`) covers new files, just add the binary to `all`.
- `./bin/stream_count [seconds] [output.jpg]` streams continuously, counts frames, and after N seconds (counted from the first frame) saves the next frame as JPEG and exits (default 5s, `output/stream_last.jpg`).

## Pipeline code pattern

Prototypes follow DeepStream sample `deepstream-test1`: build elements with `gst_element_factory_make`, link with `gst_element_link_many`, handle EOS/ERROR in a `bus_call` watch that quits a `GMainLoop`.

`capture_imx219.c` pipeline: `nvarguscamerasrc -> capsfilter(NVMM NV12, WxH@FPS) -> nvvideoconvert -> capsfilter(NVMM I420) -> nvjpegenc -> filesink`. To save a single, auto-exposure-settled frame, the source emits `NUM_BUFFERS` (30) buffers (`num-buffers`) and a pad probe on the source's `src` pad drops all but the last one. EOS from `num-buffers` ends the loop; the output file is deleted on any failure after start.

`stream_count.c` has the same pipeline without `num-buffers`; the probe drops everything until a timer fires, passes one frame, then EOS is pushed into `camfilter`'s sink pad from the main thread. Do not send EOS to the pipeline or the source: `nvarguscamerasrc` blocks forever. Use `g_timeout_add`, not `g_timeout_add_seconds` (it can be up to 1s late).

Both programs set `wbmode` via the `WBMODE` define (default 3, fluorescent; override with `-DWBMODE=n`). Auto (1) gave a magenta cast that changed from run to run; no mode was fully neutral on this IMX219 (3 is the closest, slightly green).

Sensor mode is picked by the first capsfilter (`WIDTH`/`HEIGHT`/`FPS` defines, currently 1280x720@60 = mode 4), not by `sensor-mode`. The combination must exist in the sensor's mode list (IMX219: 3280x2464@21, 3280x1848@28, 1920x1080@30, 1640x1232@30, 1280x720@60) or the pipeline fails to start.

## Target environment (Jetson Orin Nano Super, 8GB)

- L4T R39.2.1, Ubuntu 24.04, CUDA 13.2, TensorRT 10.16, DeepStream 9.2 (installed under `/opt/nvidia/deepstream`), Python 3.12.
- Camera: CSI Sony IMX219 at `/dev/video0` (a Jetson CSI sensor, so use `nvarguscamerasrc`, not `v4l2src`). No USB camera is attached.
- Memory is tight (7.4GB shared CPU/GPU); keep pipelines and models small.

## Conventions

- Keep READMEs short and free of filler; the root `README.md` only describes board, environment and camera.
- Generated artifacts (`*.engine`, `*.onnx`, `*.log`, `*.jpg`) and `bin/` are git-ignored; do not commit them.
