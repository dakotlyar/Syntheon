# SPDX-License-Identifier: GPL-3.0-or-later
# Fails if a CMake script that the build runs with `cmake -P` lacks
# cmake_minimum_required: such a script gets no policies from the project,
# so one CMake version runs it with NEW behaviour and an older one with OLD
# (IN_LIST once worked with CMake 4.4 locally and broke with 3.31 in CI).
# Finds every `-P "<...>/<file>.cmake"` in the CMakeLists.txt and cmake/
# files of the source tree. Run as a CTest test:
#   cmake -DSOURCE_DIR=<repo> -P cmake/CheckScriptPolicies.cmake
cmake_minimum_required(VERSION 3.25)
file(GLOB_RECURSE users RELATIVE "${SOURCE_DIR}"
     "${SOURCE_DIR}/CMakeLists.txt" "${SOURCE_DIR}/cmake/*.cmake" "${SOURCE_DIR}/src/*CMakeLists.txt"
     "${SOURCE_DIR}/tests/*CMakeLists.txt" "${SOURCE_DIR}/tools/*CMakeLists.txt")
set(scripts "")
foreach(user IN LISTS users)
    file(READ "${SOURCE_DIR}/${user}" text)
    string(REGEX MATCHALL "-P[ \t]+\"?\\$\\{[A-Z_]+\\}/[^\" )\n]+\\.cmake" refs "${text}")
    get_filename_component(dir "${SOURCE_DIR}/${user}" DIRECTORY)
    foreach(ref IN LISTS refs)
        string(REGEX REPLACE "^-P[ \t]+\"?\\$\\{([A-Z_]+)\\}/" "" rel "${ref}")
        string(REGEX REPLACE "^-P[ \t]+\"?\\$\\{([A-Z_]+)\\}/.*" "\\1" var "${ref}")
        if(var STREQUAL "CMAKE_CURRENT_LIST_DIR" OR var STREQUAL "CMAKE_CURRENT_SOURCE_DIR")
            set(path "${dir}/${rel}")
        else()
            set(path "${SOURCE_DIR}/${rel}")
        endif()
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "${user}: -P script ${rel} not found (looked at ${path})")
        endif()
        list(APPEND scripts "${path}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES scripts)
set(bad "")
foreach(script IN LISTS scripts)
    file(STRINGS "${script}" lines REGEX "^[ \t]*cmake_minimum_required\\(")
    if(NOT lines)
        list(APPEND bad "${script}")
    endif()
endforeach()
list(LENGTH scripts n)
if(n EQUAL 0)
    message(FATAL_ERROR "found no -P scripts at all: the search is broken")
endif()
if(bad)
    list(JOIN bad "\n  " bad)
    message(FATAL_ERROR "-P scripts without cmake_minimum_required:\n  ${bad}")
endif()
message(STATUS "all ${n} -P scripts set cmake_minimum_required")
