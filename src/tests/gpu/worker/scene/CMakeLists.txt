# SPDX-License-Identifier: GPL-3.0-or-later
# The GPU scene content (Phase 4a design §5.1, §6): plain C++ with no Qt. Built
# with strict floating point (no FMA contraction, no fast-math), so the scenes
# and their hashes are the same on every compiler (P-FP, §6.7).
add_library(synbench_gpu_scene STATIC
    math.h prng.h
    mesh_gen.h mesh_gen.cpp
    textures.h textures.cpp
    scenes.h scenes.cpp
)
target_include_directories(synbench_gpu_scene PUBLIC "${PROJECT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(synbench_gpu_scene PRIVATE /fp:precise)
else()
    target_compile_options(synbench_gpu_scene PRIVATE -ffp-contract=off)
endif()
