# SPDX-License-Identifier: GPL-3.0-or-later
# What the CPU and the OS support, and which core the single-thread mark uses
# (Phase 2 design P15). Plain C++20, no Qt: System info, the CPU test and the
# Qt-free crypto worker all link it.
add_library(synbench_isa STATIC
    cpu_features.h cpu_features.cpp
    core_ranking.h core_ranking.cpp
)
target_include_directories(synbench_isa PUBLIC "${PROJECT_SOURCE_DIR}/src")
