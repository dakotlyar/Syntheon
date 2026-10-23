# SPDX-License-Identifier: GPL-3.0-or-later
synbench_add_unit_test(tst_units SOURCES tst_units.cpp LIBS synbench_core)
synbench_add_unit_test(tst_third_party SOURCES tst_third_party.cpp LIBS synbench::zlib synbench::zstd synbench::lz4 synbench::lzma Qt6::Core)
synbench_add_unit_test(tst_options SOURCES tst_options.cpp LIBS synbench_core)
synbench_add_unit_test(tst_result SOURCES tst_result.cpp LIBS synbench_core)
synbench_add_unit_test(tst_registry SOURCES tst_registry.cpp fake_test.h LIBS synbench_builtin_tests)
synbench_add_unit_test(tst_selection_model SOURCES tst_selection_model.cpp LIBS synbench_builtin_tests)
synbench_add_unit_test(tst_storage SOURCES tst_storage.cpp LIBS synbench_storage)
synbench_add_unit_test(tst_orchestrator SOURCES tst_orchestrator.cpp fake_test.h LIBS synbench_core synbench_storage)
synbench_add_unit_test(tst_power_inhibitor SOURCES tst_power_inhibitor.cpp LIBS synbench_power)
if(UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(tst_power_inhibitor PRIVATE Qt6::DBus)
endif()
synbench_add_unit_test(tst_recovery SOURCES tst_recovery.cpp fake_test.h LIBS synbench_core synbench_storage)
synbench_add_unit_test(tst_sysinfo_parsers SOURCES tst_sysinfo_parsers.cpp smbios_builder.h LIBS synbench_sysinfo)
synbench_add_unit_test(tst_sysinfo_live SOURCES tst_sysinfo_live.cpp LIBS synbench_sysinfo Qt6::Gui)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    synbench_add_unit_test(tst_sysinfo_linux SOURCES tst_sysinfo_linux.cpp LIBS synbench_sysinfo)
endif()
synbench_add_unit_test(tst_isa SOURCES tst_isa.cpp LIBS synbench_isa synbench_sysinfo)
synbench_add_unit_test(tst_cache_topology SOURCES tst_cache_topology.cpp LIBS synbench_isa Qt6::Core)
synbench_add_unit_test(tst_cpu_engine SOURCES tst_cpu_engine.cpp LIBS synbench_test_cpu)
synbench_add_unit_test(tst_cpu_data SOURCES tst_cpu_data.cpp LIBS synbench_test_cpu synbench::zlib)
synbench_add_unit_test(tst_cpu_kernels SOURCES tst_cpu_kernels.cpp LIBS synbench_test_cpu)
synbench_add_unit_test(tst_cpu_compress SOURCES tst_cpu_compress.cpp
    LIBS synbench_test_cpu synbench::zlib synbench::zstd synbench::lz4 synbench::lzma)
target_compile_definitions(tst_cpu_compress PRIVATE SB_DEBUG_BUILD=$<IF:$<CONFIG:Debug>,1,0>)
synbench_add_unit_test(tst_ram_memory SOURCES tst_ram_memory.cpp LIBS synbench_test_ram)
synbench_add_unit_test(tst_ram_chase SOURCES tst_ram_chase.cpp LIBS synbench_test_ram)
synbench_add_unit_test(tst_ram_sweep SOURCES tst_ram_sweep.cpp LIBS synbench_test_ram)
synbench_add_unit_test(tst_ram_bandwidth SOURCES tst_ram_bandwidth.cpp LIBS synbench_test_ram)
synbench_add_unit_test(tst_ram_loaded SOURCES tst_ram_loaded.cpp LIBS synbench_test_ram)
synbench_add_unit_test(tst_ram_test SOURCES tst_ram_test.cpp LIBS synbench_test_ram)
# These keep threads busy on (up to) every CPU; alone, so a parallel ctest -j
# doesn't starve the timing-sensitive engine tests' coordinator
# (tst_cpu_engine's cancelAtWindowChangeover). CI runs ctest serially anyway.
# tst_ram_chase pins competitors to a CPU and compares timings.
set_tests_properties(tst_ram_bandwidth tst_ram_loaded tst_ram_test tst_ram_chase PROPERTIES RUN_SERIAL TRUE)
synbench_add_unit_test(tst_cpu_telemetry SOURCES tst_cpu_telemetry.cpp LIBS synbench_test_cpu)
synbench_add_unit_test(tst_cpu_test SOURCES tst_cpu_test.cpp LIBS synbench_test_cpu Qt6::Core)
add_dependencies(tst_cpu_test synbench-cpucrypto)
synbench_add_unit_test(tst_cpu_crypto SOURCES tst_cpu_crypto.cpp LIBS synbench_test_cpu Qt6::Core)
add_dependencies(tst_cpu_crypto synbench-cpucrypto)
# A fake worker for the client's checks (no OpenSSL, no Qt; never installed).
add_executable(fake-cpucrypto fake_cpucrypto.cpp)
set_target_properties(fake-cpucrypto PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-tools")
add_dependencies(tst_cpu_crypto fake-cpucrypto)
target_compile_definitions(tst_cpu_crypto PRIVATE SB_FAKE_WORKER="$<TARGET_FILE:fake-cpucrypto>")
foreach(t tst_sysinfo_parsers tst_sysinfo_live tst_sysinfo_linux tst_isa tst_cache_topology tst_cpu_telemetry tst_ram_sweep
         tst_ram_bandwidth)
    if(TARGET ${t})
        target_compile_definitions(${t} PRIVATE SB_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures")
        target_include_directories(${t} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    endif()
endforeach()
synbench_add_unit_test(tst_protocol SOURCES tst_protocol.cpp LIBS synbench_protocol Qt6::Core)
synbench_add_unit_test(tst_gpu_model SOURCES tst_gpu_model.cpp LIBS synbench_gpu_model)
target_compile_definitions(tst_gpu_model PRIVATE SB_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures")
synbench_add_unit_test(tst_gpu_scene SOURCES tst_gpu_scene.cpp LIBS synbench_gpu_scene synbench_gpu_model)
synbench_add_unit_test(tst_gpu_loaders SOURCES tst_gpu_loaders.cpp LIBS synbench::volk synbench::glad)
# The built worker, through QProcess (the null backend, no GPU).
synbench_add_unit_test(tst_gpu_worker SOURCES tst_gpu_worker.cpp LIBS synbench_gpu_model synbench_gpu_backends)
add_dependencies(tst_gpu_worker synbench-gpu)
target_compile_definitions(tst_gpu_worker PRIVATE SB_GPU_WORKER="$<TARGET_FILE:synbench-gpu>")
# GpuClient and GpuSession (task 14): the real worker on the null backend and a
# fake worker with planted defects (never installed).
synbench_add_unit_test(tst_gpu_client SOURCES tst_gpu_client.cpp LIBS synbench_test_gpu Qt6::Core)
add_executable(fake-gpuworker fake_gpuworker.cpp)
set_target_properties(fake-gpuworker PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-tools")
add_dependencies(tst_gpu_client fake-gpuworker synbench-gpu)
target_compile_definitions(tst_gpu_client PRIVATE SB_FAKE_GPUWORKER="$<TARGET_FILE:fake-gpuworker>" SB_GPU_WORKER="$<TARGET_FILE:synbench-gpu>")
# The GPU test (task 15) with the fake worker.
synbench_add_unit_test(tst_gpu_formula SOURCES tst_gpu_formula.cpp LIBS synbench_test_gpu Qt6::Core)
synbench_add_unit_test(tst_gpu_test SOURCES tst_gpu_test.cpp fake_test.h LIBS synbench_test_gpu synbench_builtin_tests synbench_storage Qt6::Core)
add_dependencies(tst_gpu_test fake-gpuworker)
target_compile_definitions(tst_gpu_test PRIVATE SB_FAKE_GPUWORKER="$<TARGET_FILE:fake-gpuworker>")
# Task 35: the history of five GPU runs that tests/tools/test_cpu_repeatability.py (cpu_repeatability_tool) reads, written
# by tst_gpu_test's repeatabilityFixture slot (its own short test, so the repeatability test doesn't wait for the whole
# binary). The full tst_gpu_test run builds the same history in a temporary folder.
set(SB_GPU_FIXTURE_DB "${CMAKE_BINARY_DIR}/test-fixtures/gpu-repeatability/synbench.db" CACHE INTERNAL "")
add_test(NAME gpu_repeatability_fixture COMMAND tst_gpu_test repeatabilityFixture)
synbench_isolate_test(gpu_repeatability_fixture)
set_property(TEST gpu_repeatability_fixture APPEND PROPERTY ENVIRONMENT "SB_GPU_FIXTURE_DB=${SB_GPU_FIXTURE_DB}")
set_property(TEST gpu_repeatability_fixture PROPERTY FIXTURES_SETUP gpu_repeatability_db)
set_property(TEST gpu_repeatability_fixture APPEND PROPERTY LABELS gpu)
# The real backends on software drivers (task 10: Vulkan on lavapipe, with
# validation): headless in-process and through the worker; tst_gpu_present
# with a window, under xvfb-run only (A-X; it skips under offscreen).
foreach(t tst_gpu_backends tst_gpu_present tst_gpu_signature)
    synbench_add_unit_test(${t} SOURCES ${t}.cpp gpu_worker_process.h LIBS synbench_gpu_model synbench_gpu_backends)
    add_dependencies(${t} synbench-gpu)
    target_compile_definitions(${t} PRIVATE SB_GPU_WORKER="$<TARGET_FILE:synbench-gpu>")
endforeach()
# The Qt raster native 2D path in process (task 26): headless on Qt's offscreen platform; the font is the
# worker's own resource file.
synbench_add_unit_test(tst_gpu_native SOURCES tst_gpu_native.cpp "${PROJECT_SOURCE_DIR}/src/tests/gpu/worker/worker.qrc"
                       LIBS synbench_gpu_qtraster synbench_gpu_scene Qt6::Gui)
set_target_properties(tst_gpu_native PROPERTIES AUTORCC ON)
if(WIN32)
    # Task 27: the Direct2D path (in the backends library) on WARP, in process.
    target_link_libraries(tst_gpu_native PRIVATE synbench_gpu_backends)
endif()
synbench_add_unit_test(tst_stubs SOURCES tst_stubs.cpp fake_test.h LIBS synbench_builtin_tests synbench_storage)
synbench_add_unit_test(tst_controllers SOURCES tst_controllers.cpp LIBS synbench_appcore)
synbench_add_unit_test(tst_run_controller SOURCES tst_run_controller.cpp LIBS synbench_appcore)
add_dependencies(tst_run_controller fake-gpuworker)
target_compile_definitions(tst_run_controller PRIVATE SB_FAKE_GPUWORKER="$<TARGET_FILE:fake-gpuworker>")
synbench_add_unit_test(tst_results SOURCES tst_results.cpp LIBS synbench_appcore)
synbench_add_unit_test(tst_hardening SOURCES tst_hardening.cpp LIBS synbench_appcore)
synbench_add_unit_test(tst_single_instance SOURCES tst_single_instance.cpp LIBS synbench_appcore)
# The real executable for the two-process test.
if(TARGET synbench_app)
    target_compile_definitions(tst_single_instance PRIVATE SYNBENCH_EXE="$<TARGET_FILE:synbench_app>")
    add_dependencies(tst_single_instance synbench_app)
endif()

# The GPU tests, to run (or skip) as a group: ctest -L gpu.
foreach(t tst_gpu_model tst_gpu_scene tst_gpu_loaders tst_gpu_worker tst_gpu_client tst_gpu_formula tst_gpu_test tst_gpu_backends tst_gpu_present tst_gpu_signature tst_gpu_native)
    if(TARGET ${t})
        set_property(TEST ${t} APPEND PROPERTY LABELS gpu)
    endif()
endforeach()
