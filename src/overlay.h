// CEF off-screen-rendered web overlay, composited as a GL texture.
//
// Smoothness recipe (the part that fixed laggy window dragging):
//  * single-threaded CEF message pump driven from the render loop
//  * external begin frames: CEF paints exactly once per vsync, in lockstep
//  * OnPaint keeps a CPU mirror; only dirty rects are uploaded to the texture
//  * input routed by alpha of the mirror pixel under the cursor
#pragma once

#include <cstdint>
#include <functional>
#include <string>

class Overlay {
 public:
  struct Config {
    int width = 0, height = 0;
    std::string url;
    int frameRate = 60;              // windowless frame rate cap
    bool externalBeginFrame = true;  // sync paints to our render loop
    std::string cachePath;
    std::string resourceDir;
    std::string localesDir;
  };

  // request -> response; return true if handled.
  using QueryHandler = std::function<bool(const std::string&, std::string&)>;
  using CursorCallback = std::function<void(int /*cef_cursor_type_t*/)>;

  // Must be the FIRST thing in main(). Returns >=0 if this process was a CEF
  // subprocess (caller must exit with that code), -1 for the browser process.
  static int ExecuteSubProcess(int argc, char** argv);

  // Requires a current GL context (creates the overlay texture).
  bool init(const Config& cfg, QueryHandler onQuery, CursorCallback onCursor);
  void shutdown();

  // Once per frame, in this order: pumpWork() then beginFrame().
  void pumpWork();
  void beginFrame();

  void resize(int w, int h);
  void setWindowPos(int x, int y);  // for GetScreenPoint (menus, tooltips)

  // Block-pump CEF until a clean (non-stretched, see uploadDirty) paint of
  // exactly w x h landed in the CPU mirror, or timeoutMs elapsed. Returns
  // true if the paint arrived. Called between resize() and the redraw of a
  // live-resize step: Chromium's relayout for the new size is asynchronous,
  // and presenting before it lands shows the previous layout anchored
  // top-left — right/bottom-anchored UI visibly lags the dragged border.
  // A DOM-only UI relayouts in a few ms, so waiting for the matching frame
  // makes every anchor pixel-exact; on timeout the caller just presents the
  // stale frame exactly as before.
  bool waitCleanPaint(int w, int h, double timeoutMs);

  // ---- input (coordinates in framebuffer pixels) ----
  void mouseMove(int x, int y, uint32_t cefModifiers);
  void mouseButton(int x, int y, int cefButton /*0=L 1=M 2=R*/, bool down,
                   int clickCount, uint32_t cefModifiers);
  void mouseWheel(int x, int y, double dx, double dy, uint32_t cefModifiers);
  void keyEvent(bool down, int windowsKeyCode, int nativeKeyCode,
                uint32_t cefModifiers);
  void charEvent(unsigned codePoint, uint32_t cefModifiers);
  void setFocus(bool focused);

  // True if the overlay has visible content (alpha > threshold) at the pixel.
  bool uiAt(int x, int y) const;

  // Diagnostics for the resize self-test: scan the CPU mirror for the first
  // pixel whose alpha crosses 200 (rising if `opaque`, falling if not);
  // returns -1 if none. Ranges may be reversed (scans backward).
  int probeAlphaEdge(int x, int y0, int y1, bool opaque) const;     // column
  int probeAlphaEdgeRow(int y, int x0, int x1, bool opaque) const;  // row

  bool loaded() const;
  void runJS(const std::string& code);

  // Upload pending dirty rects into the GL texture. Returns ms spent (0 if
  // nothing was dirty). Call with the GL context current.
  // While resize-active, frames failing the stretch-sentinel check (see
  // ui/index.html) are skipped: Chromium emits transitional frames with the
  // old layout stretched to the new viewport, which made UI elements jump
  // vertically during live resize. Skipped frames stay dirty and upload as
  // soon as a clean relayout arrives.
  double uploadDirty();
  void setResizeActive(bool active);
  uint64_t skippedUploads() const;

  unsigned texture() const { return tex_; }
  int texWidth() const { return texW_; }
  int texHeight() const { return texH_; }
  uint64_t paintCount() const;  // total OnPaint calls (for diagnostics)

 private:
  unsigned tex_ = 0;
  int texW_ = 0, texH_ = 0;
};

// Translate helpers used by main.cpp (SDL_Keymod / SDL_Keycode as uint32)
uint32_t cefModifiersFromSDL(uint32_t sdlMods, bool l, bool m, bool r);
int windowsKeyCodeFromSDL(uint32_t sdlKey);

// Mirrors EVENTFLAG_LEFT_MOUSE_BUTTON (keeps CEF headers out of main.cpp).
constexpr uint32_t kModLeftMouse = 1u << 4;
