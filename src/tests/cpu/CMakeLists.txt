# SPDX-License-Identifier: GPL-3.0-or-later
add_library(synbench_test_cpu STATIC
    cpu_test.h cpu_test.cpp
    # The measurement engine (Phase 2 design §4).
    engine/kernel.h
    engine/opaque.h
    engine/platform_threads.h engine/platform_threads.cpp
    engine/time_scale.h engine/time_scale.cpp
    engine/worker_pool.h engine/worker_pool.cpp
    # Deterministic data (§5.4, §6.4, P10).
    kernels/prng.h
    compress/corpus.h compress/corpus.cpp
    golden/golden.h
)
target_link_libraries(synbench_test_cpu PUBLIC synbench_core synbench_test_common synbench_isa)
find_package(Threads REQUIRED)
target_link_libraries(synbench_test_cpu PRIVATE Threads::Threads)
# The pinned compression libraries (cmake/ThirdParty.cmake), for the
# compression sub-test. Linked from the start so every build, the ARM64 cross
# builds included, compiles them.
target_link_libraries(synbench_test_cpu PRIVATE synbench::zlib synbench::zstd synbench::lz4 synbench::lzma)
