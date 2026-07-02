// Minimal OpenGL >1.1 function loader (avoids a GLEW/glad dependency).
// GL 1.1 entry points come from the system GL library directly; everything
// newer is loaded through SDL_GL_GetProcAddress into the `glf` struct.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // GL/gl.h needs APIENTRY on Windows
#endif
#include <GL/gl.h>
// vendored: MSVC ships GL 1.1 headers only, no glext.h
#include "khronos/glext.h"

struct GLFns {
  PFNGLCREATESHADERPROC        CreateShader;
  PFNGLSHADERSOURCEPROC        ShaderSource;
  PFNGLCOMPILESHADERPROC       CompileShader;
  PFNGLGETSHADERIVPROC         GetShaderiv;
  PFNGLGETSHADERINFOLOGPROC    GetShaderInfoLog;
  PFNGLCREATEPROGRAMPROC       CreateProgram;
  PFNGLATTACHSHADERPROC        AttachShader;
  PFNGLLINKPROGRAMPROC         LinkProgram;
  PFNGLGETPROGRAMIVPROC        GetProgramiv;
  PFNGLGETPROGRAMINFOLOGPROC   GetProgramInfoLog;
  PFNGLDELETESHADERPROC        DeleteShader;
  PFNGLUSEPROGRAMPROC          UseProgram;
  PFNGLGETUNIFORMLOCATIONPROC  GetUniformLocation;
  PFNGLUNIFORM1IPROC           Uniform1i;
  PFNGLUNIFORM1FPROC           Uniform1f;
  PFNGLUNIFORM2FPROC           Uniform2f;
  PFNGLGENVERTEXARRAYSPROC     GenVertexArrays;
  PFNGLBINDVERTEXARRAYPROC     BindVertexArray;
  PFNGLACTIVETEXTUREPROC       ActiveTexture;
  PFNGLBLENDFUNCSEPARATEPROC   BlendFuncSeparate;
};

extern GLFns glf;

// Loads all pointers above; returns false if any are missing.
bool initGLFunctions();

// Compiles vs+fs into a program; returns 0 and prints the log on failure.
unsigned buildProgram(const char* vsSrc, const char* fsSrc);
