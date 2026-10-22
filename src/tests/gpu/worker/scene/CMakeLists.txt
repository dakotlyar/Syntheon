# SPDX-License-Identifier: GPL-3.0-or-later
# The GPU scene content (Phase 4a design §5.1, §6): plain C++ with no Qt. Built
# with strict floating point (no FMA contraction, no fast-math), so the scenes
# and their hashes are the same on every compiler (P-FP, §6.7).
add_library(synbench_gpu_scene STATIC
    math.h prng.h
    mesh_gen.h mesh_gen.cpp
    textures.h textures.cpp
    scenes.h scenes.cpp
    native_ops.h native_ops.cpp
    bitmap_font.h bitmap_font.cpp bitmap_font.inc
    overlay.h overlay.cpp
    golden_hashes.inc
)
target_include_directories(synbench_gpu_scene PUBLIC "${PROJECT_SOURCE_DIR}/src")
# PUBLIC: math.h's inline maths (noise, matrices) is hashed, so every target that
# compiles it (the worker, the tests) must use the same strict floating point.
if(MSVC)
    target_compile_options(synbench_gpu_scene PUBLIC /fp:precise)
else()
    target_compile_options(synbench_gpu_scene PUBLIC -ffp-contract=off)
endif()
