# SPDX-License-Identifier: GPL-3.0-or-later
# Compiler flags shared by every SynBench target.
#
# Baseline (D11, P17): x86-64-v2 is set explicitly. A CachyOS (or other
# distro) compiler may default to x86-64-v3, and the result then SIGILLs on
# older CPUs. MSVC has no x86-64-v2 switch; its x64 default (SSE2) is below the
# baseline, so it is left as is.
if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sb_cpu)
    if(_sb_cpu MATCHES "^(x86_64|amd64)$")
        add_compile_options(-march=x86-64-v2 -mtune=generic)
    elseif(_sb_cpu MATCHES "^(aarch64|arm64)$")
        add_compile_options(-march=armv8-a)
    endif()
endif()
