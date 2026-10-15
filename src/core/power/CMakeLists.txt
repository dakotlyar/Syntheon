# The platform PowerInhibitor implementations (D8). A separate target so
# synbench_core itself keeps linking QtCore only (P21): the Linux one needs
# QtDBus.
if(WIN32)
    add_library(synbench_power STATIC power_inhibitor_win.cpp)
    target_link_libraries(synbench_power PUBLIC synbench_core)
elseif(UNIX)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    add_library(synbench_power STATIC logind_power_inhibitor.h logind_power_inhibitor.cpp)
    target_link_libraries(synbench_power PUBLIC synbench_core PRIVATE Qt6::DBus)
endif()
