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
)

# P21: core links QtCore only, so a headless Phase 9 CLI can reuse it.
target_link_libraries(synbench_core PUBLIC Qt6::Core)
target_include_directories(synbench_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
