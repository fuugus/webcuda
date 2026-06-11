// Minimal OpenGL >1.1 function loader (avoids a GLEW/glad dependency).
// GL 1.1 entry points come from libGL directly; everything newer is loaded
// through glfwGetProcAddress into the `glf` struct.
#pragma once

#include <GL/gl.h>
#include <GL/glext.h>

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
