# SPDX-License-Identifier: GPL-3.0-or-later
# The NPU engine layer (Phase 4c design §4): the dlopen shims for the three
# vendor runtimes plus the null fake. Every file here compiles as plain C++ — no
# vendor header, .lib or find_package (P-VENDOR); the vendor runtime is loaded at
# run time from the user's own install. So the build stays dependency-free and a
# CI box with no vendor SDK still builds all of it.
target_sources(syntheon_test_npu PRIVATE
    engine.h engine_dlopen.h
    engine_null.cpp          # the null fake + makeEngine() dispatch (always)
    engine_openvino.cpp      # Intel AI Boost — Q_OS_WIN || Q_OS_LINUX (every target here)
    engine_amd.cpp           # AMD Ryzen AI / Vitis AI EP (always; OS-guarded inside)
    genai_runtime.h genai_runtime.cpp  # the GenAI "second runtime" probe (SDK-free, tested)
    genai_c_api.h            # the GenAI C-API resolve surface (header-only, SDK-free)
)
if(WIN32)
    target_sources(syntheon_test_npu PRIVATE
        engine_qnn.cpp       # Qualcomm QNN — Windows only; the ARM64 body is #if-ed out on x64
    )
endif()
