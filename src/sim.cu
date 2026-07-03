#include "sim.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // GL/gl.h needs APIENTRY on Windows
#endif
#include <GL/gl.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

#include <cmath>
#include <cstdio>
#include <random>

namespace {

int W = 0, H = 0;
float2* fieldA = nullptr;  // x = U, y = V
float2* fieldB = nullptr;
cudaGraphicsResource* glRes = nullptr;
cudaEvent_t evStart, evStop;
char gDevName[256] = "unknown GPU";

// fallback mode (GL context not on a CUDA device): colorize into a linear
// buffer and copy to pinned host memory each frame
bool gFallback = false;
uchar4* dPixels = nullptr;
uchar4* hPixels = nullptr;

#define CUCHECK(call)                                                         \
  do {                                                                        \
    cudaError_t err__ = (call);                                               \
    if (err__ != cudaSuccess) {                                               \
      std::fprintf(stderr, "[cuda] %s failed: %s (%s:%d)\n", #call,           \
                   cudaGetErrorString(err__), __FILE__, __LINE__);            \
      return false;                                                           \
    }                                                                         \
  } while (0)

__global__ void kSeed(float2* f, int w, int h) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  f[y * w + x] = make_float2(1.f, 0.f);
}

__global__ void kBlob(float2* f, int w, int h, int cx, int cy, int r) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  int dx = x - cx, dy = y - cy;
  if (dx * dx + dy * dy <= r * r) f[y * w + x] = make_float2(0.5f, 0.85f);
}

// Karl Sims formulation: 9-point laplacian (0.2 cardinal, 0.05 diagonal),
// Du = 1.0, Dv = 0.5, dt = 1.0; toroidal wrap.
__global__ void kStep(const float2* __restrict__ in, float2* __restrict__ out,
                      int w, int h, float F, float k) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;

  int xm = (x == 0) ? w - 1 : x - 1;
  int xp = (x == w - 1) ? 0 : x + 1;
  int ym = (y == 0) ? h - 1 : y - 1;
  int yp = (y == h - 1) ? 0 : y + 1;

  float2 c = in[y * w + x];
  float2 l = in[y * w + xm], r = in[y * w + xp];
  float2 u = in[ym * w + x], d = in[yp * w + x];
  float2 ul = in[ym * w + xm], ur = in[ym * w + xp];
  float2 dl = in[yp * w + xm], dr = in[yp * w + xp];

  float lapU = 0.2f * (l.x + r.x + u.x + d.x) +
               0.05f * (ul.x + ur.x + dl.x + dr.x) - c.x;
  float lapV = 0.2f * (l.y + r.y + u.y + d.y) +
               0.05f * (ul.y + ur.y + dl.y + dr.y) - c.y;

  float uvv = c.x * c.y * c.y;
  float nu = c.x + (1.0f * lapU - uvv + F * (1.f - c.x));
  float nv = c.y + (0.5f * lapV + uvv - (F + k) * c.y);
  out[y * w + x] = make_float2(__saturatef(nu), __saturatef(nv));
}

__global__ void kBrush(float2* f, int w, int h, float cx, float cy, float r) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  float dx = x - cx, dy = y - cy;
  float d2 = dx * dx + dy * dy;
  if (d2 <= r * r) {
    float fall = 1.f - sqrtf(d2) / r;
    float2 c = f[y * w + x];
    c.y = __saturatef(c.y + 0.9f * fall);
    c.x = __saturatef(c.x - 0.4f * fall);
    f[y * w + x] = c;
  }
}

__device__ float3 lerp3(float3 a, float3 b, float t) {
  return make_float3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                     a.z + (b.z - a.z) * t);
}

__device__ float3 paletteMap(int pal, float t) {
  // 5-stop gradients
  const float3 stops[4][5] = {
      // 0 abyss (deep blue -> cyan -> white)
      {{0.027f, 0.035f, 0.075f}, {0.05f, 0.14f, 0.34f}, {0.09f, 0.42f, 0.72f},
       {0.38f, 0.83f, 0.90f},   {0.97f, 1.00f, 0.96f}},
      // 1 ember
      {{0.04f, 0.02f, 0.04f}, {0.34f, 0.06f, 0.06f}, {0.84f, 0.30f, 0.05f},
       {1.00f, 0.70f, 0.20f}, {1.00f, 0.98f, 0.86f}},
      // 2 bio
      {{0.02f, 0.04f, 0.03f}, {0.04f, 0.23f, 0.12f}, {0.10f, 0.54f, 0.25f},
       {0.52f, 0.86f, 0.30f}, {0.97f, 1.00f, 0.88f}},
      // 3 mono
      {{0.045f, 0.05f, 0.065f}, {0.20f, 0.21f, 0.25f}, {0.45f, 0.47f, 0.52f},
       {0.72f, 0.74f, 0.78f},  {0.96f, 0.97f, 1.00f}}};
  t = __saturatef(t) * 4.f;
  int i = min(3, (int)t);
  return lerp3(stops[pal][i], stops[pal][i + 1], t - i);
}

__device__ uchar4 shade(float2 c, int pal) {
  float t = __saturatef(c.y * 3.2f) * 0.85f + __saturatef(1.f - c.x) * 0.15f;
  float3 col = paletteMap(pal, t);
  return make_uchar4((unsigned char)(col.x * 255.f),
                     (unsigned char)(col.y * 255.f),
                     (unsigned char)(col.z * 255.f), 255);
}

__global__ void kColor(const float2* __restrict__ f, int w, int h,
                       cudaSurfaceObject_t surf, int pal) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  surf2Dwrite(shade(f[y * w + x], pal), surf, x * 4, y);
}

__global__ void kColorBuf(const float2* __restrict__ f, int w, int h,
                          uchar4* __restrict__ out, int pal) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  out[y * w + x] = shade(f[y * w + x], pal);
}

dim3 grid2d(int w, int h, dim3 block) {
  return dim3((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
}

// ---------------------------------------------------------------------------
// Scene 1: 500M-point cloud, rasterized in CUDA (Schütz-style):
// every point does one 64-bit atomicMin of (depth<<32 | color) into a
// framebuffer — no GL primitive pipeline, no vertex fetch, no raster units.
// ---------------------------------------------------------------------------
constexpr uint32_t kPointCount = 500'000'000;
constexpr unsigned long long kFbClear = 0xFFFFFFFFFFFFFFFFull;

struct PPoint {
  float x, y, z;
  uint32_t rgba;
};

PPoint* dPoints = nullptr;
unsigned long long* dFb = nullptr;  // (depth<<32) | rgba per pixel
bool pointsReady = false, pointsFailed = false;
__constant__ float cMVP[16];  // column-major, GL convention

__device__ uint32_t pcg(uint32_t v) {
  v = v * 747796405u + 2891336453u;
  v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (v >> 22u) ^ v;
}

// galaxy-ish disc: radius/angle/height from hashes, color by radius+height
__global__ void kGenPoints(PPoint* pts, uint32_t n) {
  uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  uint32_t h1 = pcg(i), h2 = pcg(h1), h3 = pcg(h2);
  float r = sqrtf(h1 * (1.f / 4294967296.f));
  float arm = (i & 1) ? 0.f : 3.14159265f;
  float ang = h2 * (6.2831853f / 4294967296.f) * 0.25f + r * 5.5f + arm;
  float y = (h3 * (2.f / 4294967296.f) - 1.f);
  y = y * y * y * 0.22f * (1.05f - r);
  PPoint p;
  p.x = r * cosf(ang);
  p.z = r * sinf(ang);
  p.y = y;
  float t = __saturatef(r * 0.85f + fabsf(y) * 2.5f);
  unsigned char cr = (unsigned char)(40.f + 215.f * t);
  unsigned char cg = (unsigned char)(90.f + 130.f * (1.f - t) + 40.f * t);
  unsigned char cb = (unsigned char)(120.f + 135.f * (1.f - t));
  p.rgba = (uint32_t)cr | ((uint32_t)cg << 8) | ((uint32_t)cb << 16) |
           0xFF000000u;
  pts[i] = p;
}

__global__ void kClearFb(unsigned long long* fb, int n) {
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < n) fb[i] = kFbClear;
}

__global__ void kRasterPoints(const PPoint* __restrict__ pts, uint32_t n,
                              unsigned long long* fb, int w, int h) {
  uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n) return;
  PPoint p = pts[i];
  float cx = cMVP[0] * p.x + cMVP[4] * p.y + cMVP[8] * p.z + cMVP[12];
  float cy = cMVP[1] * p.x + cMVP[5] * p.y + cMVP[9] * p.z + cMVP[13];
  float cz = cMVP[2] * p.x + cMVP[6] * p.y + cMVP[10] * p.z + cMVP[14];
  float cw = cMVP[3] * p.x + cMVP[7] * p.y + cMVP[11] * p.z + cMVP[15];
  if (cw <= 0.f) return;
  float inv = __frcp_rn(cw);
  float nx = cx * inv, ny = cy * inv, nz = cz * inv;
  if (nx < -1.f || nx > 1.f || ny < -1.f || ny > 1.f || nz < 0.f || nz > 1.f)
    return;
  int px = (int)((nx * 0.5f + 0.5f) * w);
  int py = (int)((0.5f - ny * 0.5f) * h);
  if (px < 0 || px >= w || py < 0 || py >= h) return;
  // float bits are order-preserving for values in [0,1] — cheap depth key
  unsigned long long v =
      ((unsigned long long)__float_as_uint(nz) << 32) | p.rgba;
  // early-out: with hundreds of points per pixel, most candidates are behind
  // the current winner — a plain read costs far less than a contended atomic
  unsigned long long* slot = &fb[py * w + px];
  if (v < *slot) atomicMin(slot, v);
}

__global__ void kResolveFb(const unsigned long long* __restrict__ fb, int w,
                           int h, cudaSurfaceObject_t surf) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  unsigned long long v = fb[y * w + x];
  uchar4 px;
  if (v == kFbClear) {
    px = make_uchar4(8, 10, 16, 255);
  } else {
    uint32_t c = (uint32_t)v;
    px = make_uchar4(c & 255, (c >> 8) & 255, (c >> 16) & 255, 255);
  }
  surf2Dwrite(px, surf, x * 4, y);
}

bool ensurePoints() {
  if (pointsReady) return true;
  if (pointsFailed) return false;
  std::printf("[points] allocating %u points (%.1f GB) + framebuffer...\n",
              kPointCount, kPointCount * sizeof(PPoint) / 1e9);
  if (cudaMalloc(&dPoints, (size_t)kPointCount * sizeof(PPoint)) !=
          cudaSuccess ||
      cudaMalloc(&dFb, (size_t)W * H * 8) != cudaSuccess) {
    std::fprintf(stderr, "[points] allocation failed — scene unavailable\n");
    pointsFailed = true;
    return false;
  }
  kGenPoints<<<(kPointCount + 255) / 256, 256>>>(dPoints, kPointCount);
  cudaDeviceSynchronize();
  std::printf("[points] ready\n");
  pointsReady = true;
  return true;
}

// minimal column-major mat4 (GL convention) — enough for perspective * lookAt
void mat4Mul(float* r, const float* a, const float* b) {
  for (int c = 0; c < 4; c++)
    for (int rw = 0; rw < 4; rw++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a[k * 4 + rw] * b[c * 4 + k];
      r[c * 4 + rw] = s;
    }
}

void buildMVP(float* out, const SimParams& p, float aspect) {
  float cp = std::cos(p.camPitch), sp = std::sin(p.camPitch);
  float cy = std::cos(p.camYaw), sy = std::sin(p.camYaw);
  float ex = p.camDist * cp * sy, ey = p.camDist * sp, ez = p.camDist * cp * cy;

  // lookAt(eye, origin, up=+Y):  f = normalize(-eye)
  float fl = std::sqrt(ex * ex + ey * ey + ez * ez);
  float fx = -ex / fl, fy = -ey / fl, fz = -ez / fl;
  // s = normalize(cross(f, up)) = normalize((-fz, 0, fx))
  float sl = std::sqrt(fz * fz + fx * fx);
  float sx = -fz / sl, sz = fx / sl;  // sy = 0
  // u = cross(s, f)
  float ux = -sz * fy, uy = sz * fx - sx * fz, uz = sx * fy;

  float view[16] = {sx, ux, -fx, 0,  0,  uy, -fy, 0,  sz, uz, -fz, 0,
                    -(sx * ex + sz * ez), -(ux * ex + uy * ey + uz * ez),
                    (fx * ex + fy * ey + fz * ez), 1};

  const float fovY = 1.0472f, zn = 0.02f, zf = 60.f;  // 60 deg
  float t = 1.f / std::tan(fovY * 0.5f);
  // depth mapped to [0,1] (D3D-style) so the raster kernel's float-bit
  // depth key stays monotonic
  float proj[16] = {t / aspect, 0, 0, 0,  0, t, 0, 0,
                    0, 0, zf / (zn - zf), -1,  0, 0, zn * zf / (zn - zf), 0};
  mat4Mul(out, proj, view);
}

bool seedPattern() {
  dim3 block(16, 16), grid = grid2d(W, H, block);
  kSeed<<<grid, block>>>(fieldA, W, H);
  std::mt19937 rng(20260611);
  std::uniform_int_distribution<int> rx(W / 8, W * 7 / 8), ry(H / 8, H * 7 / 8),
      rr(6, 22);
  for (int i = 0; i < 24; i++)
    kBlob<<<grid, block>>>(fieldA, W, H, rx(rng), ry(rng), rr(rng));
  // Launch errors surface here, not at the <<<>>> site. The big one:
  // cudaErrorUnsupportedPtxVersion — binary built by a CUDA toolkit newer
  // than the driver's JIT, with no native SASS for this GPU. Without this
  // check the sim silently no-ops (black background, kernels 0 ms).
  CUCHECK(cudaDeviceSynchronize());
  return true;
}

}  // namespace

bool simInit(int w, int h, unsigned glTexture) {
  W = w;
  H = h;

  // Pick the CUDA device that drives the current GL context. If this fails,
  // GL is rendering on a different GPU (e.g. Mesa/iGPU) and interop is
  // impossible — surface an actionable message.
  unsigned cnt = 0;
  int dev = 0;
  cudaError_t e = cudaGLGetDevices(&cnt, &dev, 1, cudaGLDeviceListAll);
  if (e != cudaSuccess || cnt == 0) {
    std::fprintf(stderr,
                 "[cuda] GL context is not on a CUDA device (%s) — using "
                 "device->host copy fallback instead of zero-copy interop\n",
                 cudaGetErrorString(e));
    cudaGetLastError();  // clear sticky error
    gFallback = true;
    dev = 0;
  }
  CUCHECK(cudaSetDevice(dev));
  cudaDeviceProp prop{};
  CUCHECK(cudaGetDeviceProperties(&prop, dev));
  std::snprintf(gDevName, sizeof gDevName, "%s%s", prop.name,
                gFallback ? " (copy fallback)" : "");

  size_t bytes = (size_t)W * H * sizeof(float2);
  CUCHECK(cudaMalloc(&fieldA, bytes));
  CUCHECK(cudaMalloc(&fieldB, bytes));
  if (gFallback) {
    CUCHECK(cudaMalloc(&dPixels, (size_t)W * H * sizeof(uchar4)));
    CUCHECK(cudaMallocHost(&hPixels, (size_t)W * H * sizeof(uchar4)));
  } else {
    CUCHECK(cudaGraphicsGLRegisterImage(
        &glRes, glTexture, GL_TEXTURE_2D,
        cudaGraphicsRegisterFlagsSurfaceLoadStore));
  }
  CUCHECK(cudaEventCreate(&evStart));
  CUCHECK(cudaEventCreate(&evStop));

  if (!seedPattern()) {
    std::fprintf(stderr,
                 "[cuda] kernels cannot run on %s — if the error above says "
                 "'unsupported toolchain'/'unsupported PTX version', this "
                 "binary was built by a newer CUDA toolkit than the driver "
                 "supports and has no native code for this GPU; rebuild on "
                 "this machine (build.sh picks -arch=native) or update the "
                 "driver\n",
                 prop.name);
    return false;
  }
  return true;
}

void simReset() {
  if (fieldA) seedPattern();
}

void simBrush(float x, float y, float radius) {
  if (!fieldA) return;
  dim3 block(16, 16), grid = grid2d(W, H, block);
  kBrush<<<grid, block>>>(fieldA, W, H, x, y, radius);
}

void simFrame(const SimParams& p, SimStats& out) {
  if (!fieldA) return;
  dim3 block(16, 16), grid = grid2d(W, H, block);

  cudaSurfaceObject_t surf = 0;
  if (!gFallback) {
    cudaGraphicsMapResources(1, &glRes, nullptr);
    cudaArray_t arr = nullptr;
    cudaGraphicsSubResourceGetMappedArray(&arr, glRes, 0, 0);
    cudaResourceDesc rd{};
    rd.resType = cudaResourceTypeArray;
    rd.res.array.array = arr;
    cudaCreateSurfaceObject(&surf, &rd);
  }

  cudaEventRecord(evStart);
  if (p.scene == 1 && ensurePoints()) {
    // CUDA point rasterizer: clear -> atomicMin raster -> resolve to surface
    float mvp[16];
    buildMVP(mvp, p, (float)W / (float)H);
    cudaMemcpyToSymbol(cMVP, mvp, sizeof mvp);
    int fbN = W * H;
    kClearFb<<<(fbN + 255) / 256, 256>>>(dFb, fbN);
    kRasterPoints<<<(kPointCount + 255) / 256, 256>>>(dPoints, kPointCount,
                                                      dFb, W, H);
    if (gFallback) {
      // resolve to surface unavailable without interop; not supported here
    } else {
      kResolveFb<<<grid, block>>>(dFb, W, H, surf);
    }
  } else {
    if (!p.paused) {
      for (int i = 0; i < p.steps; i++) {
        kStep<<<grid, block>>>(fieldA, fieldB, W, H, p.F, p.k);
        float2* t = fieldA;
        fieldA = fieldB;
        fieldB = t;
      }
    }
    if (gFallback) {
      kColorBuf<<<grid, block>>>(fieldA, W, H, dPixels, p.palette & 3);
      cudaMemcpyAsync(hPixels, dPixels, (size_t)W * H * sizeof(uchar4),
                      cudaMemcpyDeviceToHost);
    } else {
      kColor<<<grid, block>>>(fieldA, W, H, surf, p.palette & 3);
    }
  }
  cudaEventRecord(evStop);

  if (!gFallback) {
    cudaDestroySurfaceObject(surf);
    cudaGraphicsUnmapResources(1, &glRes, nullptr);
  }
  cudaEventSynchronize(evStop);
  cudaEventElapsedTime(&out.kernelMs, evStart, evStop);
}

const unsigned char* simPixels() {
  return gFallback ? reinterpret_cast<const unsigned char*>(hPixels) : nullptr;
}

void simShutdown() {
  if (glRes) cudaGraphicsUnregisterResource(glRes), glRes = nullptr;
  if (fieldA) cudaFree(fieldA), fieldA = nullptr;
  if (fieldB) cudaFree(fieldB), fieldB = nullptr;
  if (dPixels) cudaFree(dPixels), dPixels = nullptr;
  if (hPixels) cudaFreeHost(hPixels), hPixels = nullptr;
  if (dPoints) cudaFree(dPoints), dPoints = nullptr;
  if (dFb) cudaFree(dFb), dFb = nullptr;
}

const char* simDeviceName() { return gDevName; }
