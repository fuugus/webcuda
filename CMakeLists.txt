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
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

# ---------------- CEF (binary distribution in third_party/cef) ----------------
set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

# ---------------- other deps ----------------
find_package(CUDAToolkit REQUIRED)
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

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
target_compile_options(webcuda PRIVATE -O2 -Wall)
target_link_libraries(webcuda PRIVATE
  libcef_lib
  libcef_dll_wrapper
  sim
  PkgConfig::GLFW
  OpenGL::GL
  X11::X11
  CUDA::cudart)

# libcef.so sits next to the executable
set_target_properties(webcuda PROPERTIES
  BUILD_RPATH "$ORIGIN"
  INSTALL_RPATH "$ORIGIN")

# ---------------- runtime files next to the binary ----------------
add_custom_command(TARGET webcuda POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_ROOT}/Release"   "$<TARGET_FILE_DIR:webcuda>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CEF_ROOT}/Resources" "$<TARGET_FILE_DIR:webcuda>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:webcuda>/ui"
  COMMENT "Staging CEF runtime + UI files")
