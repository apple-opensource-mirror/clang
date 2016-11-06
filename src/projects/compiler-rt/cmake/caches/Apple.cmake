# This file sets up a CMakeCache for Apple-style stage2 bootstrap. It is
# specified by the stage1 build.

set(LLVM_TARGETS_TO_BUILD X86 ARM AArch64 CACHE STRING "") 
set(COMPILER_RT_INCLUDE_TESTS OFF CACHE BOOL "")
set(COMPILER_RT_HAS_SAFESTACK OFF CACHE BOOL "")
set(COMPILER_RT_EXTERNALIZE_DEBUGINFO ON CACHE BOOL "")
set(CMAKE_MACOSX_RPATH ON CACHE BOOL "")

set(CMAKE_C_FLAGS_RELEASE "-O3" CACHE STRING "")
set(CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE STRING "")
set(CMAKE_ASM_FLAGS_RELEASE "-O3" CACHE STRING "")
set(CMAKE_BUILD_TYPE RELEASE CACHE STRING "")
