# SPDX-License-Identifier: GPL-3.0-or-later
# The kernels: their own target, for their compile flags (P7).
add_subdirectory(kernels)
# The encryption worker, synbench-cpucrypto (§5.5): its own executable.
add_subdirectory(worker)

add_library(synbench_test_cpu STATIC
    cpu_test.h cpu_test.cpp
    cpu_context.h cpu_context.cpp
    # The measurement engine (Phase 2 design §4).
    engine/kernel.h
    engine/opaque.h
    engine/platform_threads.h engine/platform_threads.cpp
    engine/time_scale.h engine/time_scale.cpp
    engine/worker_pool.h engine/worker_pool.cpp
    # Deterministic data (§5.4, §6.4, P10).
    compress/corpus.h compress/corpus.cpp
    # The encryption worker's client (§5.5).
    crypto_client.h crypto_client.cpp
    # Compression (§5.4, P11).
    compress/codec.h compress/codec.cpp
    compress/compress_kernels.h compress/compress_kernels.cpp
    golden/golden.h
    # Telemetry and the throttling flag (§8).
    telemetry/telemetry.h telemetry/telemetry.cpp
    telemetry/sampler.h telemetry/sampler.cpp
    telemetry/throttle.h telemetry/throttle.cpp
)
target_link_libraries(synbench_test_cpu PUBLIC synbench_core synbench_test_common synbench_isa synbench_cpu_kernels)
find_package(Threads REQUIRED)
target_link_libraries(synbench_test_cpu PRIVATE Threads::Threads)
# Windows: telemetry reads PDH counters (§8.1), the power profile powrprof.
if(WIN32)
    target_link_libraries(synbench_test_cpu PRIVATE pdh powrprof)
endif()
# The pinned compression libraries (cmake/ThirdParty.cmake), for the
# compression sub-test. Linked from the start so every build, the ARM64 cross
# builds included, compiles them.
target_link_libraries(synbench_test_cpu PRIVATE synbench::zlib synbench::zstd synbench::lz4 synbench::lzma)
# They get the build type's flags: a Debug build measures unoptimised codecs
# (Dave, 2026-10-17: Debug CPU numbers are local-only and flagged; nothing is
# forced). codecsOptimised() tells the CPU test.
target_compile_definitions(synbench_test_cpu PRIVATE SYNBENCH_THIRD_PARTY_OPTIMISED=$<IF:$<CONFIG:Debug>,0,1>)
