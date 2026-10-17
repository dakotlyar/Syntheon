# SPDX-License-Identifier: GPL-3.0-or-later
# synbench-cpucrypto, the CPU test's encryption worker (Phase 2 design §5.5,
# D6, P17): no Qt, OpenSSL's libcrypto linked statically (cmake/OpenSSL.cmake).
#
# It compiles its own copy of the engine and of the CPU-feature code instead
# of linking synbench_test_cpu / synbench_isa, so that on Windows it can use
# the release C runtime in every build type: OpenSSL is always built with
# /MD, and a Debug build's /MDd libraries can't share a program with it
# (Dave, 2026-10-17: release-CRT OpenSSL in the worker only; Debug CPU
# numbers are local-only and flagged).
add_executable(synbench-cpucrypto
    main.cpp
    crypto_kernel.h crypto_kernel.cpp
    "${PROJECT_SOURCE_DIR}/src/tests/cpu/golden/crypto.inc"
    "${PROJECT_SOURCE_DIR}/src/tests/cpu/engine/worker_pool.cpp"
    "${PROJECT_SOURCE_DIR}/src/tests/cpu/engine/platform_threads.cpp"
    "${PROJECT_SOURCE_DIR}/src/isa/cpu_features.cpp"
)
target_include_directories(synbench-cpucrypto PRIVATE "${PROJECT_SOURCE_DIR}/src")
find_package(Threads REQUIRED)
target_link_libraries(synbench-cpucrypto PRIVATE synbench::crypto Threads::Threads)
set_target_properties(synbench-cpucrypto PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL  # /MD in every build type, as OpenSSL
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF    # no Qt here
    INSTALL_RPATH "")                      # no library search path of its own
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # As the helper: nothing from a bundled library directory is needed.
    target_link_options(synbench-cpucrypto PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Next to SynBench.exe on Windows; usr/libexec in the AppDir (beside
# synbench-helper; the CPU test starts it by path, it isn't dispatched
# through AppRun), like the helper.
if(WIN32)
    install(TARGETS synbench-cpucrypto RUNTIME DESTINATION .)
else()
    include(GNUInstallDirs)
    install(TARGETS synbench-cpucrypto RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")
endif()
