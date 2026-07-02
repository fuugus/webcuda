#include "overlay.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // CefMainArgs wants the HINSTANCE on Windows
#endif

#include "gl_funcs.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_render_process_handler.h"
#include "include/wrapper/cef_message_router.h"

namespace {

std::atomic<bool> g_pumpScheduled{true};

// ---------------------------------------------------------------------------
// CefApp — shared between browser process and subprocesses. Hosts the
// renderer-side message router (window.cefQuery binding).
// ---------------------------------------------------------------------------
class WebApp : public CefApp,
               public CefBrowserProcessHandler,
               public CefRenderProcessHandler {
 public:
  WebApp() = default;

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    // Software OSR path: with GPU compositing Chromium renders on the GPU and
    // reads back, adding latency; for a DOM-only UI software raster is faster.
    command_line->AppendSwitch("disable-gpu");
    command_line->AppendSwitch("disable-gpu-compositing");
#ifdef __linux__
    command_line->AppendSwitchWithValue("ozone-platform", "x11");
#endif
  }

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return this;
  }

  // BrowserProcessHandler
  void OnScheduleMessagePumpWork(int64_t delay_ms) override {
    if (delay_ms <= 0) g_pumpScheduled.store(true, std::memory_order_relaxed);
  }
  bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine>,
                                   const CefString&) override {
    // Another instance launched with the same cache dir. Without this,
    // Chromium "handles" the relaunch by opening a plain Chrome window.
    std::fprintf(stderr,
                 "[cef] second instance relaunch ignored (cache dir in use)\n");
    return true;
  }

  // RenderProcessHandler (runs in the renderer subprocess)
  void OnWebKitInitialized() override {
    CefMessageRouterConfig cfg;
    rendererRouter_ = CefMessageRouterRendererSide::Create(cfg);
  }
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override {
    if (rendererRouter_) rendererRouter_->OnContextCreated(browser, frame, context);
  }
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override {
    if (rendererRouter_) rendererRouter_->OnContextReleased(browser, frame, context);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    return rendererRouter_ &&
           rendererRouter_->OnProcessMessageReceived(browser, frame,
                                                     source_process, message);
  }

 private:
  CefRefPtr<CefMessageRouterRendererSide> rendererRouter_;
  IMPLEMENT_REFCOUNTING(WebApp);
};

// ---------------------------------------------------------------------------
// Browser-side query bridge: JS cefQuery(...) -> app callback
// ---------------------------------------------------------------------------
class QueryBridge : public CefMessageRouterBrowserSide::Handler {
 public:
  explicit QueryBridge(Overlay::QueryHandler h) : handler_(std::move(h)) {}
  bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
               int64_t query_id, const CefString& request, bool persistent,
               CefRefPtr<Callback> callback) override {
    std::string response;
    if (handler_ && handler_(request.ToString(), response)) {
      callback->Success(response);
      return true;
    }
    return false;
  }

 private:
  Overlay::QueryHandler handler_;
};

// ---------------------------------------------------------------------------
// Client: render handler (OSR), lifespan, load, display, context menu
// ---------------------------------------------------------------------------
struct DirtyRect {
  int x, y, w, h;
};

class OverlayClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler,
                      public CefContextMenuHandler,
                      public CefRequestHandler {
 public:
  OverlayClient(int w, int h, Overlay::QueryHandler onQuery,
                Overlay::CursorCallback onCursor)
      : width_(w), height_(h), cursorCb_(std::move(onCursor)) {
    CefMessageRouterConfig cfg;
    router_ = CefMessageRouterBrowserSide::Create(cfg);
    bridge_ = std::make_unique<QueryBridge>(std::move(onQuery));
    router_->AddHandler(bridge_.get(), true);
  }

  // CefClient
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return this;
  }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override {
    return router_->OnProcessMessageReceived(browser, frame, source_process,
                                             message);
  }

  // CefRenderHandler ---------------------------------------------------------
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
    rect.Set(0, 0, width_ > 0 ? width_ : 1, height_ > 0 ? height_ : 1);
  }
  bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override {
    info.device_scale_factor = 1.0f;
    info.rect = {0, 0, width_, height_};
    info.available_rect = info.rect;
    return true;
  }
  bool GetScreenPoint(CefRefPtr<CefBrowser>, int viewX, int viewY, int& screenX,
                      int& screenY) override {
    screenX = viewX + winX_;
    screenY = viewY + winY_;
    return true;
  }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type,
               const RectList& dirtyRects, const void* buffer, int width,
               int height) override {
    if (type != PET_VIEW) return;  // popup widgets (select dropdowns) unused
    paintCount_++;
    const bool sizeChanged =
        (width != mirrorW_ || height != mirrorH_ || mirror_.empty());
    if (sizeChanged) {
      mirrorW_ = width;
      mirrorH_ = height;
      mirror_.assign((size_t)width * height * 4, 0);
      std::memcpy(mirror_.data(), buffer, mirror_.size());
      dirty_.clear();
      dirty_.push_back({0, 0, width, height});
      return;
    }
    const uint8_t* src = static_cast<const uint8_t*>(buffer);
    for (const auto& r : dirtyRects) {
      for (int row = 0; row < r.height; row++) {
        size_t off = ((size_t)(r.y + row) * width + r.x) * 4;
        std::memcpy(mirror_.data() + off, src + off, (size_t)r.width * 4);
      }
      dirty_.push_back({r.x, r.y, r.width, r.height});
    }
  }

  // CefLifeSpanHandler -------------------------------------------------------
  bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int,
                     const CefString& target_url, const CefString&,
                     CefLifeSpanHandler::WindowOpenDisposition, bool,
                     const CefPopupFeatures&, CefWindowInfo&,
                     CefRefPtr<CefClient>&, CefBrowserSettings&,
                     CefRefPtr<CefDictionaryValue>&, bool*) override {
    // never allow native popup windows over the overlay
    std::fprintf(stderr, "[cef] popup suppressed: %s\n",
                 target_url.ToString().c_str());
    return true;
  }
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    browser_ = browser;
  }
  bool DoClose(CefRefPtr<CefBrowser>) override { return false; }
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    router_->OnBeforeClose(browser);
    browser_ = nullptr;
    closed_ = true;
  }

  // CefLoadHandler -----------------------------------------------------------
  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                 int) override {
    if (frame->IsMain()) loaded_ = true;
  }
  void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode, const CefString& errorText,
                   const CefString& failedUrl) override {
    if (frame->IsMain())
      std::fprintf(stderr, "[cef] load error %d (%s) for %s\n", errorCode,
                   errorText.ToString().c_str(),
                   failedUrl.ToString().c_str());
  }

  // CefDisplayHandler --------------------------------------------------------
  bool OnCursorChange(CefRefPtr<CefBrowser>, CefCursorHandle,
                      cef_cursor_type_t type,
                      const CefCursorInfo&) override {
    if (cursorCb_) cursorCb_(static_cast<int>(type));
    return true;  // we own the cursor
  }
  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                        const CefString& message, const CefString& source,
                        int line) override {
    std::fprintf(stderr, "[ui:%d] %s (%s:%d)\n", (int)level,
                 message.ToString().c_str(), source.ToString().c_str(), line);
    return true;
  }

  // CefContextMenuHandler ----------------------------------------------------
  void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                           CefRefPtr<CefContextMenuParams>,
                           CefRefPtr<CefMenuModel> model) override {
    model->Clear();  // no native context menus over the overlay
  }

  // CefRequestHandler --------------------------------------------------------
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest>, bool, bool) override {
    router_->OnBeforeBrowse(browser, frame);
    return false;
  }
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status, int error_code,
                                 const CefString& error_string) override {
    std::fprintf(stderr, "[cef] renderer terminated (%d): %s — reloading\n",
                 (int)status, error_string.ToString().c_str());
    router_->OnRenderProcessTerminated(browser);
    browser->Reload();
  }

  // --------------------------------------------------------------------------
  CefRefPtr<CefBrowser> browser_;
  CefRefPtr<CefMessageRouterBrowserSide> router_;
  std::unique_ptr<QueryBridge> bridge_;
  Overlay::CursorCallback cursorCb_;

  int width_ = 0, height_ = 0;   // logical view size (== framebuffer px)
  int winX_ = 0, winY_ = 0;      // app window position on screen
  int mirrorW_ = 0, mirrorH_ = 0;
  std::vector<uint8_t> mirror_;  // BGRA CPU copy of the last composited frame
  std::vector<DirtyRect> dirty_;
  uint64_t paintCount_ = 0;
  bool loaded_ = false;
  bool closed_ = false;

  IMPLEMENT_REFCOUNTING(OverlayClient);
};

CefRefPtr<WebApp> g_app;
CefRefPtr<OverlayClient> g_client;
int g_argc = 0;
char** g_argv = nullptr;
bool g_cefInitialized = false;

// stretch-sentinel state (see Overlay::uploadDirty / ui/index.html)
constexpr int kSentinelX = 1, kSentinelY = 300;
uint8_t g_sentinel[4] = {};
bool g_sentinelLearned = false;
bool g_resizeActive = false;
uint64_t g_skippedUploads = 0;
bool g_externalBF = true;

const uint8_t* sentinelPixel() {
  if (!g_client || g_client->mirror_.empty()) return nullptr;
  if (kSentinelY >= g_client->mirrorH_ || kSentinelX >= g_client->mirrorW_)
    return nullptr;
  return g_client->mirror_.data() +
         ((size_t)kSentinelY * g_client->mirrorW_ + kSentinelX) * 4;
}

// size-echo marker (see ui/index.html): opaque pixel at (0,302) whose color
// encodes innerWidth/innerHeight. It is set synchronously in the UI's resize
// handler, so a paint carrying the echo for (w,h) provably contains the
// finished relayout for that size — including the JS-anchored windows. A
// compositor frame of the old layout at the new surface size (which passes
// the stretch sentinel!) still carries the old echo and is rejected.
constexpr int kEchoX = 0, kEchoY = 302;
bool g_echoLearned = false;  // echo present in this UI build

bool echoMatches(int w, int h) {
  if (!g_client || g_client->mirror_.empty()) return false;
  if (kEchoY >= g_client->mirrorH_ || kEchoX >= g_client->mirrorW_)
    return false;
  const uint8_t* p = g_client->mirror_.data() +
                     ((size_t)kEchoY * g_client->mirrorW_ + kEchoX) * 4;
  if (p[3] != 255) return false;  // mirror is BGRA; the echo is fully opaque
  int ew = ((p[0] >> 4) << 8) | p[2];
  int eh = ((p[0] & 15) << 8) | p[1];
  return ew == w && eh == h;
}

CefRefPtr<CefBrowserHost> host() {
  return g_client && g_client->browser_ ? g_client->browser_->GetHost()
                                        : nullptr;
}

}  // namespace

// ===========================================================================
int Overlay::ExecuteSubProcess(int argc, char** argv) {
  g_argc = argc;
  g_argv = argv;
  g_app = new WebApp();
#ifdef _WIN32
  CefMainArgs args(GetModuleHandle(nullptr));
#else
  CefMainArgs args(argc, argv);
#endif
  return CefExecuteProcess(args, g_app, nullptr);
}

bool Overlay::init(const Config& cfg, QueryHandler onQuery,
                   CursorCallback onCursor) {
  CefSettings settings;
  settings.windowless_rendering_enabled = 1;
  settings.external_message_pump = 1;
  settings.multi_threaded_message_loop = 0;
  settings.no_sandbox = 1;
  settings.background_color = 0;  // fully transparent
  settings.log_severity = LOGSEVERITY_WARNING;
  if (!cfg.cachePath.empty()) {
    CefString(&settings.root_cache_path) = cfg.cachePath;
    CefString(&settings.cache_path) = cfg.cachePath;
  }
  if (!cfg.resourceDir.empty())
    CefString(&settings.resources_dir_path) = cfg.resourceDir;
  if (!cfg.localesDir.empty())
    CefString(&settings.locales_dir_path) = cfg.localesDir;

#ifdef _WIN32
  CefMainArgs args(GetModuleHandle(nullptr));
#else
  CefMainArgs args(g_argc, g_argv);
#endif
  if (!CefInitialize(args, settings, g_app, nullptr)) {
    std::fprintf(stderr, "[cef] CefInitialize failed\n");
    return false;
  }
  g_cefInitialized = true;
  g_externalBF = cfg.externalBeginFrame;

  g_client = new OverlayClient(cfg.width, cfg.height, std::move(onQuery),
                               std::move(onCursor));

  CefWindowInfo wi;
  wi.SetAsWindowless(0);
  wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;  // required for windowless
  wi.external_begin_frame_enabled = cfg.externalBeginFrame ? 1 : 0;

  CefBrowserSettings bs;
  bs.windowless_frame_rate = cfg.frameRate;
  bs.background_color = 0;

  if (!CefBrowserHost::CreateBrowserSync(wi, g_client, cfg.url, bs, nullptr,
                                         nullptr)) {
    std::fprintf(stderr, "[cef] CreateBrowserSync failed\n");
    return false;
  }

  // Overlay texture (RGBA8; uploads use GL_BGRA source format)
  texW_ = cfg.width;
  texH_ = cfg.height;
  glGenTextures(1, &tex_);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW_, texH_, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, nullptr);
  return true;
}

void Overlay::shutdown() {
  if (g_client && g_client->browser_) {
    g_client->browser_->GetHost()->CloseBrowser(true);
    auto t0 = std::chrono::steady_clock::now();
    while (!g_client->closed_ &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
      CefDoMessageLoopWork();
    }
  }
  g_client = nullptr;
  if (g_cefInitialized) CefShutdown();
}

void Overlay::pumpWork() {
  // Called once per render frame; also honor explicit pump requests.
  CefDoMessageLoopWork();
  if (g_pumpScheduled.exchange(false, std::memory_order_relaxed))
    CefDoMessageLoopWork();
}

void Overlay::beginFrame() {
  if (auto h = host()) h->SendExternalBeginFrame();
}

void Overlay::resize(int w, int h) {
  if (!g_client || (w == g_client->width_ && h == g_client->height_)) return;
  g_client->width_ = w;
  g_client->height_ = h;
  if (auto h2 = host()) h2->WasResized();
}

bool Overlay::waitCleanPaint(int w, int h, double timeoutMs) {
  // Not reentrant: CEF's pump can dispatch window messages, and on Windows a
  // nested WM_SIZE would land back here through the resize event watch.
  static bool pumping = false;
  if (pumping || !g_client) return false;
  pumping = true;

  auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  double lastBf = -1e9;
  bool ok = false;
  for (;;) {
    if (g_client->mirrorW_ == w && g_client->mirrorH_ == h) {
      if (echoMatches(w, h)) {
        g_echoLearned = true;
        ok = true;  // relayout for exactly this size has landed
        break;
      }
      // no echo in this UI (older page): fall back to size + stretch check
      if (!g_echoLearned) {
        const uint8_t* sp = sentinelPixel();
        if (!sp || !g_sentinelLearned || std::memcmp(sp, g_sentinel, 4) == 0) {
          ok = true;
          break;
        }
      }
    }
    double el = elapsed();
    if (el >= timeoutMs) break;
    // begin frames give Chromium composite opportunities; pace them — one
    // per loop iteration would just be dropped
    if (g_externalBF && el - lastBf >= 2.0) {
      if (auto hst = host()) hst->SendExternalBeginFrame();
      lastBf = el;
    }
    CefDoMessageLoopWork();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pumping = false;
  return ok;
}

void Overlay::setWindowPos(int x, int y) {
  if (!g_client) return;
  g_client->winX_ = x;
  g_client->winY_ = y;
}

void Overlay::mouseMove(int x, int y, uint32_t mods) {
  if (auto h = host()) {
    CefMouseEvent e;
    e.x = x;
    e.y = y;
    e.modifiers = mods;
    h->SendMouseMoveEvent(e, false);
  }
}

void Overlay::mouseButton(int x, int y, int button, bool down, int clickCount,
                          uint32_t mods) {
  if (auto h = host()) {
    CefMouseEvent e;
    e.x = x;
    e.y = y;
    e.modifiers = mods;
    auto bt = button == 1 ? MBT_MIDDLE : (button == 2 ? MBT_RIGHT : MBT_LEFT);
    h->SendMouseClickEvent(e, bt, !down, clickCount);
  }
}

void Overlay::mouseWheel(int x, int y, double dx, double dy, uint32_t mods) {
  if (auto h = host()) {
    CefMouseEvent e;
    e.x = x;
    e.y = y;
    e.modifiers = mods;
    h->SendMouseWheelEvent(e, (int)(dx * 53), (int)(dy * 53));
  }
}

void Overlay::keyEvent(bool down, int windowsKeyCode, int nativeKeyCode,
                       uint32_t mods) {
  if (auto h = host()) {
    CefKeyEvent e;
    e.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    e.windows_key_code = windowsKeyCode;
    e.native_key_code = nativeKeyCode;
    e.modifiers = mods;
    h->SendKeyEvent(e);
  }
}

void Overlay::charEvent(unsigned codePoint, uint32_t mods) {
  if (auto h = host()) {
    CefKeyEvent e;
    e.type = KEYEVENT_CHAR;
    e.character = (char16_t)codePoint;
    e.unmodified_character = (char16_t)codePoint;
    e.windows_key_code = (int)codePoint;
    e.modifiers = mods;
    h->SendKeyEvent(e);
  }
}

void Overlay::setFocus(bool focused) {
  if (auto h = host()) h->SetFocus(focused);
}

bool Overlay::uiAt(int x, int y) const {
  if (!g_client || g_client->mirror_.empty()) return false;
  if (x < 0 || y < 0 || x >= g_client->mirrorW_ || y >= g_client->mirrorH_)
    return false;
  uint8_t a = g_client->mirror_[((size_t)y * g_client->mirrorW_ + x) * 4 + 3];
  return a > 24;
}

int Overlay::probeAlphaEdge(int x, int y0, int y1, bool opaque) const {
  if (!g_client || g_client->mirror_.empty()) return -1;
  const int mw = g_client->mirrorW_, mh = g_client->mirrorH_;
  if (x < 0 || x >= mw) return -1;
  int step = y0 <= y1 ? 1 : -1;
  for (int y = std::clamp(y0, 0, mh - 1); y != std::clamp(y1, 0, mh - 1);
       y += step) {
    bool hit = g_client->mirror_[((size_t)y * mw + x) * 4 + 3] > 200;
    if (hit == opaque) return y;
  }
  return -1;
}

int Overlay::probeAlphaEdgeRow(int y, int x0, int x1, bool opaque) const {
  if (!g_client || g_client->mirror_.empty()) return -1;
  const int mw = g_client->mirrorW_, mh = g_client->mirrorH_;
  if (y < 0 || y >= mh) return -1;
  int step = x0 <= x1 ? 1 : -1;
  for (int x = std::clamp(x0, 0, mw - 1); x != std::clamp(x1, 0, mw - 1);
       x += step) {
    bool hit = g_client->mirror_[((size_t)y * mw + x) * 4 + 3] > 200;
    if (hit == opaque) return x;
  }
  return -1;
}

bool Overlay::loaded() const { return g_client && g_client->loaded_; }

void Overlay::runJS(const std::string& code) {
  if (g_client && g_client->browser_) {
    auto frame = g_client->browser_->GetMainFrame();
    frame->ExecuteJavaScript(code, frame->GetURL(), 0);
  }
}

void Overlay::setResizeActive(bool active) { g_resizeActive = active; }
uint64_t Overlay::skippedUploads() const { return g_skippedUploads; }

double Overlay::uploadDirty() {
  if (!g_client || g_client->dirty_.empty() || g_client->mirror_.empty())
    return 0.0;

  // Stretch filter: during a live resize Chromium delivers transitional
  // frames with the previous layout scaled to the new size; displaying them
  // makes the UI jump. The sentinel is byte-exact only in clean relayouts.
  if (const uint8_t* sp = sentinelPixel()) {
    if (g_resizeActive && g_sentinelLearned) {
      if (std::memcmp(sp, g_sentinel, 4) != 0) {
        g_skippedUploads++;  // keep dirty_; a clean frame uploads it all
        return 0.0;
      }
    } else if (!g_resizeActive && sp[3] != 0) {
      std::memcpy(g_sentinel, sp, 4);
      g_sentinelLearned = true;
    }
  }

  auto t0 = std::chrono::steady_clock::now();

  glBindTexture(GL_TEXTURE_2D, tex_);
  const int mw = g_client->mirrorW_, mh = g_client->mirrorH_;
  if (mw != texW_ || mh != texH_) {
    texW_ = mw;
    texH_ = mh;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW_, texH_, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, nullptr);
    g_client->dirty_.clear();
    g_client->dirty_.push_back({0, 0, mw, mh});
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, mw);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  for (const auto& r : g_client->dirty_) {
    const uint8_t* p =
        g_client->mirror_.data() + ((size_t)r.y * mw + r.x) * 4;
    glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_BGRA,
                    GL_UNSIGNED_BYTE, p);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  g_client->dirty_.clear();

  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

uint64_t Overlay::paintCount() const {
  return g_client ? g_client->paintCount_ : 0;
}

// ===========================================================================
// SDL -> CEF translation helpers
// ===========================================================================
uint32_t cefModifiersFromSDL(uint32_t sdlMods, bool l, bool m, bool r) {
  uint32_t f = 0;
  if (sdlMods & SDL_KMOD_SHIFT) f |= EVENTFLAG_SHIFT_DOWN;
  if (sdlMods & SDL_KMOD_CTRL) f |= EVENTFLAG_CONTROL_DOWN;
  if (sdlMods & SDL_KMOD_ALT) f |= EVENTFLAG_ALT_DOWN;
  if (sdlMods & SDL_KMOD_CAPS) f |= EVENTFLAG_CAPS_LOCK_ON;
  if (l) f |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  if (m) f |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  if (r) f |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  return f;
}

int windowsKeyCodeFromSDL(uint32_t key) {
  if (key >= SDLK_A && key <= SDLK_Z) return 'A' + (int)(key - SDLK_A);
  if (key >= SDLK_0 && key <= SDLK_9) return '0' + (int)(key - SDLK_0);
  if (key >= SDLK_F1 && key <= SDLK_F12) return 0x70 + (int)(key - SDLK_F1);
  switch (key) {
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 0x0D;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:
    case SDLK_RCTRL: return 0x11;
    case SDLK_LALT:
    case SDLK_RALT: return 0x12;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_SPACE: return 0x20;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_END: return 0x23;
    case SDLK_HOME: return 0x24;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D;
    case SDLK_DELETE: return 0x2E;
    default: return 0;
  }
}
