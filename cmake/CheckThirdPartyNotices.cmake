# SPDX-License-Identifier: GPL-3.0-or-later
# Fails if THIRD-PARTY-NOTICES.txt lacks a component SynBench ships, or a
# component's licence text. Run as a CTest test:
#   cmake -DNOTICES=<build>/THIRD-PARTY-NOTICES.txt "-DEXPECT=<title>|<phrase>;..." -P cmake/CheckThirdPartyNotices.cmake
# Each EXPECT entry is a section title and a phrase that must follow it (from
# that component's licence text).
if(NOT EXISTS "${NOTICES}")
    message(FATAL_ERROR "${NOTICES} does not exist")
endif()
file(READ "${NOTICES}" text)
set(missing "")
foreach(entry IN LISTS EXPECT)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 title)
    list(GET parts 1 phrase)
    # The section: from its title line to the next section's rule.
    string(FIND "${text}" "\n${title}\nLicence: " at)
    if(at LESS 0)
        list(APPEND missing "no section '${title}'")
        continue()
    endif()
    string(SUBSTRING "${text}" ${at} -1 rest)
    string(FIND "${rest}" "\n\n\n================" next)
    if(next GREATER 0)
        string(SUBSTRING "${rest}" 0 ${next} rest)
    endif()
    string(FIND "${rest}" "${phrase}" found)
    if(found LESS 0)
        list(APPEND missing "section '${title}' lacks '${phrase}'")
    endif()
endforeach()
if(missing)
    list(JOIN missing "\n  " missing)
    message(FATAL_ERROR "THIRD-PARTY-NOTICES.txt is incomplete:\n  ${missing}")
endif()
list(LENGTH EXPECT n)
message(STATUS "THIRD-PARTY-NOTICES.txt has all ${n} expected sections")
