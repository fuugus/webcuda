cmake_minimum_required(VERSION 3.26)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Must be set before project(): CMake's CUDA detection otherwise probes
# sm_52, which CUDA 13.x no longer supports.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES native)
endif()

project(webcuda LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 20)  # CEF 148 headers use C++20 concepts

if(WIN32)
  # CEF binaries link the static CRT; everything in the process must match
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

# ---------------- CEF (binary distribution; build.sh fetches per platform) ----
if(NOT DEFINED CEF_ROOT)
  set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
endif()
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

# ---------------- other deps ----------------
find_package(CUDAToolkit REQUIRED)
find_package(OpenGL REQUIRED)
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)  # frameless window protocol; Linux only
endif()

# SDL3, vendored + static: its X11 backend implements the _NET_WM_SYNC_REQUEST
# frame-sync handshake (acked inside SDL_GL_SwapWindow), which is what makes
# interactive resize artifact-free. GLFW has no equivalent.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.2.16
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# ---------------- CUDA simulation (separate target: keeps nvcc away from CEF flags) ----------------
add_library(sim STATIC src/sim.cu)
set_target_properties(sim PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(sim PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-O3 --use_fast_math>)
target_link_libraries(sim PRIVATE CUDA::cudart OpenGL::GL)

# ---------------- main app ----------------
add_executable(webcuda
  src/main.cpp
  src/overlay.cpp
  src/gl_funcs.cpp)

target_include_directories(webcuda PRIVATE "${CEF_ROOT}" ${CUDAToolkit_INCLUDE_DIRS})
target_compile_definitions(webcuda PRIVATE $<$<CONFIG:Release>:NDEBUG>)
if(MSVC)
  target_compile_options(webcuda PRIVATE /W3)
else()
  target_compile_options(webcuda PRIVATE -O2 -Wall)
endif()
target_link_libraries(webcuda PRIVATE
  libcef_lib
  libcef_dll_wrapper
  sim
  SDL3::SDL3-static
  OpenGL::GL
  CUDA::cudart)
if(UNIX AND NOT APPLE)
  target_link_libraries(webcuda PRIVATE X11::X11)
  # libcef.so sits next to the executable
  set_target_properties(webcuda PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")
endif()
if(WIN32)
  # GUI subsystem but keep main() as the entry point
  set_target_properties(webcuda PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_options(webcuda PRIVATE /ENTRY:mainCRTStartup)
endif()

# ---------------- runtime files next to the binary ----------------
add_custom_command(TARGET webcuda POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_ROOT}/Release"   "$<TARGET_FILE_DIR:webcuda>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:webcuda>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:webcuda>/ui"
  COMMENT "Staging CEF runtime + UI files")
