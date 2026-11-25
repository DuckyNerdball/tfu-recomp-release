# tfu - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(tfu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(TFU_SOURCES
    src/main.cpp
    src/crash_trace.cpp
    src/tfu_menu.cpp
    src/tfu_lua.cpp
)

if(WIN32)
    add_executable(tfu WIN32 ${TFU_SOURCES})
else()
    add_executable(tfu ${TFU_SOURCES})
endif()

rexglue_setup_target(tfu GPU_PLUGINS xenos)
# Perf (2026-11-17): the SDK default is -msse4.1. Target AVX2/FMA/BMI2 (x86-64-v3; Ryzen 5800X3D) for the
# recompiled guest code - guest logic dipped to 28 fps in fights (CPU bound). -ffp-contract=off keeps a*b+c as two
# roundings like the guest's separate fmul/fadd (no silent FMA fusion changing physics results).
# Our flags only: the shared SDK (also used by the BioShock recomp) is not touched.
target_compile_options(tfu PRIVATE -march=x86-64-v3 -ffp-contract=off)
if(TARGET tfu_recomp)   # the 238 recompiled guest files live in this OBJECT library (generated/rexglue.cmake)
    target_compile_options(tfu_recomp PRIVATE -march=x86-64-v3 -ffp-contract=off)
endif()
if(WIN32)
    target_link_libraries(tfu PRIVATE dbghelp)   # crash_trace.cpp: stack symbolization
endif()

