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

  bool loaded() const;
  void runJS(const std::string& code);

  // Upload pending dirty rects into the GL texture. Returns ms spent (0 if
  // nothing was dirty). Call with the GL context current.
  double uploadDirty();

  unsigned texture() const { return tex_; }
  int texWidth() const { return texW_; }
  int texHeight() const { return texH_; }
  uint64_t paintCount() const;  // total OnPaint calls (for diagnostics)

 private:
  unsigned tex_ = 0;
  int texW_ = 0, texH_ = 0;
};

// Translate helpers used by main.cpp
uint32_t cefModifiersFromGlfw(int glfwMods, bool l, bool m, bool r);
int windowsKeyCodeFromGlfw(int glfwKey);

// Mirrors EVENTFLAG_LEFT_MOUSE_BUTTON (keeps CEF headers out of main.cpp).
constexpr uint32_t kModLeftMouse = 1u << 4;
