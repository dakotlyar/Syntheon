# SPDX-License-Identifier: GPL-3.0-or-later
# The parts of THIRD-PARTY-NOTICES.txt (D11) that come with Qt's deployment
# (AP-5 M3): the third-party code compiled into the Qt libraries and plugins
# Syntheon ships (PCRE2, HarfBuzz, libpng, libjpeg, md4c, double-conversion,
# the FreeType-derived rasteriser, …), read from Qt's own SBOM
# (<Qt prefix>/sbom/<module>-<version>.spdx.json) for the modules the packages
# deploy, with the full text of each licence; and, on Linux, ICU, whose
# libraries the AppImage carries.
#
#   syntheon_add_qt_deployment_notices()   # after find_package(Qt6), before
#                                          # syntheon_write_third_party_notices()
#
# The licence texts are in packaging/licenses/spdx (SPDX's own texts, see the
# README there); a licence Qt's SBOM names that has no text there is a
# configure error. A missing SBOM is a warning, and the notices check (CTest
# third_party_notices) then fails, because the section is missing.
include_guard(GLOBAL)
include(ThirdPartyNotices)

function(syntheon_add_qt_deployment_notices)
    set(prefix "${QT6_INSTALL_PREFIX}")
    if(NOT prefix)
        get_target_property(_core_location Qt6::Core LOCATION)
        get_filename_component(prefix "${_core_location}" DIRECTORY)
        get_filename_component(prefix "${prefix}/.." ABSOLUTE)
    endif()
    # The modules the packages deploy (the app's Qt libraries, QML imports
    # and plugins); qtwayland only on Linux. Tools, docs and translations
    # aren't shipped.
    set(modules qtbase qtdeclarative qtsvg)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND modules qtwayland)
    endif()

    set(components "")   # "<name>\t<version>\t<licence>\t<copyright>" entries, deduplicated
    set(seen "")
    set(licences "")
    set(extracted_ids "")
    set(icu_version "")
    set(found_modules "")
    foreach(module IN LISTS modules)
        set(sbom "${prefix}/sbom/${module}-${Qt6_VERSION}.spdx.json")
        if(NOT EXISTS "${sbom}")
            message(WARNING "Qt's SBOM ${sbom} isn't there: THIRD-PARTY-NOTICES.txt can't list ${module}'s bundled code")
            continue()
        endif()
        list(APPEND found_modules ${module})
        file(READ "${sbom}" json)
        # Texts of LicenseRef-* licences, from this SBOM.
        string(JSON n_extracted ERROR_VARIABLE err LENGTH "${json}" hasExtractedLicensingInfos)
        if(NOT err AND n_extracted GREATER 0)
            math(EXPR last "${n_extracted} - 1")
            foreach(i RANGE ${last})
                string(JSON id GET "${json}" hasExtractedLicensingInfos ${i} licenseId)
                if(NOT id IN_LIST extracted_ids)
                    string(JSON text GET "${json}" hasExtractedLicensingInfos ${i} extractedText)
                    list(APPEND extracted_ids "${id}")
                    set(_extracted_${id} "${text}")
                    set(_extracted_${id} "${text}" PARENT_SCOPE)
                endif()
            endforeach()
        endif()
        string(JSON n LENGTH "${json}" packages)
        math(EXPR last "${n} - 1")
        foreach(i RANGE ${last})
            string(JSON spdxid GET "${json}" packages ${i} SPDXID)
            string(JSON name GET "${json}" packages ${i} name)
            string(JSON version ERROR_VARIABLE err GET "${json}" packages ${i} versionInfo)
            if(err)
                set(version "")
            endif()
            if(name STREQUAL "ICU")
                set(icu_version "${version}")
            endif()
            # Bundled third-party code only (not the system libraries Qt
            # links), and not the parts only Qt's tests and build tools use.
            if(NOT spdxid MATCHES "-3rdparty-" OR spdxid MATCHES "-system-3rdparty-")
                continue()
            endif()
            if(name MATCHES "^(Test|TestInternals|Bootstrap)")
                continue()
            endif()
            string(JSON licence ERROR_VARIABLE err GET "${json}" packages ${i} licenseConcluded)
            if(err OR licence STREQUAL "NOASSERTION" OR licence STREQUAL "")
                continue()
            endif()
            string(JSON copyright ERROR_VARIABLE err GET "${json}" packages ${i} copyrightText)
            if(err OR copyright STREQUAL "NOASSERTION")
                set(copyright "")
            endif()
            string(REGEX REPLACE "^[A-Za-z0-9]+_Attribution_" "" shown "${name}")
            string(REGEX REPLACE "^Bundled" "" shown "${shown}")
            string(REGEX REPLACE "Private$" "" shown "${shown}")
            if(version STREQUAL "unknown")
                set(version "")
            endif()
            # The same code under two targets (BundledLibpng / PngPrivate):
            # once, by its licence, version and copyright.
            string(SUBSTRING "${copyright}" 0 80 key)
            string(MD5 key "${licence}|${version}|${key}")
            if(key IN_LIST seen)
                continue()
            endif()
            list(APPEND seen ${key})
            # A list entry can't hold ';' (and the fields are tab-separated).
            string(REPLACE ";" "<semicolon>" copyright "${copyright}")
            string(REPLACE "\t" " " copyright "${copyright}")
            list(APPEND components "${shown}\t${version}\t${licence}\t${copyright}")
            # The licences to reproduce: of "A OR B" the first is the one used
            # (Syntheon takes FTL for the FreeType rasteriser, AFL-2.1 for the
            # D-Bus headers); every licence of "A AND B" and its exception.
            string(REGEX REPLACE " OR .*" "" chosen "${licence}")
            string(REGEX REPLACE "[()]" " " chosen "${chosen}")
            string(REGEX REPLACE " (AND|WITH) " ";" chosen "${chosen}")
            foreach(id IN LISTS chosen)
                string(STRIP "${id}" id)
                if(id AND NOT id IN_LIST licences)
                    list(APPEND licences "${id}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    if(NOT found_modules)
        return()
    endif()

    list(SORT components CASE INSENSITIVE)
    list(SORT licences CASE INSENSITIVE)
    list(JOIN found_modules ", " module_list)
    string(CONCAT out "These components are compiled into the Qt libraries and plugins Syntheon ships, as\n"
                      "Qt's SBOM lists them for ${module_list}. Where Qt offers a choice of\n"
                      "licences, the first one named is the one used.\n\n")
    foreach(entry IN LISTS components)
        string(REPLACE "\t" ";" f "${entry}")
        list(GET f 0 c_name)
        list(GET f 1 c_version)
        list(GET f 2 c_licence)
        list(GET f 3 c_copyright)
        string(REPLACE "<semicolon>" ";" c_copyright "${c_copyright}")
        set(line "  ${c_name}")
        if(c_version)
            string(APPEND line " ${c_version}")
        endif()
        string(APPEND out "${line} (${c_licence})\n")
        if(c_copyright)
            string(REPLACE "\n" "\n      " c_copyright "${c_copyright}")
            string(APPEND out "      ${c_copyright}\n")
        endif()
    endforeach()
    if("FTL" IN_LIST licences)
        # The FreeType Licence asks for this credit in the documentation.
        string(APPEND out "\nPortions of this software are copyright © 2000-2016 The FreeType Project\n"
                          "(www.freetype.org). All rights reserved.\n")
    endif()
    string(APPEND out "\nThe licence texts:\n")
    foreach(id IN LISTS licences)
        if(id STREQUAL "LicenseRef-Qt-Commercial")
            continue()
        endif()
        string(APPEND out "\n---- ${id} ----\n\n")
        if(id MATCHES "^LicenseRef-")
            if(NOT id IN_LIST extracted_ids)
                message(FATAL_ERROR "Qt's SBOM names ${id} without its text")
            endif()
            string(APPEND out "${_extracted_${id}}\n")
        else()
            set(file "${PROJECT_SOURCE_DIR}/packaging/licenses/spdx/${id}.txt")
            if(NOT EXISTS "${file}")
                message(FATAL_ERROR "Qt bundles code under ${id}, and packaging/licenses/spdx/${id}.txt is missing: "
                                    "add its text (packaging/licenses/README.md)")
            endif()
            file(READ "${file}" text)
            string(APPEND out "${text}\n")
        endif()
    endforeach()
    set(path "${PROJECT_BINARY_DIR}/notices/qt-bundled.txt")
    file(WRITE "${path}" "${out}")
    syntheon_add_notice(NAME "Third-party code inside Qt" VERSION ${Qt6_VERSION} LICENCE "various, listed below"
        FILE "${path}")

    # ICU: the Linux Qt links it, and the AppImage carries its libraries.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND icu_version)
        if(NOT icu_version MATCHES "^73\\.")
            message(FATAL_ERROR "Qt's ICU is ${icu_version}; packaging/licenses/ICU-73.txt is ICU 73's licence: replace it")
        endif()
        syntheon_add_notice(NAME "ICU" VERSION ${icu_version} LICENCE "Unicode-DFS-2016 (ICU 73 licence)"
            FILE "${PROJECT_SOURCE_DIR}/packaging/licenses/ICU-73.txt"
            NOTE "The ICU libraries (libicu*.so.73) ship inside the AppImage with Qt.")
    endif()
    set(SYNTHEON_QT_NOTICE_ICU "${icu_version}" PARENT_SCOPE)
endfunction()
