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
)
if(WIN32)
    target_sources(syntheon_test_npu PRIVATE
        engine_qnn.cpp       # Qualcomm QNN — Windows only; the ARM64 body is #if-ed out on x64
    )
endif()

# The OpenVINO GenAI LLM shim (DD-1 §5.4 Option A — the P-VENDOR amendment Dave
# pre-approved): a small shared module wrapping ov::genai::LLMPipeline behind the
# stable C boundary in genai_shim.h, so the engine (and any SDK-free build or CI
# box) never compiles a GenAI header. This shim is the ONLY vendor build input,
# and it is built only when a GenAI SDK dir is configured:
#
#   cmake -DSYNTHEON_OPENVINO_GENAI_SDK_DIR=<dir> ...
#
# where <dir> holds include/openvino/genai/*.hpp (plus the openvino core headers
# under the same include/ — the pip openvino_genai wheel keeps them in the
# sibling openvino package, which this block adds when needed) and, on Windows,
# lib/openvino_genai.lib (the pip wheel ships both). The runtime itself is never
# bundled: it stays the user's install, LoadLibrary'd at run time, so a missing
# GenAI runtime is a clean "LLM runtime missing" error on the llm workload —
# never a launch failure. Without the SDK dir the engine still builds and only
# the LLM workload is unavailable.
set(SYNTHEON_OPENVINO_GENAI_SDK_DIR "" CACHE PATH
    "Dir with the OpenVINO GenAI headers (include/openvino/genai) + import lib (lib/openvino_genai.lib on Windows)")
if(SYNTHEON_OPENVINO_GENAI_SDK_DIR)
    add_library(syn_openvino_genai_shim SHARED genai_shim.cpp)
    target_include_directories(syn_openvino_genai_shim PRIVATE
        "${SYNTHEON_OPENVINO_GENAI_SDK_DIR}/include")
    if(NOT EXISTS "${SYNTHEON_OPENVINO_GENAI_SDK_DIR}/include/openvino/openvino.hpp")
        # pip layout: the core headers live in the sibling openvino package's include/.
        target_include_directories(syn_openvino_genai_shim PRIVATE
            "${SYNTHEON_OPENVINO_GENAI_SDK_DIR}/../openvino/include")
    endif()
    if(WIN32)
        target_link_libraries(syn_openvino_genai_shim PRIVATE
            "${SYNTHEON_OPENVINO_GENAI_SDK_DIR}/lib/openvino_genai.lib")
        # SMOKE: confirm the pip openvino-genai wheel ships openvino_genai.lib
        # beside the DLL (it should — the Windows wheels carry the import lib);
        # if not, generate it with `lib /def:openvino_genai.def` / dumpbin.
    else()
        # Linux: the shim links with UNDEFINED ov::genai::* symbols — they are
        # resolved at dlopen time from the user's openvino_genai module, which
        # the engine loads with RTLD_GLOBAL first (headers only, no lib needed).
        target_link_options(syn_openvino_genai_shim PRIVATE -Wl,--allow-shlib-undefined)
    endif()
    set_target_properties(syn_openvino_genai_shim PROPERTIES
        OUTPUT_NAME syn_openvino_genai_shim)   # the name the engine probes next to the app module
endif()
