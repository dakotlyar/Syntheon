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
# Two SDK layouts are accepted (probed by the file layout): the pip wheel
# package dir (<dir>/include + <dir>/lib) and the OpenVINO runtime root
# (<dir>/runtime/include + <dir>/runtime/lib/intel64/Release). On Windows the
# import libs are openvino_genai.lib (the shim's own symbols) AND openvino.lib
# (LLMPipeline's ctor instantiates ov::Any, whose destructor lives in
# openvino.dll — linking only the genai lib would LNK2019). The runtime itself
# is never bundled: it stays the user's install, LoadLibrary'd at run time, so a
# missing GenAI runtime is a clean "LLM runtime missing" error on the llm
# workload — never a launch failure. Without the SDK dir the engine still builds
# and only the LLM workload is unavailable.
set(SYNTHEON_OPENVINO_GENAI_SDK_DIR "" CACHE PATH
    "Dir with the OpenVINO GenAI headers (include/openvino/genai) + import libs (lib/openvino_genai.lib + lib/openvino.lib on Windows)")
if(SYNTHEON_OPENVINO_GENAI_SDK_DIR)
    # Locate the headers/import-libs under the SDK dir (both layouts above).
    set(_genai_sdk "${SYNTHEON_OPENVINO_GENAI_SDK_DIR}")
    if(NOT EXISTS "${_genai_sdk}/include/openvino/genai/llm_pipeline.hpp"
       AND EXISTS "${_genai_sdk}/runtime/include/openvino/genai/llm_pipeline.hpp")
        set(_genai_sdk "${_genai_sdk}/runtime")
    endif()
    set(_genai_inc "${_genai_sdk}/include")
    if(EXISTS "${_genai_sdk}/lib/intel64/Release")
        set(_genai_lib "${_genai_sdk}/lib/intel64/Release")
    else()
        set(_genai_lib "${_genai_sdk}/lib")
    endif()

    add_library(syn_openvino_genai_shim SHARED genai_shim.cpp)
    target_include_directories(syn_openvino_genai_shim PRIVATE "${_genai_inc}")
    if(NOT EXISTS "${_genai_inc}/openvino/openvino.hpp")
        # pip layout: the core headers live in the sibling openvino package's include/.
        target_include_directories(syn_openvino_genai_shim PRIVATE
            "${_genai_sdk}/../openvino/include")
    endif()
    if(WIN32)
        target_link_libraries(syn_openvino_genai_shim PRIVATE
            "${_genai_lib}/openvino_genai.lib"
            "${_genai_lib}/openvino.lib")
        # SMOKE: confirm the pip openvino-genai wheel ships openvino_genai.lib +
        # openvino.lib beside the DLLs (the Windows wheels carry the import
        # libs); if not, generate them with `lib /def:...` / dumpbin.
        # The pip/toolkit GenAI DLLs are Release (/MD); a Debug (/MDd) shim
        # would pass std::string/std::filesystem::path across an STL-ABI
        # boundary (and _wputenv_s would write to the wrong CRT). Force the
        # Release-style runtime + iterator ABI on the shim so it is usable in
        # every build config (AP-5 4c).
        set_target_properties(syn_openvino_genai_shim PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        target_compile_definitions(syn_openvino_genai_shim PRIVATE _ITERATOR_DEBUG_LEVEL=0)
    else()
        # Linux: the shim links with UNDEFINED ov::genai::* symbols — they are
        # resolved at dlopen time from the user's openvino_genai module, which
        # the engine loads with RTLD_GLOBAL first (headers only, no lib needed).
        target_link_options(syn_openvino_genai_shim PRIVATE -Wl,--allow-shlib-undefined)
    endif()
    set_target_properties(syn_openvino_genai_shim PROPERTIES
        OUTPUT_NAME syn_openvino_genai_shim)   # the name the engine probes next to the app module
    # The engine probes QCoreApplication::applicationDirPath(), so the shim must
    # ride NEXT TO the app module: Windows beside Syntheon.exe, Linux beside the
    # binary (the same bindir — not the libdir, or the probe would miss it).
    include(GNUInstallDirs)
    install(TARGETS syn_openvino_genai_shim
        RUNTIME DESTINATION .
        LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
