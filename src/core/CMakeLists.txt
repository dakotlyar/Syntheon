# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(synbench_core STATIC
    enums.h enums.cpp
    result.h result.cpp
    scoring.h scoring.cpp
    options.h options.cpp
    detail.h detail.cpp
    units.h units.cpp
    test_info.h
    log_line.h
    itest.h itest.cpp
    registry.h registry.cpp
    selection_model.h selection_model.cpp
    run_sink.h
    power_inhibitor.h
    orchestrator.h orchestrator.cpp
    result_rules.h result_rules.cpp
)

# P21: core links QtCore only, so a headless Phase 9 CLI can reuse it.
# synbench_privileged is QtCore-only too: the PrivilegedOps interface tests
# reach through their contexts (§2.7).
target_link_libraries(synbench_core PUBLIC Qt6::Core synbench_privileged)
target_include_directories(synbench_core PUBLIC "${PROJECT_SOURCE_DIR}/src")

add_subdirectory(power)
