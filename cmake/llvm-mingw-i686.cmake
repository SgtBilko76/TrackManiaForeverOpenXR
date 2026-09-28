# Cross-compile the 32-bit Windows DLL on Linux with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw). Set LLVM_MINGW_ROOT or put the
# toolchain's bin directory on PATH.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT LLVM_MINGW_ROOT AND DEFINED ENV{LLVM_MINGW_ROOT})
  set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
endif()
if(LLVM_MINGW_ROOT)
  set(_prefix "${LLVM_MINGW_ROOT}/bin/")
endif()

set(CMAKE_C_COMPILER "${_prefix}i686-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${_prefix}i686-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${_prefix}i686-w64-mingw32-windres")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
