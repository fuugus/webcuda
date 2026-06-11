#include "gl_funcs.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>

GLFns glf{};

bool initGLFunctions() {
#define LOAD(name, type)                                              \
  glf.name = reinterpret_cast<type>(glfwGetProcAddress("gl" #name)); \
  if (!glf.name) {                                                    \
    std::fprintf(stderr, "[gl] missing entry point gl%s\n", #name);   \
    return false;                                                     \
  }
  LOAD(CreateShader, PFNGLCREATESHADERPROC)
  LOAD(ShaderSource, PFNGLSHADERSOURCEPROC)
  LOAD(CompileShader, PFNGLCOMPILESHADERPROC)
  LOAD(GetShaderiv, PFNGLGETSHADERIVPROC)
  LOAD(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC)
  LOAD(CreateProgram, PFNGLCREATEPROGRAMPROC)
  LOAD(AttachShader, PFNGLATTACHSHADERPROC)
  LOAD(LinkProgram, PFNGLLINKPROGRAMPROC)
  LOAD(GetProgramiv, PFNGLGETPROGRAMIVPROC)
  LOAD(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC)
  LOAD(DeleteShader, PFNGLDELETESHADERPROC)
  LOAD(UseProgram, PFNGLUSEPROGRAMPROC)
  LOAD(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC)
  LOAD(Uniform1i, PFNGLUNIFORM1IPROC)
  LOAD(Uniform1f, PFNGLUNIFORM1FPROC)
  LOAD(Uniform2f, PFNGLUNIFORM2FPROC)
  LOAD(GenVertexArrays, PFNGLGENVERTEXARRAYSPROC)
  LOAD(BindVertexArray, PFNGLBINDVERTEXARRAYPROC)
  LOAD(ActiveTexture, PFNGLACTIVETEXTUREPROC)
  LOAD(BlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC)
#undef LOAD
  return true;
}

static unsigned compileStage(GLenum type, const char* src) {
  unsigned s = glf.CreateShader(type);
  glf.ShaderSource(s, 1, &src, nullptr);
  glf.CompileShader(s);
  GLint ok = 0;
  glf.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glf.GetShaderInfoLog(s, sizeof log, nullptr, log);
    std::fprintf(stderr, "[gl] shader compile failed:\n%s\n", log);
    return 0;
  }
  return s;
}

unsigned buildProgram(const char* vsSrc, const char* fsSrc) {
  unsigned vs = compileStage(GL_VERTEX_SHADER, vsSrc);
  unsigned fs = compileStage(GL_FRAGMENT_SHADER, fsSrc);
  if (!vs || !fs) return 0;
  unsigned p = glf.CreateProgram();
  glf.AttachShader(p, vs);
  glf.AttachShader(p, fs);
  glf.LinkProgram(p);
  glf.DeleteShader(vs);
  glf.DeleteShader(fs);
  GLint ok = 0;
  glf.GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glf.GetProgramInfoLog(p, sizeof log, nullptr, log);
    std::fprintf(stderr, "[gl] program link failed:\n%s\n", log);
    return 0;
  }
  return p;
}
