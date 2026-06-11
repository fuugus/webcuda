#include "sim.h"

#include <GL/gl.h>
#include <cuda_runtime.h>
#include <cuda_gl_interop.h>

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

void seedPattern() {
  dim3 block(16, 16), grid = grid2d(W, H, block);
  kSeed<<<grid, block>>>(fieldA, W, H);
  std::mt19937 rng(20260611);
  std::uniform_int_distribution<int> rx(W / 8, W * 7 / 8), ry(H / 8, H * 7 / 8),
      rr(6, 22);
  for (int i = 0; i < 24; i++)
    kBlob<<<grid, block>>>(fieldA, W, H, rx(rng), ry(rng), rr(rng));
  cudaDeviceSynchronize();
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

  seedPattern();
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
}

const char* simDeviceName() { return gDevName; }
