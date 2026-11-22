// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The engine ↔ GenAI-shim C ABI (DD-1 §5.4 Option A). The OpenVINO GenAI LLM
// path is C++-only (ov::genai::LLMPipeline — there is no C ABI to dlsym), so
// the P-VENDOR amendment Dave pre-approved adds the GenAI headers as a build
// input for ONE small shared module (genai_shim.cpp) that wraps the pipeline
// behind this stable, plain-C boundary. The engine resolves these entry points
// by name from the loaded shim module (never links it — the runtime stays the
// user's install, LoadLibrary'd at run time, never bundled). This header is OUR
// code: it includes no GenAI header, so the engine TU (and any SDK-free build
// or CI box) compiles without any vendor SDK.
//
// Nothing here throws: every GenAI exception is caught inside the shim and
// reported through the error buffers; a missing/damaged shim or runtime is a
// clean "LLM runtime missing" error, never a crash.

#include <cstddef>
#include <cstdint>

// The export annotation for the shim's extern "C" entry points. The shim
// module defines SYN_GENAI_SHIM_BUILD (genai_shim.cpp) so MSVC builds export
// the symbols (dllexport) — without it a Windows shim DLL would export
// nothing and the engine's GetProcAddress would always fail. GCC/Clang get
// explicit default visibility so a -fvisibility=hidden build still exports.
#if defined(_WIN32) && defined(SYN_GENAI_SHIM_BUILD)
#  define SYN_GENAI_EXPORT __declspec(dllexport)
#elif defined(_WIN32)
#  define SYN_GENAI_EXPORT
#elif defined(__GNUC__) || defined(__clang__)
#  define SYN_GENAI_EXPORT __attribute__((visibility("default")))
#else
#  define SYN_GENAI_EXPORT
#endif

namespace sb::npu::genai_shim {

// Why the streamer stopped a window (SynGenAiControl::stopReason). A window
// also ends "naturally" when GenAI reaches EOS or max_new_tokens — that keeps
// stopReason at 0 (StopRunning), which is how the shim tells a natural
// completion from a streamer-stop (needed for the §5.3 floor).
enum StopReason : int {
    StopRunning = 0,    // EOS or the max_new_tokens bound ended the generation
    StopWindow = 1,     // the 5 s window deadline elapsed (streamer returned STOP)
    StopCancelled = 2,  // the caller's cancel predicate fired (streamer returned CANCEL)
};

// The per-window control block: engine-owned, read/written by the shim during
// the window's synchronous generate() calls (single-threaded, no locking).
// The engine loops generate() until the window deadline; the shim ACCUMULATES
// across those calls, so `tokens`/`firstTokenNs`/`decodeNs` span the whole
// window, not one call. All times are steady_clock nanoseconds since epoch.
struct SynGenAiControl {
    std::int64_t deadlineNs = 0;                // steady_clock::now() + the 5 s window (engine side)
    int (*pollCancelled)(void *user) = nullptr; // per-token Stop poll; may be null
    void *cancelledUser = nullptr;              // passed to pollCancelled
    // Policy input (engine side):
    std::int64_t minNewTokens = 0;              // the §5.3 floor: a naturally-EOS'd call below this is garbage
    // Outputs (written by the shim across the window):
    std::int64_t tokens = 0;                    // total generated tokens (all calls)
    std::int64_t decodeTokens = 0;              // accumulated (callTokens−1) per call: the tokens the decode
                                                // spans covered — each call's FIRST token is the prefill's, so
                                                // subtracting one per CALL (not per window) keeps llm.tps honest
    std::int64_t firstTokenNs = 0;              // first generated token of the window (ttft anchor; 0 = none)
    std::int64_t decodeNs = 0;                  // accumulated (last−first) token span per call: the DECODE time,
                                                // excluding every call's prefill/tokenization/detokenization
    int eosSeen = 0;                            // 1 when the EOS token id was generated
    int floorBroken = 0;                        // 1 when a naturally-ended call produced < minNewTokens
    int stopReason = StopRunning;               // why the LAST streamer stop happened
};

// Opaque handle to one LLMPipeline (the shim owns it; created per model dir).
struct SynGenAiContext;

// The error-buffer length every entry point takes (messages are truncated).
constexpr std::size_t kSynGenAiErrorLen = 512;

// The resolved entry points, as typed function pointers (mirror resolveApi):
// each is null-checked at resolve time, so a missing symbol fails the LLM
// runtime load cleanly with the symbol name.
using SynGenAiCreateFn = SynGenAiContext *(*)(const char *model_dir_utf8, const char *device,
                                              const char *tokenizers_path_utf8, std::int64_t eos_token_id,
                                              char *errbuf, std::size_t errbuf_len);
using SynGenAiGenerateFn = int (*)(SynGenAiContext *ctx, const char *prompt_utf8, std::int64_t max_new_tokens,
                                   SynGenAiControl *control, char *errbuf, std::size_t errbuf_len);
using SynGenAiVersionFn = int (*)(char *buf, std::size_t buf_len);   // 0 = filled, non-zero = unknown
using SynGenAiDeleteFn = void (*)(SynGenAiContext *ctx);

} // namespace sb::npu::genai_shim

// The shim's exported entry points. Declared OUTSIDE the namespace in an
// extern "C" block: genai_shim.cpp defines these exact entities (extern "C",
// global scope), so the ABI types are qualified with the namespace. A previous
// revision declared them INSIDE the namespace without extern "C" — C++-mangled
// sb::npu::genai_shim::syn_genai_* declarations that were never defined, so a
// Windows shim DLL exported nothing (AP-5 round 2, finding 1).
extern "C" {
SYN_GENAI_EXPORT sb::npu::genai_shim::SynGenAiContext *syn_genai_pipeline_create(
    const char *model_dir_utf8, const char *device, const char *tokenizers_path_utf8, std::int64_t eos_token_id,
    char *errbuf, std::size_t errbuf_len);
SYN_GENAI_EXPORT int syn_genai_generate(sb::npu::genai_shim::SynGenAiContext *ctx, const char *prompt_utf8,
                                        std::int64_t max_new_tokens, sb::npu::genai_shim::SynGenAiControl *control,
                                        char *errbuf, std::size_t errbuf_len);
SYN_GENAI_EXPORT int syn_genai_runtime_version(char *buf, std::size_t buf_len);
SYN_GENAI_EXPORT void syn_genai_pipeline_delete(sb::npu::genai_shim::SynGenAiContext *ctx);
} // extern "C"
