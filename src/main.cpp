// webcuda — unified graphics pipeline demo
//   CUDA simulation -> OpenGL texture (zero-copy interop)
//   CEF web overlay -> transparent UI layer in the same swapchain
//
// One OS window (SDL3). The web layer provides draggable/resizable in-app
// windows; input is routed to web or simulation based on overlay alpha under
// the cursor. SDL3's X11 backend implements the _NET_WM_SYNC_REQUEST
// frame-sync handshake (acknowledged inside SDL_GL_SwapWindow), so the window
// manager waits for our frame on every interactive resize step — no mismatch
// frames, no jitter.

#include <X11/Xlib.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "gl_funcs.h"
#include "overlay.h"
#include "sim.h"

#include <limits.h>
#include <unistd.h>

namespace {

struct AppState {
  SDL_Window* window = nullptr;
  SDL_GLContext glctx = nullptr;
  Overlay overlay;
  SimParams params;
  SimStats simStats;

  // native X11 handles (frameless move/resize protocol); null off X11
  Display* xdpy = nullptr;
  ::Window xwin = 0;

  int fbW = 0, fbH = 0;
  int simW = 0, simH = 0;
  int hz = 60;

  // input routing
  double mouseX = 0, mouseY = 0;
  bool btn[3] = {false, false, false};
  bool webCaptured = false;    // mouse-down started on web UI
  bool sceneCaptured = false;  // mouse-down started on simulation
  bool webFocused = false;

  // cursors
  SDL_Cursor* cursors[10] = {};
  SDL_Cursor* webCursor = nullptr;
  SDL_Cursor* current = nullptr;

  // frameless mode (default): web header bar owns move/minimize/maximize/close
  bool frameless = true;

  // renders one frame, returns true if it actually presented; callable from
  // resize/expose events for low-latency redraws during interactive resize
  // (arg: advance the simulation?)
  std::function<bool(bool)> render;
  double lastResizeTime = -1e9;
  uint64_t presentCount = 0;
  bool pendingShot = false;

  // resize probe (selftest): tracks UI element rows in Chromium's output
  // while the window height oscillates — distinguishes "Chromium moved the
  // boxes" from compositing/presentation artifacts
  int probeBarMin = 1 << 20, probeBarMax = -1;
  int probeBoxMin = 1 << 20, probeBoxMax = -1;
  bool tCollapsed = false, tExpanded = false;

  // stats
  double fps = 0, frameMs = 0;
  double paintsPerSec = 0;
  double uploadMsAvg = 0;

  // selftest
  bool selftest = false;
  int selftestPhase = 0;  // 0 wait-ready, 1 settle, 2 drag, 3 resize, 4 done
  int selftestFrame = 0;
  double dragPointX = -1, dragPointY = -1;
  uint64_t dragStartPaints = 0;
  std::chrono::steady_clock::time_point dragStartTime;
  int dragFrames = 240;
  std::string shotPath = "selftest.ppm";
  bool uiReady = false;

  bool shouldQuit = false;
};

AppState g;

double nowSec() { return SDL_GetTicksNS() * 1e-9; }

std::string exeDir() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return ".";
  buf[n] = 0;
  std::string s(buf);
  auto p = s.rfind('/');
  return p == std::string::npos ? "." : s.substr(0, p);
}

uint32_t mods() {
  return cefModifiersFromSDL(SDL_GetModState(), g.btn[0], g.btn[1], g.btn[2]);
}

void brushAt(double x, double y) {
  double sx = x / std::max(1, g.fbW) * g.simW;
  double sy = y / std::max(1, g.fbH) * g.simH;
  simBrush((float)sx, (float)sy, g.params.brushRadius);
}

void setWebFocus(bool focused) {
  if (g.webFocused == focused) return;
  g.webFocused = focused;
  g.overlay.setFocus(focused);
  // text-input events (and IME) only while a web element can take them
  if (focused)
    SDL_StartTextInput(g.window);
  else
    SDL_StopTextInput(g.window);
}

// ---------------------------------------------------------------------------
// Frameless window: move/resize via the WM protocol (_NET_WM_MOVERESIZE).
// The window manager performs the operation natively, so dragging by our web
// header bar behaves exactly like a real titlebar (snapping included).
// ---------------------------------------------------------------------------
enum {  // _NET_WM_MOVERESIZE directions
  kWmSizeTopLeft = 0, kWmSizeTop = 1, kWmSizeTopRight = 2, kWmSizeRight = 3,
  kWmSizeBottomRight = 4, kWmSizeBottom = 5, kWmSizeBottomLeft = 6,
  kWmSizeLeft = 7, kWmMove = 8,
};

bool canMoveResize() {
  return g.frameless && g.xdpy &&
         !(SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN);
}

// Direction for the resize border under the cursor, or -1.
int edgeDir(double x, double y) {
  if (!canMoveResize() ||
      (SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED))
    return -1;
  const int B = 6;
  bool l = x < B, r = x > g.fbW - B, t = y < B, b = y > g.fbH - B;
  if (t && l) return kWmSizeTopLeft;
  if (t && r) return kWmSizeTopRight;
  if (b && l) return kWmSizeBottomLeft;
  if (b && r) return kWmSizeBottomRight;
  if (t) return kWmSizeTop;
  if (b) return kWmSizeBottom;
  if (l) return kWmSizeLeft;
  if (r) return kWmSizeRight;
  return -1;
}

void startWmMoveResize(int dir) {
  if (!canMoveResize()) return;

  // The WM takes over the pointer: we will never see the button release.
  // Settle our own state and the web layer's first.
  if (g.btn[0]) g.overlay.mouseButton((int)g.mouseX, (int)g.mouseY, 0, false, 1, 0);
  g.btn[0] = g.btn[1] = g.btn[2] = false;
  g.webCaptured = g.sceneCaptured = false;

  int wx = 0, wy = 0;
  SDL_GetWindowPosition(g.window, &wx, &wy);

  XUngrabPointer(g.xdpy, CurrentTime);  // release the implicit press grab
  XEvent ev{};
  ev.xclient.type = ClientMessage;
  ev.xclient.window = g.xwin;
  ev.xclient.message_type = XInternAtom(g.xdpy, "_NET_WM_MOVERESIZE", False);
  ev.xclient.format = 32;
  ev.xclient.data.l[0] = wx + (long)g.mouseX;
  ev.xclient.data.l[1] = wy + (long)g.mouseY;
  ev.xclient.data.l[2] = dir;
  ev.xclient.data.l[3] = Button1;
  ev.xclient.data.l[4] = 1;  // source: normal application
  XSendEvent(g.xdpy, DefaultRootWindow(g.xdpy), False,
             SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  XFlush(g.xdpy);
}

// ---------------------------------------------------------------------------
// Input handlers
// ---------------------------------------------------------------------------
void onMouseMove(double x, double y) {
  g.mouseX = x;
  g.mouseY = y;
  if (!g.sceneCaptured)  // keep hover states correct on the web layer
    g.overlay.mouseMove((int)x, (int)y, mods());
  if (g.sceneCaptured && g.btn[0]) brushAt(x, y);

  // cursor shape: resize arrows on frameless borders, web cursor over UI,
  // crosshair over the simulation
  SDL_Cursor* want;
  int dir = (g.webCaptured || g.sceneCaptured) ? -1 : edgeDir(x, y);
  switch (dir) {
    case kWmSizeTop: case kWmSizeBottom: want = g.cursors[5]; break;
    case kWmSizeLeft: case kWmSizeRight: want = g.cursors[4]; break;
    case kWmSizeTopLeft: case kWmSizeBottomRight: want = g.cursors[6]; break;
    case kWmSizeTopRight: case kWmSizeBottomLeft: want = g.cursors[7]; break;
    default:
      want = (g.webCaptured ||
              (!g.sceneCaptured && g.overlay.uiAt((int)x, (int)y)))
                 ? g.webCursor
                 : g.cursors[2];  // crosshair
  }
  if (want != g.current) {
    g.current = want;
    SDL_SetCursor(want);
  }
}

void onMouseButton(int button, bool down, int clicks) {
  if (button < 0 || button > 2) return;
  g.btn[button] = down;

  if (down) {
    // frameless resize borders take priority over everything
    if (button == 0) {
      int dir = edgeDir(g.mouseX, g.mouseY);
      if (dir >= 0) {
        g.btn[0] = false;
        startWmMoveResize(dir);
        return;
      }
    }

    bool onUi = g.overlay.uiAt((int)g.mouseX, (int)g.mouseY);
    if (onUi) {
      g.webCaptured = true;
      setWebFocus(true);
      g.overlay.mouseButton((int)g.mouseX, (int)g.mouseY, button, true,
                            std::clamp(clicks, 1, 3), mods());
    } else {
      g.sceneCaptured = true;
      setWebFocus(false);
      if (button == 0) brushAt(g.mouseX, g.mouseY);
    }
  } else {
    if (g.webCaptured) {
      g.overlay.mouseButton((int)g.mouseX, (int)g.mouseY, button, false,
                            std::clamp(clicks, 1, 3), mods());
    }
    if (!g.btn[0] && !g.btn[1] && !g.btn[2]) {
      g.webCaptured = false;
      g.sceneCaptured = false;
    }
  }
}

void onScroll(double dx, double dy) {
  if (g.overlay.uiAt((int)g.mouseX, (int)g.mouseY)) {
    g.overlay.mouseWheel((int)g.mouseX, (int)g.mouseY, dx, dy, mods());
  } else {
    g.params.brushRadius =
        std::clamp(g.params.brushRadius + (float)dy * 2.f, 4.f, 80.f);
  }
}

void toggleFullscreen() {
  bool fs = SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN;
  SDL_SetWindowFullscreen(g.window, !fs);
}

void onKey(const SDL_KeyboardEvent& e) {
  if (e.down && !e.repeat) {
    if (e.key == SDLK_F11) {
      toggleFullscreen();
      return;
    }
    if (e.key == SDLK_ESCAPE && !g.webFocused) {
      g.shouldQuit = true;
      return;
    }
  }
  if (!g.webFocused) return;
  int wkc = windowsKeyCodeFromSDL(e.key);
  if (!wkc) return;
  uint32_t m = cefModifiersFromSDL(e.mod, g.btn[0], g.btn[1], g.btn[2]);
  g.overlay.keyEvent(e.down, wkc, e.scancode, m);
}

void onTextInput(const char* utf8) {
  if (!g.webFocused) return;
  uint32_t m = mods();
  for (const unsigned char* p = (const unsigned char*)utf8; *p;) {
    unsigned cp = 0;
    int extra = 0;
    unsigned char c = *p++;
    if (c < 0x80) cp = c;
    else if ((c >> 5) == 0x6) { cp = c & 0x1f; extra = 1; }
    else if ((c >> 4) == 0xe) { cp = c & 0x0f; extra = 2; }
    else if ((c >> 3) == 0x1e) { cp = c & 0x07; extra = 3; }
    else continue;
    while (extra-- > 0 && (*p >> 6) == 0x2) cp = (cp << 6) | (*p++ & 0x3f);
    g.overlay.charEvent(cp, m);
  }
}

// ---------------------------------------------------------------------------
// JS -> native queries
// ---------------------------------------------------------------------------
bool onQuery(const std::string& req, std::string& resp) {
  char name[32];
  float v = 0;
  if (std::sscanf(req.c_str(), "set %31s %f", name, &v) == 2) {
    if (!std::strcmp(name, "F")) g.params.F = v;
    else if (!std::strcmp(name, "k")) g.params.k = v;
    else if (!std::strcmp(name, "steps")) g.params.steps = std::clamp((int)v, 0, 60);
    else if (!std::strcmp(name, "brush")) g.params.brushRadius = std::clamp(v, 4.f, 80.f);
    else if (!std::strcmp(name, "palette")) g.params.palette = (int)v & 3;
    resp = "ok";
    return true;
  }
  double rx, ry;
  if (std::sscanf(req.c_str(), "ready %lf %lf", &rx, &ry) == 2) {
    g.dragPointX = rx;
    g.dragPointY = ry;
    g.uiReady = true;
    resp = "ok";
    return true;
  }
  if (req == "win drag") { startWmMoveResize(kWmMove); resp = "ok"; return true; }
  if (req == "win minimize") { SDL_MinimizeWindow(g.window); resp = "ok"; return true; }
  if (req == "win maximize") {
    if (SDL_GetWindowFlags(g.window) & SDL_WINDOW_MAXIMIZED)
      SDL_RestoreWindow(g.window);
    else
      SDL_MaximizeWindow(g.window);
    resp = "ok";
    return true;
  }
  if (req == "win close") { g.shouldQuit = true; resp = "ok"; return true; }
  if (req == "cmd pause") { g.params.paused = true; resp = "ok"; return true; }
  if (req == "cmd resume") { g.params.paused = false; resp = "ok"; return true; }
  if (req == "cmd reset") { simReset(); resp = "ok"; return true; }
  if (req == "cmd quit") { g.shouldQuit = true; resp = "ok"; return true; }
  if (req == "info") {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s|%dx%d", simDeviceName(), g.simW, g.simH);
    resp = buf;
    return true;
  }
  return false;
}

void onCursorChange(int cefType) {
  // cef_cursor_type_t values we care about
  SDL_Cursor* c = g.cursors[0];
  switch (cefType) {
    case 0: c = g.cursors[0]; break;   // CT_POINTER
    case 1: c = g.cursors[2]; break;   // CT_CROSS
    case 2: c = g.cursors[3]; break;   // CT_HAND
    case 3: c = g.cursors[1]; break;   // CT_IBEAM
    case 6: case 13: case 14: c = g.cursors[4]; break;  // E/W resize
    case 7: case 10: c = g.cursors[5]; break;           // N/S resize
    case 8: case 12: c = g.cursors[7]; break;           // NE/SW
    case 9: case 11: c = g.cursors[6]; break;           // NW/SE
    case 15: c = g.cursors[5]; break;  // NS
    case 16: c = g.cursors[4]; break;  // EW
    case 17: c = g.cursors[7]; break;  // NESW
    case 18: c = g.cursors[6]; break;  // NWSE
    case 35: c = g.cursors[8]; break;  // CT_MOVE-ish
    default: c = g.cursors[0]; break;
  }
  g.webCursor = c;
}

// ---------------------------------------------------------------------------
void writePPM(const char* path, int w, int h) {
  std::vector<unsigned char> px((size_t)w * h * 3);
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = h - 1; y >= 0; y--)
    std::fwrite(px.data() + (size_t)y * w * 3, 1, (size_t)w * 3, f);
  std::fclose(f);
  std::printf("[shot] wrote %s (%dx%d)\n", path, w, h);
}

const char* kVS = R"(#version 330 core
out vec2 uv;
void main() {
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  uv = vec2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
  gl_Position = vec4(p, 0.0, 1.0);
})";

// uvScale maps screen pixels to texture pixels. (1,1) stretches to fill;
// (fbW/texW, fbH/texH) samples pixel-exact anchored top-left, which keeps the
// UI rock-solid during live resize while CEF repaints lag the window size.
const char* kFS = R"(#version 330 core
in vec2 uv;
out vec4 frag;
uniform sampler2D tex;
uniform vec2 uvScale;
void main() {
  vec2 t = uv * uvScale;
  if (t.x > 1.0 || t.y > 1.0) discard;
  frag = texture(tex, t);
})";

void handleEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      g.shouldQuit = true;
      break;
    case SDL_EVENT_MOUSE_MOTION:
      onMouseMove(e.motion.x, e.motion.y);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      int idx = e.button.button == SDL_BUTTON_LEFT     ? 0
                : e.button.button == SDL_BUTTON_MIDDLE ? 1
                : e.button.button == SDL_BUTTON_RIGHT  ? 2
                                                       : -1;
      onMouseButton(idx, e.button.down, e.button.clicks);
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      double s = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0 : 1.0;
      onScroll(e.wheel.x * s, e.wheel.y * s);
      break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      onKey(e.key);
      break;
    case SDL_EVENT_TEXT_INPUT:
      onTextInput(e.text.text);
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
      bool changed = e.window.data1 != g.fbW || e.window.data2 != g.fbH;
      g.fbW = e.window.data1;
      g.fbH = e.window.data2;
      if (changed && g.fbW > 0 && g.fbH > 0) {
        g.lastResizeTime = nowSec();
        g.overlay.resize(g.fbW, g.fbH);
        // redraw in lockstep with the resize event: SDL acknowledges the
        // WM's sync request inside this swap, so the compositor displays
        // exactly this frame for this resize step
        if (g.render) g.render(false);
      }
      break;
    }
    case SDL_EVENT_WINDOW_EXPOSED:
      if (g.render) g.render(false);
      break;
    case SDL_EVENT_WINDOW_MOVED:
      g.overlay.setWindowPos(e.window.data1, e.window.data2);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      if (g.webFocused) g.overlay.setFocus(true);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      if (g.webFocused) g.overlay.setFocus(false);
      break;
    case SDL_EVENT_WINDOW_MAXIMIZED:
      g.overlay.runJS("__winState && __winState({maximized:true})");
      break;
    case SDL_EVENT_WINDOW_RESTORED:
      g.overlay.runJS("__winState && __winState({maximized:false})");
      break;
    default:
      break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  // CEF subprocesses re-enter this executable; nothing else may run first.
  int sub = Overlay::ExecuteSubProcess(argc, argv);
  if (sub >= 0) return sub;

  int winW = 1600, winH = 900;
  bool extBeginFrame = true;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--selftest")) g.selftest = true;
    else if (!std::strcmp(argv[i], "--native")) g.frameless = false;
    else if (!std::strcmp(argv[i], "--no-ext-bf")) extBeginFrame = false;
    else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) g.shotPath = argv[++i];
    else if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
      std::sscanf(argv[++i], "%dx%d", &winW, &winH);
  }

  // Prefer X11: NVIDIA GLX interop is the proven path, and the frameless
  // move/resize protocol plus _NET_WM_SYNC_REQUEST need it.
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
  }
  const char* driver = SDL_GetCurrentVideoDriver();
  std::printf("[sdl] video driver: %s\n", driver);
  if (g.frameless && std::strcmp(driver, "x11") != 0) {
    std::fprintf(stderr, "[sdl] not on X11 — using native decorations\n");
    g.frameless = false;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
  if (g.frameless) flags |= SDL_WINDOW_BORDERLESS;
  g.window = SDL_CreateWindow("WebCUDA — CUDA × OpenGL × Web UI", winW, winH,
                              flags);
  if (!g.window) {
    std::fprintf(stderr, "window creation failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetWindowMinimumSize(g.window, 720, 420);
  g.glctx = SDL_GL_CreateContext(g.window);
  if (!g.glctx || !SDL_GL_MakeCurrent(g.window, g.glctx)) {
    std::fprintf(stderr, "GL context failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetSwapInterval(1);
  if (!initGLFunctions()) return 1;
  std::printf("[gl] renderer: %s\n", (const char*)glGetString(GL_RENDERER));

  if (!std::strcmp(driver, "x11")) {
    SDL_PropertiesID props = SDL_GetWindowProperties(g.window);
    g.xdpy = (Display*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    g.xwin = (::Window)SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
  }
  if (g.xdpy) {
    // Anchor stale buffer content top-left during resize (backup for WMs
    // without the sync protocol; with it, mismatch frames don't occur).
    XSetWindowAttributes wa{};
    wa.bit_gravity = NorthWestGravity;
    XChangeWindowAttributes(g.xdpy, g.xwin, CWBitGravity, &wa);
  }

  SDL_GetWindowSizeInPixels(g.window, &g.fbW, &g.fbH);
  g.simW = g.fbW;
  g.simH = g.fbH;

  // cursors (indices match onCursorChange/edge mapping)
  const SDL_SystemCursor shapes[] = {
      SDL_SYSTEM_CURSOR_DEFAULT,     SDL_SYSTEM_CURSOR_TEXT,
      SDL_SYSTEM_CURSOR_CROSSHAIR,   SDL_SYSTEM_CURSOR_POINTER,
      SDL_SYSTEM_CURSOR_EW_RESIZE,   SDL_SYSTEM_CURSOR_NS_RESIZE,
      SDL_SYSTEM_CURSOR_NWSE_RESIZE, SDL_SYSTEM_CURSOR_NESW_RESIZE,
      SDL_SYSTEM_CURSOR_MOVE,        SDL_SYSTEM_CURSOR_NOT_ALLOWED};
  for (int i = 0; i < 10; i++) g.cursors[i] = SDL_CreateSystemCursor(shapes[i]);
  g.webCursor = g.cursors[0];

  // simulation texture (CUDA writes into it)
  unsigned simTex = 0;
  glGenTextures(1, &simTex);
  glBindTexture(GL_TEXTURE_2D, simTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.simW, g.simH, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);

  if (!simInit(g.simW, g.simH, simTex)) return 1;
  std::printf("[cuda] device: %s, sim %dx%d\n", simDeviceName(), g.simW, g.simH);

  unsigned prog = buildProgram(kVS, kFS);
  if (!prog) return 1;
  unsigned vao = 0;
  glf.GenVertexArrays(1, &vao);
  int texLoc = glf.GetUniformLocation(prog, "tex");
  int uvScaleLoc = glf.GetUniformLocation(prog, "uvScale");

  // monitor refresh -> overlay frame rate
  const SDL_DisplayMode* mode =
      SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
  g.hz = (mode && mode->refresh_rate > 1.f)
             ? std::clamp((int)std::lround(mode->refresh_rate), 30, 240)
             : 60;
  const int hz = g.hz;

  Overlay::Config cfg;
  cfg.width = g.fbW;
  cfg.height = g.fbH;
  cfg.frameRate = hz;
  cfg.externalBeginFrame = extBeginFrame;
  std::string dir = exeDir();
  cfg.url = "file://" + dir + "/ui/index.html" +
            (g.frameless ? "?frameless=1" : "");
  // selftest runs get an isolated cache: a killed run leaves Chromium's
  // singleton lock behind, and the next launch would forward itself to the
  // "existing instance" instead of starting
  cfg.cachePath = g.selftest
                      ? "/tmp/webcuda-selftest-" + std::to_string(getpid())
                      : dir + "/cef_cache";
  cfg.resourceDir = dir;
  cfg.localesDir = dir + "/locales";
  if (!g.overlay.init(cfg, onQuery, onCursorChange)) return 1;
  std::printf("[cef] overlay up, %d Hz, external begin frames: %s\n", hz,
              extBeginFrame ? "on" : "off");

  {
    int wx = 0, wy = 0;
    SDL_GetWindowPosition(g.window, &wx, &wy);
    g.overlay.setWindowPos(wx, wy);
  }

  auto tPrev = std::chrono::steady_clock::now();
  auto tPaintWindow = tPrev;
  uint64_t paintWindowStart = 0;
  uint64_t frame = 0;
  double lastSimStep = 0;

  g.render = [&](bool stepSim) -> bool {
    g.overlay.pumpWork();
    if (g.fbW <= 0 || g.fbH <= 0) return false;
    double now = nowSec();

    // Vsync stays ON even during interactive resize. The WM's sync request is
    // acknowledged inside SDL_GL_SwapWindow; with vsync off the swap returns
    // while the frame is still queued, the ack races ahead of the actual
    // present, and the compositor shows a stale-size buffer — visible as the
    // UI boxes jumping vertically. A blocking swap keeps ack ≈ present, and
    // the sync protocol itself paces the resize to our frame rate.
    bool fast = now - g.lastResizeTime < 0.25;
    g.overlay.setResizeActive(fast);

    g.overlay.beginFrame();  // exactly one begin-frame per presented frame

    // ---- CUDA simulation -> GL texture ----
    // Advance the sim at most at refresh rate so resize frames (and the
    // uncapped loop) don't fast-forward it; skipped frames only re-colorize.
    SimParams p = g.params;
    SimStats scratch;
    bool advance = stepSim && now - lastSimStep >= 0.7 / hz;
    if (advance) lastSimStep = now;
    if (!advance) p.paused = true;
    simFrame(p, advance ? g.simStats : scratch);
    if (const unsigned char* px = simPixels()) {  // no-interop fallback
      glBindTexture(GL_TEXTURE_2D, simTex);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g.simW, g.simH, GL_RGBA,
                      GL_UNSIGNED_BYTE, px);
    }

    // ---- web overlay dirty-rect upload ----
    double upMs = g.overlay.uploadDirty();
    if (upMs > 0) g.uploadMsAvg = g.uploadMsAvg * 0.9 + upMs * 0.1;

    // ---- composite ----
    glViewport(0, 0, g.fbW, g.fbH);
    glClearColor(0.02f, 0.02f, 0.03f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glf.UseProgram(prog);
    glf.BindVertexArray(vao);
    glf.ActiveTexture(GL_TEXTURE0);
    glf.Uniform1i(texLoc, 0);

    glDisable(GL_BLEND);  // simulation: stretch to fill
    glf.Uniform2f(uvScaleLoc, 1.f, 1.f);
    glBindTexture(GL_TEXTURE_2D, simTex);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_BLEND);  // CEF output is premultiplied alpha
    glf.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                          GL_ONE_MINUS_SRC_ALPHA);
    // overlay: pixel-exact, top-left anchored (no rubber-banding while
    // CEF's repaint chases the window size during live resize)
    glf.Uniform2f(uvScaleLoc, (float)g.fbW / g.overlay.texWidth(),
                  (float)g.fbH / g.overlay.texHeight());
    glBindTexture(GL_TEXTURE_2D, g.overlay.texture());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);

    if (g.pendingShot) {
      writePPM(g.shotPath.c_str(), g.fbW, g.fbH);
      g.pendingShot = false;
    }
    SDL_GL_SwapWindow(g.window);  // also acks the WM's resize sync request
    if (fast) glFinish();  // ensure the present completed before the next ack
    g.presentCount++;

    if (!stepSim || fast) return true;  // resize/callback frames: no stats

    // ---- timing / stats ----
    auto tNow = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(tNow - tPrev).count();
    tPrev = tNow;
    g.frameMs = g.frameMs * 0.92 + dt * 1000.0 * 0.08;
    g.fps = g.fps * 0.92 + (dt > 0 ? 1.0 / dt : 0) * 0.08;

    double paintWin = std::chrono::duration<double>(tNow - tPaintWindow).count();
    if (paintWin >= 1.0) {
      g.paintsPerSec = (g.overlay.paintCount() - paintWindowStart) / paintWin;
      paintWindowStart = g.overlay.paintCount();
      tPaintWindow = tNow;
    }

    if (++frame % 15 == 0 && g.overlay.loaded()) {
      char js[512];
      std::snprintf(js, sizeof js,
                    "window.__stats && __stats({fps:%.1f,frameMs:%.2f,"
                    "cudaMs:%.2f,paints:%.0f,upMs:%.2f,sim:'%dx%d',gpu:'%s'})",
                    g.fps, g.frameMs, g.simStats.kernelMs, g.paintsPerSec,
                    g.uploadMsAvg, g.simW, g.simH, simDeviceName());
      g.overlay.runJS(js);
    }
    return true;
  };

  while (!g.shouldQuit) {
    uint64_t pc0 = g.presentCount;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handleEvent(e);
    if (g.shouldQuit) break;

    // a resize/expose event may have rendered already; with vsync on, a
    // second swap this cycle would just block and add latency
    bool presented =
        g.presentCount != pc0 ? true : g.render(true);
    if (!presented) {  // nothing rendered (e.g. zero-size window): don't spin
      SDL_Delay(4);
      continue;
    }

    // ---- selftest state machine: native-driven drag of a web window ----
    // (steps once per *presented* frame, never per loop iteration)
    if (g.selftest) {
      switch (g.selftestPhase) {
        case 0:
          if (g.overlay.loaded() && g.uiReady) {
            g.selftestPhase = 1;
            g.selftestFrame = 0;
          }
          break;
        case 1:  // settle, then press on the titlebar
          if (++g.selftestFrame >= 90) {
            g.overlay.mouseMove((int)g.dragPointX, (int)g.dragPointY, 0);
            g.overlay.mouseButton((int)g.dragPointX, (int)g.dragPointY, 0,
                                  true, 1, kModLeftMouse);
            g.dragStartPaints = g.overlay.paintCount();
            g.dragStartTime = std::chrono::steady_clock::now();
            g.selftestPhase = 2;
            g.selftestFrame = 0;
          }
          break;
        case 2: {  // drag diagonally, one move per frame
          g.selftestFrame++;
          double px = g.dragPointX + g.selftestFrame * 2.0;
          double py = g.dragPointY + std::sin(g.selftestFrame * 0.05) * 60.0 +
                      g.selftestFrame * 0.4;
          g.overlay.mouseMove((int)px, (int)py, kModLeftMouse);
          if (g.selftestFrame >= g.dragFrames) {
            double secs = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() -
                              g.dragStartTime)
                              .count();
            uint64_t paints = g.overlay.paintCount() - g.dragStartPaints;
            double pps = paints / std::max(1e-6, secs);
            double expect = g.dragFrames / std::max(1e-6, secs);
            std::printf(
                "[selftest] drag: %llu paints in %.2fs = %.1f paints/s "
                "(render %.1f fps) -> %s\n",
                (unsigned long long)paints, secs, pps, expect,
                pps >= 0.75 * expect ? "PASS" : "FAIL");
            g.overlay.mouseButton((int)px, (int)py, 0, false, 1, 0);
            g.selftestPhase = 3;
            g.selftestFrame = 0;
          }
          break;
        }
        case 3: {  // oscillating live resize + probe: do the UI boxes move?
          g.selftestFrame++;
          int w, h;
          SDL_GetWindowSize(g.window, &w, &h);
          // frames 1..40: settle (drag-tail pointer events must drain first,
          // or leftover box motion contaminates the probe)
          if (g.selftestFrame > 40 && g.selftestFrame <= 80)
            SDL_SetWindowSize(g.window, w, h - 4);
          else if (g.selftestFrame > 80 && g.selftestFrame <= 120)
            SDL_SetWindowSize(g.window, w + 6, h + 4);
          if (g.selftestFrame > 40) {
            // probe Chromium's output (CPU mirror): topbar bottom edge at the
            // left margin, dragged sim-window top edge at its titlebar column
            int bar = g.overlay.probeAlphaEdge(12, 20, 120, false);
            int box = g.overlay.probeAlphaEdge(
                (int)(g.dragPointX + g.dragFrames * 2.0), 45, g.fbH - 10, true);
            if (bar >= 0) {
              g.probeBarMin = std::min(g.probeBarMin, bar);
              g.probeBarMax = std::max(g.probeBarMax, bar);
            }
            if (box >= 0) {
              g.probeBoxMin = std::min(g.probeBoxMin, box);
              g.probeBoxMax = std::max(g.probeBoxMax, box);
            }
          }
          if (g.selftestFrame == 130) {
            std::printf(
                "[selftest] resize probe: topbar bottom %d..%d (drift %d), "
                "box top %d..%d (drift %d), stretched frames filtered: %llu\n",
                g.probeBarMin, g.probeBarMax, g.probeBarMax - g.probeBarMin,
                g.probeBoxMin, g.probeBoxMax, g.probeBoxMax - g.probeBoxMin,
                (unsigned long long)g.overlay.skippedUploads());
          }
          // exercise collapse/expand on the perf window through the REAL
          // pointer path, aimed at the chevron ICON center (clicks landing on
          // the svg used to start a titlebar drag and get swallowed).
          // Perf titlebar: right edge at fbW-40; buttons: [collapse][close],
          // 22px wide, 8px gap, 6px padding -> collapse center ~ fbW-87.
          if (g.selftestFrame == 140 || g.selftestFrame == 165) {
            int cx = g.fbW - 87, cy = 86;
            g.overlay.mouseMove(cx, cy, 0);
            g.overlay.mouseButton(cx, cy, 0, true, 1, kModLeftMouse);
            g.overlay.mouseButton(cx, cy, 0, false, 1, 0);
          }
          if (g.selftestFrame == 160)  // body gone after collapse?
            g.tCollapsed = !g.overlay.uiAt(g.fbW - 200, 160);
          if (g.selftestFrame == 185)  // body back after expand?
            g.tExpanded = g.overlay.uiAt(g.fbW - 200, 160);
          // after settling: did the anchored windows track their borders?
          // Performance is right-anchored (gap 40), About bottom-anchored
          // (gap 70) — the window resized +240 x / net -160 y meanwhile.
          if (g.selftestFrame >= 185) {
            std::printf(
                "[selftest] collapse via pointer click: collapse %s, expand "
                "%s\n",
                g.tCollapsed ? "PASS" : "FAIL", g.tExpanded ? "PASS" : "FAIL");
            int perfRight =
                g.overlay.probeAlphaEdgeRow(86, g.fbW - 2, g.fbW - 250, true);
            int aboutBottom =
                g.overlay.probeAlphaEdge(g.fbW / 2, g.fbH - 2, g.fbH - 250, true);
            int gapR = perfRight < 0 ? -1 : g.fbW - 1 - perfRight;
            int gapB = aboutBottom < 0 ? -1 : g.fbH - 1 - aboutBottom;
            std::printf(
                "[selftest] anchors after resize: perf right gap %d (want 40), "
                "about bottom gap %d (want 70) -> %s\n",
                gapR, gapB,
                (std::abs(gapR - 40) <= 2 && std::abs(gapB - 70) <= 2)
                    ? "PASS"
                    : "FAIL");
            g.selftestPhase = 4;
            g.pendingShot = true;
          }
          break;
        }
        case 4:
          break;
      }
      if (g.selftestPhase == 4) {
        if (g.pendingShot) g.render(false);  // flush the screenshot
        break;
      }
    }
  }

  simShutdown();
  g.overlay.shutdown();
  SDL_GL_DestroyContext(g.glctx);
  SDL_DestroyWindow(g.window);
  SDL_Quit();
  return 0;
}
