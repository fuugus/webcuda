// webcuda — unified graphics pipeline demo
//   CUDA simulation -> OpenGL texture (zero-copy interop)
//   CEF web overlay -> transparent UI layer in the same swapchain
//
// One OS window. The web layer provides draggable/resizable in-app windows;
// input is routed to web or simulation based on overlay alpha under cursor.

#include <X11/Xlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gl_funcs.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "overlay.h"
#include "sim.h"

#include <limits.h>
#include <unistd.h>

namespace {

struct AppState {
  GLFWwindow* window = nullptr;
  Overlay overlay;
  SimParams params;
  SimStats simStats;

  int fbW = 0, fbH = 0;
  int simW = 0, simH = 0;

  // input routing
  double mouseX = 0, mouseY = 0;
  bool btn[3] = {false, false, false};
  bool webCaptured = false;    // mouse-down started on web UI
  bool sceneCaptured = false;  // mouse-down started on simulation
  bool webFocused = false;
  int glfwMods = 0;
  double lastClickTime = 0;
  double lastClickX = 0, lastClickY = 0;
  int clickCount = 1;

  // cursors
  GLFWcursor* cursors[10] = {};
  GLFWcursor* webCursor = nullptr;
  GLFWcursor* current = nullptr;

  // windowed <-> fullscreen
  int savedX = 0, savedY = 0, savedW = 0, savedH = 0;

  // stats
  double fps = 0, frameMs = 0;
  uint64_t lastPaintCount = 0;
  double paintsPerSec = 0;
  double uploadMsAvg = 0;

  // selftest
  bool selftest = false;
  int selftestPhase = 0;  // 0 wait-ready, 1 settle, 2 drag, 3 settle, 4 done
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
  return cefModifiersFromGlfw(g.glfwMods, g.btn[0], g.btn[1], g.btn[2]);
}

void brushAt(double x, double y) {
  double sx = x / std::max(1, g.fbW) * g.simW;
  double sy = y / std::max(1, g.fbH) * g.simH;
  simBrush((float)sx, (float)sy, g.params.brushRadius);
}

// ---------------------------------------------------------------------------
// GLFW callbacks
// ---------------------------------------------------------------------------
void onCursorPos(GLFWwindow*, double x, double y) {
  g.mouseX = x;
  g.mouseY = y;
  if (!g.sceneCaptured)  // keep hover states correct on the web layer
    g.overlay.mouseMove((int)x, (int)y, mods());
  if (g.sceneCaptured && g.btn[0]) brushAt(x, y);

  // cursor shape: web cursor over UI, crosshair over the simulation
  GLFWcursor* want =
      (g.webCaptured || (!g.sceneCaptured && g.overlay.uiAt((int)x, (int)y)))
          ? g.webCursor
          : g.cursors[2];  // crosshair
  if (want != g.current) {
    g.current = want;
    glfwSetCursor(g.window, want);
  }
}

void onMouseButton(GLFWwindow*, int button, int action, int m) {
  g.glfwMods = m;
  if (button > 2) return;
  bool down = action == GLFW_PRESS;
  g.btn[button] = down;

  if (down) {
    // double-click detection (text selection etc.)
    double t = glfwGetTime();
    if (t - g.lastClickTime < 0.4 && std::abs(g.mouseX - g.lastClickX) < 4 &&
        std::abs(g.mouseY - g.lastClickY) < 4)
      g.clickCount = std::min(3, g.clickCount + 1);
    else
      g.clickCount = 1;
    g.lastClickTime = t;
    g.lastClickX = g.mouseX;
    g.lastClickY = g.mouseY;

    bool onUi = g.overlay.uiAt((int)g.mouseX, (int)g.mouseY);
    if (onUi) {
      g.webCaptured = true;
      if (!g.webFocused) {
        g.webFocused = true;
        g.overlay.setFocus(true);
      }
      g.overlay.mouseButton((int)g.mouseX, (int)g.mouseY, button, true,
                            g.clickCount, mods());
    } else {
      g.sceneCaptured = true;
      if (g.webFocused) {
        g.webFocused = false;
        g.overlay.setFocus(false);
      }
      if (button == 0) brushAt(g.mouseX, g.mouseY);
    }
  } else {
    if (g.webCaptured) {
      g.overlay.mouseButton((int)g.mouseX, (int)g.mouseY, button, false,
                            g.clickCount, mods());
    }
    if (!g.btn[0] && !g.btn[1] && !g.btn[2]) {
      g.webCaptured = false;
      g.sceneCaptured = false;
    }
  }
}

void onScroll(GLFWwindow*, double dx, double dy) {
  if (g.overlay.uiAt((int)g.mouseX, (int)g.mouseY)) {
    g.overlay.mouseWheel((int)g.mouseX, (int)g.mouseY, dx, dy, mods());
  } else {
    g.params.brushRadius =
        std::clamp(g.params.brushRadius + (float)dy * 2.f, 4.f, 80.f);
  }
}

void toggleFullscreen() {
  GLFWmonitor* mon = glfwGetWindowMonitor(g.window);
  if (mon) {
    glfwSetWindowMonitor(g.window, nullptr, g.savedX, g.savedY, g.savedW,
                         g.savedH, 0);
  } else {
    glfwGetWindowPos(g.window, &g.savedX, &g.savedY);
    glfwGetWindowSize(g.window, &g.savedW, &g.savedH);
    GLFWmonitor* primary = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(primary);
    glfwSetWindowMonitor(g.window, primary, 0, 0, mode->width, mode->height,
                         mode->refreshRate);
  }
}

void onKey(GLFWwindow*, int key, int scancode, int action, int m) {
  g.glfwMods = m;
  if (action == GLFW_PRESS) {
    if (key == GLFW_KEY_F11) {
      toggleFullscreen();
      return;
    }
    if (key == GLFW_KEY_ESCAPE && !g.webFocused) {
      g.shouldQuit = true;
      return;
    }
  }
  if (!g.webFocused) return;
  int wkc = windowsKeyCodeFromGlfw(key);
  if (!wkc) return;
  if (action == GLFW_PRESS || action == GLFW_REPEAT)
    g.overlay.keyEvent(true, wkc, scancode, mods());
  else if (action == GLFW_RELEASE)
    g.overlay.keyEvent(false, wkc, scancode, mods());
}

void onChar(GLFWwindow*, unsigned codepoint) {
  if (g.webFocused) g.overlay.charEvent(codepoint, mods());
}

void onFramebufferSize(GLFWwindow*, int w, int h) {
  g.fbW = w;
  g.fbH = h;
  if (w > 0 && h > 0) g.overlay.resize(w, h);
}

void onWindowFocus(GLFWwindow*, int focused) {
  if (g.webFocused) g.overlay.setFocus(focused != 0);
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
  GLFWcursor* c = g.cursors[0];
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

const char* kFS = R"(#version 330 core
in vec2 uv;
out vec4 frag;
uniform sampler2D tex;
void main() { frag = texture(tex, uv); })";

}  // namespace

int main(int argc, char** argv) {
  // CEF subprocesses re-enter this executable; nothing else may run first.
  int sub = Overlay::ExecuteSubProcess(argc, argv);
  if (sub >= 0) return sub;

  XInitThreads();

  int winW = 1600, winH = 900;
  bool extBeginFrame = true;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--selftest")) g.selftest = true;
    else if (!std::strcmp(argv[i], "--no-ext-bf")) extBeginFrame = false;
    else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) g.shotPath = argv[++i];
    else if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
      std::sscanf(argv[++i], "%dx%d", &winW, &winH);
  }

  glfwSetErrorCallback([](int code, const char* msg) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, msg);
  });
  glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
  if (!glfwInit()) {
    glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    if (!glfwInit()) {
      std::fprintf(stderr, "glfwInit failed\n");
      return 1;
    }
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  const char* title = "WebCUDA — CUDA × OpenGL × Web UI";
  g.window = glfwCreateWindow(winW, winH, title, nullptr, nullptr);
  if (!g.window && glfwGetPlatform() == GLFW_PLATFORM_X11) {
    // GLX may be broken (e.g. NVIDIA driver mismatch) while Wayland/EGL works
    std::fprintf(stderr, "[glfw] X11/GLX context failed, retrying Wayland\n");
    glfwTerminate();
    glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
    if (glfwInit()) {
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
      g.window = glfwCreateWindow(winW, winH, title, nullptr, nullptr);
    }
  }
  if (!g.window) {
    std::fprintf(stderr, "window creation failed\n");
    return 1;
  }
  glfwMakeContextCurrent(g.window);
  glfwSwapInterval(1);
  if (!initGLFunctions()) return 1;
  std::printf("[gl] renderer: %s\n", (const char*)glGetString(GL_RENDERER));

  glfwGetFramebufferSize(g.window, &g.fbW, &g.fbH);
  g.simW = g.fbW;
  g.simH = g.fbH;

  // cursors
  const int shapes[] = {GLFW_ARROW_CURSOR,       GLFW_IBEAM_CURSOR,
                        GLFW_CROSSHAIR_CURSOR,   GLFW_POINTING_HAND_CURSOR,
                        GLFW_RESIZE_EW_CURSOR,   GLFW_RESIZE_NS_CURSOR,
                        GLFW_RESIZE_NWSE_CURSOR, GLFW_RESIZE_NESW_CURSOR,
                        GLFW_RESIZE_ALL_CURSOR,  GLFW_NOT_ALLOWED_CURSOR};
  for (int i = 0; i < 10; i++) g.cursors[i] = glfwCreateStandardCursor(shapes[i]);
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

  // monitor refresh -> overlay frame rate
  const GLFWvidmode* mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
  int hz = mode ? std::clamp(mode->refreshRate, 30, 240) : 60;

  Overlay::Config cfg;
  cfg.width = g.fbW;
  cfg.height = g.fbH;
  cfg.frameRate = hz;
  cfg.externalBeginFrame = extBeginFrame;
  std::string dir = exeDir();
  cfg.url = "file://" + dir + "/ui/index.html";
  cfg.cachePath = dir + "/cef_cache";
  cfg.resourceDir = dir;
  cfg.localesDir = dir + "/locales";
  if (!g.overlay.init(cfg, onQuery, onCursorChange)) return 1;
  std::printf("[cef] overlay up, %d Hz, external begin frames: %s\n", hz,
              extBeginFrame ? "on" : "off");

  glfwSetCursorPosCallback(g.window, onCursorPos);
  glfwSetMouseButtonCallback(g.window, onMouseButton);
  glfwSetScrollCallback(g.window, onScroll);
  glfwSetKeyCallback(g.window, onKey);
  glfwSetCharCallback(g.window, onChar);
  glfwSetFramebufferSizeCallback(g.window, onFramebufferSize);
  glfwSetWindowFocusCallback(g.window, onWindowFocus);

  auto tPrev = std::chrono::steady_clock::now();
  auto tPaintWindow = tPrev;
  uint64_t paintWindowStart = 0;
  uint64_t frame = 0;

  while (!glfwWindowShouldClose(g.window) && !g.shouldQuit) {
    glfwPollEvents();

    if (glfwGetPlatform() == GLFW_PLATFORM_X11) {  // Wayland has no window pos
      int wx, wy;
      glfwGetWindowPos(g.window, &wx, &wy);
      g.overlay.setWindowPos(wx, wy);
    }

    g.overlay.pumpWork();
    g.overlay.beginFrame();

    // ---- selftest state machine: native-driven drag of a web window ----
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
        case 3:
          if (++g.selftestFrame >= 30) g.selftestPhase = 4;
          break;
      }
    }

    // ---- CUDA simulation -> GL texture ----
    if (g.fbW > 0 && g.fbH > 0) {
      simFrame(g.params, g.simStats);
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

      glDisable(GL_BLEND);
      glBindTexture(GL_TEXTURE_2D, simTex);
      glDrawArrays(GL_TRIANGLES, 0, 3);

      glEnable(GL_BLEND);  // CEF output is premultiplied alpha
      glf.BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, g.overlay.texture());
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glDisable(GL_BLEND);
    }

    if (g.selftest && g.selftestPhase == 4) {
      writePPM(g.shotPath.c_str(), g.fbW, g.fbH);
      break;
    }

    glfwSwapBuffers(g.window);

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
  }

  simShutdown();
  g.overlay.shutdown();
  glfwDestroyWindow(g.window);
  glfwTerminate();
  return 0;
}
