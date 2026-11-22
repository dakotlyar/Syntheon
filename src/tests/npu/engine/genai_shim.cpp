// SPDX-License-Identifier: GPL-3.0-or-later
// The OpenVINO GenAI shim (DD-1 §5.4 Option A). Compiled ONLY when a GenAI SDK
// dir is configured (SYNTHEON_OPENVINO_GENAI_SDK_DIR, see engine/CMakeLists.txt);
// the engine and any SDK-free build never see this file. It wraps
// ov::genai::LLMPipeline behind the stable C boundary in genai_shim.h, so the
// engine drives the LLM workload through plain resolved function pointers —
// the P-VENDOR amendment: GenAI headers/import-lib are a build input, the
// runtime stays the user's install (LoadLibrary at run time, never bundled).
//
// Everything that can throw (GenAI throws ov::Exception on a compile/device
// failure) is caught here and reported through the error buffer — nothing
// crosses the boundary. The streamer counts generated tokens, records the
// first-token time (ttft) and the EOS sighting, and stops a call when the
// engine's 5 s window elapses or its Stop predicate fires (P-STOP ≤ 250 ms:
// tokens on the NPU land every ~30 ms). The engine loops generate() calls
// until the deadline; this TU accumulates the per-call results into the
// window control block.

#define SYN_GENAI_SHIM_BUILD   // dllexport the entry points on Windows (genai_shim.h)
#include "tests/npu/engine/genai_shim.h"

#include <openvino/genai/generation_config.hpp>
#include <openvino/genai/llm_pipeline.hpp>
#include <openvino/genai/streamer_base.hpp>
#include <openvino/genai/version.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cstdlib>
#endif

namespace {

using namespace sb::npu::genai_shim;

std::int64_t steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void setError(char *errbuf, std::size_t errbuf_len, const char *msg)
{
    if (errbuf && errbuf_len > 0)
        std::snprintf(errbuf, errbuf_len, "%s", msg);
}

// GenAI's C++ Tokenizer loads the openvino-tokenizers extension from the path
// in OPENVINO_TOKENIZERS_PATH_GENAI (tokenizers_relative_to_genai(): the
// tokenizers lib BESIDE the genai module). A pip install puts genai and
// tokenizers in sibling package dirs, so that default path is wrong and the
// pipeline would fail to load the tokenizer IRs. Point the env var at the
// actual tokenizers lib (only if the user didn't already set a NON-EMPTY
// value — GenAI's ScopedVar would otherwise leave ours in place; an empty
// value counts as unset). On Windows the CRT-wide env (_wputenv_s) must be
// used: GenAI reads it with _wgetenv, which does not see
// SetEnvironmentVariableA/qputenv changes. Does not throw (the wide conversion
// is bounded); callers still keep it inside the try so any STL allocation in
// the wide string is caught.
void ensureTokenizersPath(const char *tokenizers_path_utf8)
{
    if (!tokenizers_path_utf8 || tokenizers_path_utf8[0] == '\0')
        return;
#if defined(_WIN32)
    const wchar_t *existing = _wgetenv(L"OPENVINO_TOKENIZERS_PATH_GENAI");
    if (existing && existing[0] != L'\0')
        return;
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, tokenizers_path_utf8, -1, nullptr, 0);
    if (wideLen <= 0)
        return;
    std::wstring wide(std::size_t(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tokenizers_path_utf8, -1, wide.data(), wideLen);
    _wputenv_s(L"OPENVINO_TOKENIZERS_PATH_GENAI", wide.c_str());
#else
    const char *existing = getenv("OPENVINO_TOKENIZERS_PATH_GENAI");
    if (existing && existing[0] != '\0')
        return;
    setenv("OPENVINO_TOKENIZERS_PATH_GENAI", tokenizers_path_utf8, 1);
#endif
}

// Counts ONE generate() call's generated tokens (callTokens), records that
// call's first/last token times (callFirstNs/callLastNs), notes the EOS id,
// and stops the call when the window elapses or the caller's Stop predicate
// fires. GenAI calls write() once per new token on the stateful batch-1 greedy
// path; the base write(vector) routes every token through this override. The
// EOS token itself may or may not reach the streamer (GenAI filters it
// internally in some paths) — eosSeen is informational only, never
// load-bearing (the §5.3 floor uses the token counts).
class SynStreamer final : public ov::genai::StreamerBase
{
public:
    SynStreamer(SynGenAiControl *control, std::int64_t eosTokenId, std::int64_t *callFirstNs,
                std::int64_t *callLastNs, std::int64_t *callTokens)
        : m_control(control), m_eosTokenId(eosTokenId), m_callFirstNs(callFirstNs), m_callLastNs(callLastNs),
          m_callTokens(callTokens)
    {
    }
    ov::genai::StreamingStatus write(std::int64_t token) override
    {
        const std::int64_t nowNs = steadyNs();
        if (*m_callTokens == 0)
            *m_callFirstNs = nowNs;
        *m_callLastNs = nowNs;
        ++*m_callTokens;
        if (m_eosTokenId >= 0 && token == m_eosTokenId)
            m_control->eosSeen = 1;
        if (m_control->pollCancelled && m_control->pollCancelled(m_control->cancelledUser)) {
            m_control->stopReason = StopCancelled;
            return ov::genai::StreamingStatus::CANCEL;
        }
        if (nowNs >= m_control->deadlineNs) {
            m_control->stopReason = StopWindow;
            return ov::genai::StreamingStatus::STOP;
        }
        return ov::genai::StreamingStatus::RUNNING;
    }
    void end() override {}

private:
    SynGenAiControl *m_control;
    std::int64_t m_eosTokenId;
    std::int64_t *m_callFirstNs;
    std::int64_t *m_callLastNs;
    std::int64_t *m_callTokens;
};

// The opaque handle the C boundary returns: the ABI header declares
// sb::npu::genai_shim::SynGenAiContext; this internal struct is the concrete
// object behind it (named differently so it can't collide with the ABI type
// through the using-directive).
struct PipelineHolder {
    std::unique_ptr<ov::genai::LLMPipeline> pipeline;
    std::int64_t eosTokenId = -1;
};

} // namespace

extern "C" {

SYN_GENAI_EXPORT SynGenAiContext *syn_genai_pipeline_create(const char *model_dir_utf8, const char *device,
                                                            const char *tokenizers_path_utf8,
                                                            std::int64_t eos_token_id, char *errbuf,
                                                            std::size_t errbuf_len)
{
    if (!model_dir_utf8 || !device) {
        setError(errbuf, errbuf_len, "syn_genai_pipeline_create: null argument");
        return nullptr;
    }
    try {
        // The tokenizers env hook may allocate (the wide string); it must not
        // throw across the boundary, so it lives inside the try.
        ensureTokenizersPath(tokenizers_path_utf8);
        // SMOKE: the ctor form the Nivedellir smoke proved on this pack and IR
        // — LLMPipeline(model_dir, "NPU") — plus the fail-closed NPUW
        // properties (AP-5, 2026-11-22): with NPUW_DEVICES="NPU" and
        // NPUW_FALLBACK_EXEC=false GenAI throws hard on a VCL/compile/exec
        // failure instead of silently falling back to CPU, so the EC-P9 guard
        // cannot be bypassed. SMOKE: confirm TinyLlama needs no CPU tail op
        // under this config (if it does, fall back to a
        // NPU_DEVICE_ALLOC_MEM_SIZE delta check). GenAI reads the model dir's
        // own generation_config.json + chat template, tokenizes the prompt
        // internally (CPU), staticizes the KV state and compiles on "NPU".
        // The model dir comes in as UTF-8, converted with the C++20 char8_t
        // path ctor so a non-ASCII cache root never round-trips through the
        // Windows ANSI codepage (the engine's L1 rule).
        auto ctx = std::make_unique<PipelineHolder>();
        ctx->pipeline = std::make_unique<ov::genai::LLMPipeline>(
            std::filesystem::path(
                std::u8string_view(reinterpret_cast<const char8_t *>(model_dir_utf8))),
            std::string(device),
            ov::AnyMap{{"NPUW_DEVICES", std::string("NPU")}, {"NPUW_FALLBACK_EXEC", false}});
        ctx->eosTokenId = eos_token_id;
        return reinterpret_cast<SynGenAiContext *>(ctx.release());
    } catch (const std::exception &e) {
        setError(errbuf, errbuf_len, "failed to create the LLM pipeline on device 'NPU': ");
        if (errbuf && errbuf_len > 0)
            std::snprintf(errbuf + std::char_traits<char>::length(errbuf), errbuf_len - std::char_traits<char>::length(errbuf),
                          "%s", e.what());
    } catch (...) {
        setError(errbuf, errbuf_len, "failed to create the LLM pipeline on device 'NPU' (unknown error)");
    }
    return nullptr;
}

SYN_GENAI_EXPORT int syn_genai_generate(SynGenAiContext *ctx, const char *prompt_utf8,
                                        std::int64_t max_new_tokens, SynGenAiControl *control, char *errbuf,
                                        std::size_t errbuf_len)
{
    if (!ctx || !prompt_utf8 || !control) {
        setError(errbuf, errbuf_len, "syn_genai_generate: null argument");
        return -1;
    }
    auto *holder = reinterpret_cast<PipelineHolder *>(ctx);
    if (!holder->pipeline) {
        setError(errbuf, errbuf_len, "syn_genai_generate: the pipeline is not initialised");
        return -1;
    }
    try {
        // max_new_tokens is the ONLY config field set: GenAI merges the model
        // dir's generation_config.json defaults (eos_token_id, sampling) for
        // the rest (resolve_generation_config), exactly like the smoke's
        // Python call generate("The capital of France is", max_new_tokens=N).
        ov::genai::GenerationConfig config;
        config.max_new_tokens = std::size_t(max_new_tokens > 0 ? max_new_tokens : 0);
        // Per-call state: the streamer must be heap-allocated (StreamerVariant
        // holds a shared_ptr<StreamerBase>; a stack object would be deleted
        // inside generate()). stopReason is reset so a natural EOS/bound end
        // (StopRunning) is distinguishable from a streamer stop.
        control->stopReason = StopRunning;
        std::int64_t callFirstNs = 0, callLastNs = 0, callTokens = 0;
        auto streamer = std::make_shared<SynStreamer>(control, holder->eosTokenId, &callFirstNs, &callLastNs,
                                                      &callTokens);
        // The DecodedResults text is discarded: the scored signal is the token
        // stream (the streamer) and the wall clock — the "Paris" text check is
        // the smoke/validation cross-check, not the engine's.
        holder->pipeline->generate(std::string(prompt_utf8), config, streamer);
        // Accumulate this call into the window control. decodeTokens mirrors
        // decodeNs: each call's FIRST token is the prefill's output (it got no
        // decode time), so only callTokens−1 tokens belong to the decode span —
        // subtracting one per CALL, not per window (AP-5 round 2, finding 3).
        control->tokens += callTokens;
        control->decodeTokens += (callTokens > 1 ? callTokens - 1 : 0);
        if (callFirstNs > 0) {
            if (control->firstTokenNs == 0)
                control->firstTokenNs = callFirstNs;
            if (callLastNs >= callFirstNs)
                control->decodeNs += callLastNs - callFirstNs;
        }
        // §5.3 floor (EOS-on-step-1 fails closed): a call that ended NATURALLY
        // (stopReason still StopRunning — GenAI hit EOS or the bound, not the
        // window/Stop) with fewer than the floor's tokens is a garbage
        // generation; the engine stops looping and reports an Error.
        if (control->stopReason == StopRunning && control->minNewTokens > 0
            && callTokens < control->minNewTokens)
            control->floorBroken = 1;
        return 0;
    } catch (const std::exception &e) {
        setError(errbuf, errbuf_len, "generation failed: ");
        if (errbuf && errbuf_len > 0)
            std::snprintf(errbuf + std::char_traits<char>::length(errbuf), errbuf_len - std::char_traits<char>::length(errbuf),
                          "%s", e.what());
        return -2;
    } catch (...) {
        setError(errbuf, errbuf_len, "generation failed (unknown error)");
        return -3;
    }
}

SYN_GENAI_EXPORT int syn_genai_runtime_version(char *buf, std::size_t buf_len)
{
    try {
        const ov::Version version = ov::genai::get_version();
        if (version.buildNumber) {
            if (buf && buf_len > 0)
                std::snprintf(buf, buf_len, "%s", version.buildNumber);
            return 0;
        }
    } catch (...) {
        // fall through: unknown version
    }
    if (buf && buf_len > 0)
        buf[0] = '\0';
    return -1;
}

SYN_GENAI_EXPORT void syn_genai_pipeline_delete(SynGenAiContext *ctx)
{
    delete reinterpret_cast<PipelineHolder *>(ctx);
}

} // extern "C"
