# WebCUDA

A unified graphics pipeline: **CUDA** compute writing directly into an
**OpenGL** texture (zero-copy interop), with a **Chromium (CEF)** web overlay
composited over the full viewport in the same swapchain. One OS window, one
present per frame. The web layer provides professional draggable/resizable
in-app windows; everything outside them is transparent and belongs to the
GPU scene.

```
┌─ GLFW window ──────────────────────────────────────────┐
│  frame N:                                              │
│   1. pump CEF + SendExternalBeginFrame()  (UI paints)  │
│   2. CUDA kernels → GL texture (surface writes, 0copy) │
│   3. upload CEF dirty rects → overlay texture          │
│   4. draw sim quad, blend overlay quad (premult alpha) │
│   5. swap (vsync)                                      │
└────────────────────────────────────────────────────────┘
```

## Why dragging web windows is smooth here (it wasn't in earlier attempts)

1. **External begin frames** — CEF's off-screen renderer normally free-runs at
   its own (default 30 fps) cadence. Here the render loop calls
   `SendExternalBeginFrame()` once per vsync, so Chromium composites in
   lockstep with the GL swapchain: one UI paint per displayed frame, no beat
   frequencies, no 30 fps cap.
2. **Dirty-rect uploads** — `OnPaint` keeps a CPU mirror of the UI frame and
   only the damaged rectangles are uploaded with `glTexSubImage2D`
   (`GL_UNPACK_ROW_LENGTH`). Dragging a 330 px window uploads ~0.5 MB/frame,
   not the whole 1600×900 buffer.
3. **Single window, alpha-routed input** — there is no second OS window to
   fight the compositor. Mouse events are routed to web or simulation by
   sampling the overlay's alpha under the cursor (with capture semantics for
   drags in both directions).
4. **Software OSR on purpose** — `--disable-gpu-compositing` in the CEF layer
   avoids Chromium's GPU→CPU readback latency; a DOM-only UI rasterizes in
   well under a frame.

The built-in self-test measures this end-to-end: it synthetically drags the
"Simulation" window for 240 frames and counts real CEF paints
(`./webcuda --selftest` → ~59 paints/s at 60 Hz = a repaint every frame).

## Layout

| path | what |
|---|---|
| `src/main.cpp` | GLFW window, render loop, compositing, input routing, self-test |
| `src/overlay.{h,cpp}` | CEF: off-screen browser, message pump, dirty rects, JS↔C++ bridge |
| `src/sim.{h,cu}` | CUDA Gray-Scott reaction-diffusion + palettes + GL interop |
| `src/gl_funcs.{h,cpp}` | tiny GL extension loader (no GLEW/glad dependency) |
| `ui/index.html` | the overlay UI: window manager (drag/resize/z-order), controls, stats |
| `third_party/cef/` | CEF binary distribution (not in git; see below) |

## Building (Linux)

Dependencies: CUDA toolkit 13.x, `libglfw3-dev`, `libgl-dev`, `libx11-dev`,
cmake ≥ 3.26. CEF 148 linux64-minimal extracted to `third_party/cef`:

```sh
cd third_party
curl -LO "https://cef-builds.spotifycdn.com/cef_binary_148.0.10%2Bg7ee53f5%2Bchromium-148.0.7778.218_linux64_minimal.tar.bz2"
tar xjf cef_binary_*.tar.bz2 && mv cef_binary_*linux64_minimal cef
```

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CUDA_ARCHITECTURES=120 \
      -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.1/bin/nvcc
cmake --build build -j
./build/webcuda
```

(The two `-D`s work around Ubuntu shipping an older `/usr/bin/nvcc` and
CMake 3.31 probing the removed `sm_52`.)

## Window chrome

By default the app runs **frameless** with its own web-rendered header bar:
minimize / maximize / close buttons, drag the empty bar space to move the OS
window, double-click to maximize, and pull any window edge to resize. Move
and resize are delegated to the window manager via `_NET_WM_MOVERESIZE`, so
they are exactly as smooth as a native titlebar and keep WM snapping.
`--native` starts with normal OS decorations instead. (Frameless mode
requires X11; on other platforms it falls back to `--native` automatically.)

## Controls

- **drag/resize** the UI windows by titlebar / edges, like any desktop app
- **left-drag on the background** injects chemical V into the simulation
- **mouse wheel** over background: brush size; over UI: scrolls
- **F11** fullscreen toggle, **Esc** quits (when a UI window isn't focused)
- top bar buttons reopen closed windows

Flags: `--native` (OS window decorations instead of the web header bar),
`--selftest` (automated drag-smoothness measurement + screenshot),
`--shot file.ppm`, `--size WxH`, `--no-ext-bf` (compare against the old
free-running paint mode).

## Fallback path

If the GL context is not on a CUDA device (software GL, hybrid laptop,
mismatched driver), the app prints a notice and switches to a
device→host→texture copy per frame. CUDA still computes on the GPU; only the
display path pays one copy. The zero-copy interop engages automatically when
GL runs on the NVIDIA card (X11/GLX preferred, Wayland fallback handled).

## Windows notes (design only, untested)

Every component is cross-platform: GLFW, CEF, CUDA-GL interop all work on
Windows. Porting checklist:
- replace the `readlink("/proc/self/exe")` exe-dir lookup and drop `XInitThreads`
- CEF on Windows wants `CefMainArgs(hInstance)` and the sandbox disabled the same way
- keyboard translation already uses Windows VK codes (CEF's cross-platform convention)
- optional upgrade: CEF `shared_texture_enabled` + `OnAcceleratedPaint` gives a
  D3D11 shared texture on Windows (zero-copy UI too); interop via
  `WGL_NV_DX_interop2` or move the scene to Vulkan external-memory
