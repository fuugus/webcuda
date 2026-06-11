// CUDA Gray-Scott reaction-diffusion simulation rendering straight into an
// OpenGL texture via CUDA-GL interop (surface writes, zero CPU copies).
#pragma once

struct SimParams {
  float F = 0.0545f;        // feed rate
  float k = 0.0620f;        // kill rate
  int   steps = 12;         // simulation steps per rendered frame
  int   palette = 0;        // 0..3
  float brushRadius = 18.f; // in sim cells
  bool  paused = false;
};

struct SimStats {
  float kernelMs = 0.f;     // GPU time for sim+colorize this frame
};

// Registers the GL texture (must be GL_RGBA8, w x h) with CUDA and allocates
// the simulation fields. GL context must be current. Returns false on error
// (prints the reason to stderr).
// If the GL context is not on a CUDA device (software GL, hybrid graphics),
// falls back to a device->host copy: simPixels() then returns the colorized
// frame for manual texture upload.
bool simInit(int w, int h, unsigned glTexture);

// nullptr in zero-copy interop mode; host RGBA pixels in fallback mode.
const unsigned char* simPixels();

// Re-seed the field with the default blob pattern.
void simReset();

// Inject chemical V at sim coordinates (immediate kernel launch).
void simBrush(float x, float y, float radius);

// Run `steps` sim iterations and colorize into the GL texture.
void simFrame(const SimParams& p, SimStats& out);

void simShutdown();

const char* simDeviceName();
