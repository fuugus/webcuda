# WebCUDA

A unified graphics pipeline: **CUDA** compute writing directly into an
**OpenGL** texture (zero-copy interop), with a **Chromium (CEF)** web overlay
composited over the full viewport in the same swapchain. One OS window, one
present per frame. The web layer provides professional draggable/resizable
in-app windows; everything outside them is transparent and belongs to the
GPU scene.

```
┌─ SDL3 window ──────────────────────────────────────────┐
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

## Why interactive resize is artifact-free

The windowing layer is SDL3 (vendored via FetchContent, statically linked)
specifically because its X11 backend implements the `_NET_WM_SYNC_REQUEST`
frame-sync handshake: the WM tags each resize step, and SDL acknowledges the
tag inside `SDL_GL_SwapWindow` — so the compositor waits for our matching
frame before displaying that step. Mismatch frames (the classic GL resize
jitter, worst vertically due to GL's bottom-left origin) are impossible by
protocol. This is the same mechanism Chrome and GTK apps use; GLFW does not
implement it. On top of that the app renders synchronously inside the resize
event, drops vsync while a resize is in progress (presents paced at ~2×
refresh; the sim never fast-forwards), and draws the overlay pixel-exact
anchored top-left while CEF's relayout catches up.

The built-in self-test measures this end-to-end: it synthetically drags the
"Simulation" window for 240 frames and counts real CEF paints
(`./webcuda --selftest` → ~59 paints/s at 60 Hz = a repaint every frame).

## Layout

| path | what |
|---|---|
| `src/main.cpp` | SDL3 window, render loop, compositing, input routing, self-test |
| `src/overlay.{h,cpp}` | CEF: off-screen browser, message pump, dirty rects, JS↔C++ bridge |
| `src/sim.{h,cu}` | CUDA Gray-Scott reaction-diffusion + palettes + GL interop |
| `src/gl_funcs.{h,cpp}` | tiny GL extension loader (no GLEW/glad dependency) |
| `ui/index.html` | the overlay UI: window manager (drag/resize/z-order), controls, stats |
| `third_party/cef/` | CEF binary distribution (not in git; see below) |

## Building

```sh
./build.sh              # builds the host platform (downloads CEF on first run)
./build.sh linux --test # build + run the self-test (needs display + NVIDIA GPU)
./build.sh windows      # on a Windows host (Git Bash + MSVC + CUDA toolkit)
```

The script auto-detects the host, fetches the matching CEF binary
distribution into `third_party/cef-<platform>`, picks the newest CUDA
toolkit over a stale distro `nvcc`, and selects GPU architectures (`native`
with a GPU present, a fixed set otherwise — which is how the CI compiles
without one). SDL3 is fetched and built automatically by CMake.

| platform | status |
|---|---|
| Linux x86_64 | full support, runtime-tested (this is the reference platform) |
| Windows x86_64 | full stack supported (CUDA/GL/SDL3/CEF) incl. frameless chrome and live resize; runtime-tested (`build.bat` wraps the MSVC env setup) |
| macOS | **unsupported** — CUDA does not exist on Apple platforms (Apple dropped NVIDIA in 2019; Apple Silicon cannot host NVIDIA GPUs). The shell would port with a Metal/CPU compute backend behind `sim.h` |

nvcc cannot cross-compile (Windows CUDA requires MSVC), so each platform
builds natively; `.github/workflows/build.yml` compile-checks Linux and
Windows on every push.

Linux dependencies: CUDA toolkit, `libgl-dev`, `libx11-dev` + X11 extension
headers (xext, xrandr, xcursor, xfixes, xi), cmake ≥ 3.26.

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

On Windows the same chrome works via the Win32 route: move/resize hand off
to the OS modal loop (`WM_NCLBUTTONDOWN` + hit-test codes, so DWM snapping
works), and an SDL event watch keeps rendering from inside that loop —
without it the app would freeze for the duration of every border drag,
since `SDL_PollEvent` stops returning there. On other platforms frameless
mode falls back to `--native` automatically.

## Fallback path

If the GL context is not on a CUDA device (software GL, hybrid laptop,
mismatched driver), the app prints a notice and switches to a
device→host→texture copy per frame. CUDA still computes on the GPU; only the
display path pays one copy. The zero-copy interop engages automatically when
GL runs on the NVIDIA card (X11/GLX preferred, Wayland fallback handled).

## Windows notes

The port is complete and runtime-tested: exe-dir lookup, `CefMainArgs(hInstance)`,
VK-code keyboard translation, frameless chrome (`WM_NCLBUTTONDOWN` hit-test
handoff) and live resize (event-watch rendering inside the modal loop) all
have Win32 paths. Remaining optional upgrade: CEF `shared_texture_enabled` +
`OnAcceleratedPaint` gives a D3D11 shared texture on Windows (zero-copy UI
too); interop via `WGL_NV_DX_interop2` or move the scene to Vulkan
external-memory.

One deployment gotcha: a CUDA binary only runs kernels on GPUs it carries
SASS for (or newer PTX the *driver's* JIT accepts — a driver older than the
building toolkit rejects the PTX outright). Building locally uses
`-arch=native`, so this only concerns binaries built elsewhere, e.g. CI
artifacts.
