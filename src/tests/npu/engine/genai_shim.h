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

namespace sb::npu::genai_shim {

// Why the streamer stopped a window (SynGenAiControl::stopReason). The window
// also ends "naturally" when GenAI reaches EOS or max_new_tokens — that keeps
// stopReason at 0 (StopRunning).
enum StopReason : int {
    StopRunning = 0,    // EOS or the max_new_tokens bound ended the window
    StopWindow = 1,     // the 5 s window deadline elapsed (streamer returned STOP)
    StopCancelled = 2,  // the caller's cancel predicate fired (streamer returned CANCEL)
};

// The per-window control block: engine-owned, written by the shim during one
// synchronous generate() call. Single-threaded (one window at a time), so no
// locking is needed. All times are steady_clock nanoseconds since epoch.
struct SynGenAiControl {
    std::int64_t deadlineNs = 0;                // steady_clock::now() + the 5 s window (engine side)
    int (*pollCancelled)(void *user) = nullptr; // per-token Stop poll; may be null
    void *cancelledUser = nullptr;              // passed to pollCancelled
    // Outputs, written by the shim's streamer:
    std::int64_t tokens = 0;                    // generated tokens seen by the streamer
    std::int64_t firstTokenNs = 0;              // steady_clock ns of the first generated token (0 = none)
    int eosSeen = 0;                            // 1 when the EOS token id was generated
    int stopReason = StopRunning;               // one of StopReason
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
using SynGenAiDeleteFn = void (*)(SynGenAiContext *ctx);

} // namespace sb::npu::genai_shim
