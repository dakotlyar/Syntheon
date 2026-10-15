# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_executable(synbench_app WIN32 main.cpp)
set_target_properties(synbench_app PROPERTIES OUTPUT_NAME SynBench)

qt_add_qml_module(synbench_app
    URI SynBench
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(synbench_app PRIVATE
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    synbench_version
)
