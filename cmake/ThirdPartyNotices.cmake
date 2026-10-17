# SPDX-License-Identifier: GPL-3.0-or-later
# THIRD-PARTY-NOTICES.txt (Phase 2 design §11.3, D11): one file with the
# licence of everything SynBench ships that isn't its own, generated at
# configure time from the licence files of the pinned sources (and, for Qt
# and the fonts, from the texts kept in this repository). It is written to
# <build>/THIRD-PARTY-NOTICES.txt; the packages install it beside LICENSE.txt.
#
#   synbench_add_notice(NAME <name> [VERSION <v>] LICENCE <spdx> FILE <file> [NOTE <text>])
#   synbench_write_third_party_notices()   # once, after every notice is added
include_guard(GLOBAL)

function(synbench_add_notice)
    cmake_parse_arguments(ARG "" "NAME;VERSION;LICENCE;FILE;NOTE" "" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_LICENCE OR NOT ARG_FILE)
        message(FATAL_ERROR "synbench_add_notice needs NAME, LICENCE and FILE")
    endif()
    if(NOT EXISTS "${ARG_FILE}")
        message(FATAL_ERROR "Licence file for ${ARG_NAME} not found: ${ARG_FILE}")
    endif()
    # One property per field: names and notes may contain ';', which a single
    # list entry could not hold.
    get_property(index GLOBAL PROPERTY SYNBENCH_NOTICE_COUNT)
    if("${index}" STREQUAL "")  # an unset property leaves the variable undefined
        set(index 0)
    endif()
    foreach(field NAME VERSION LICENCE FILE NOTE)
        set_property(GLOBAL PROPERTY SYNBENCH_NOTICE_${index}_${field} "${ARG_${field}}")
    endforeach()
    math(EXPR count "${index} + 1")
    set_property(GLOBAL PROPERTY SYNBENCH_NOTICE_COUNT "${count}")
endfunction()

function(synbench_write_third_party_notices)
    get_property(n GLOBAL PROPERTY SYNBENCH_NOTICE_COUNT)
    set(rule "================================================================================")
    set(out "SynBench third-party notices\n${rule}\n\n")
    string(APPEND out "SynBench is free software under the GNU General Public License, version 3 or\n"
                      "later (LICENSE.txt). It includes or uses the third-party software below, each\n"
                      "under its own licence, reproduced in full.\n\n")
    set(sections "")
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
        foreach(field NAME VERSION LICENCE FILE NOTE)
            get_property(${field} GLOBAL PROPERTY SYNBENCH_NOTICE_${i}_${field})
        endforeach()
        set(title "${NAME}")
        if(VERSION)
            string(APPEND title " ${VERSION}")
        endif()
        string(APPEND out "  ${title} (${LICENCE})\n")
        file(READ "${FILE}" text)
        string(APPEND sections "\n\n${rule}\n${title}\nLicence: ${LICENCE}\n")
        if(NOTE)
            string(APPEND sections "${NOTE}\n")
        endif()
        string(APPEND sections "${rule}\n\n${text}")
    endforeach()
    string(APPEND out "${sections}")
    # Rewritten only when it changes, so a reconfigure doesn't re-trigger packaging.
    file(WRITE "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt.tmp" "${out}")
    file(COPY_FILE "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt.tmp" "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" ONLY_IF_DIFFERENT)
    file(REMOVE "${PROJECT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt.tmp")
endfunction()
