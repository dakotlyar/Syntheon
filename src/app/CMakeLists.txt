# SPDX-License-Identifier: GPL-3.0-or-later

# App-side controllers, in a library so they can be unit-tested.
qt_add_library(synbench_appcore STATIC
    controllers/app_controller.h controllers/app_controller.cpp
)
target_link_libraries(synbench_appcore PUBLIC synbench_core synbench_sysinfo synbench_privileged Qt6::Gui)

qt_add_executable(synbench_app WIN32 main.cpp)
set_target_properties(synbench_app PROPERTIES OUTPUT_NAME SynBench)

qt_add_qml_module(synbench_app
    URI SynBench
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/dialogs/AuthDialog.qml
)

target_link_libraries(synbench_app PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    synbench_appcore
    synbench_version
)
if(TARGET synbench-helper)
    add_dependencies(synbench_app synbench-helper)  # pkexec runs it from the same folder (dev builds)
endif()
