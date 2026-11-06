# SPDX-License-Identifier: GPL-3.0-or-later
# The platform PowerInhibitor implementations (D8). A separate target so
# syntheon_core itself keeps linking QtCore only (P21): the Linux one needs
# QtDBus.
if(WIN32)
    add_library(syntheon_power STATIC power_inhibitor_win.cpp)
    target_link_libraries(syntheon_power PUBLIC syntheon_core)
elseif(UNIX)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    add_library(syntheon_power STATIC logind_power_inhibitor.h logind_power_inhibitor.cpp)
    target_link_libraries(syntheon_power PUBLIC syntheon_core PRIVATE Qt6::DBus)
endif()
