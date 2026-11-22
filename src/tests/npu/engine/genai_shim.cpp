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
// first-token time (ttft) and the EOS sighting, and stops the generation when
// the engine's 5 s window elapses or its Stop predicate fires — that is how a
// window is bounded without a thread (P-STOP ≤ 250 ms: tokens on the NPU land
// every ~30 ms).

#include "tests/npu/engine/genai_shim.h"

#include <openvino/genai/generation_config.hpp>
#include <openvino/genai/llm_pipeline.hpp>
#include <openvino/genai/streamer_base.hpp>

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

void setError(char *errbuf, std::size_t errbuf_len, const std::string &msg)
{
    if (errbuf && errbuf_len > 0)
        std::snprintf(errbuf, errbuf_len, "%s", msg.c_str());
}

// GenAI's C++ Tokenizer loads the openvino-tokenizers extension from the path
// in OPENVINO_TOKENIZERS_PATH_GENAI (tokenizers_relative_to_genai(): the
// tokenizers lib BESIDE the genai module). A pip install puts genai and
// tokenizers in sibling package dirs, so that default path is wrong and the
// pipeline would fail to load the tokenizer IRs. Point the env var at the
// actual tokenizers lib (only if the user didn't already set it — GenAI's
// ScopedVar would otherwise leave ours in place). On Windows the CRT-wide env
// (_wputenv_s) must be used: GenAI reads it with _wgetenv, which does not see
// SetEnvironmentVariableA/qputenv changes.
void ensureTokenizersPath(const std::string &tokenizers_path_utf8)
{
    if (tokenizers_path_utf8.empty())
        return;
#if defined(_WIN32)
    if (_wgetenv(L"OPENVINO_TOKENIZERS_PATH_GENAI") != nullptr)
        return;
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, tokenizers_path_utf8.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
        return;
    std::wstring wide(std::size_t(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, tokenizers_path_utf8.c_str(), -1, wide.data(), wideLen);
    _wputenv_s(L"OPENVINO_TOKENIZERS_PATH_GENAI", wide.c_str());
#else
    if (getenv("OPENVINO_TOKENIZERS_PATH_GENAI") != nullptr)
        return;
    setenv("OPENVINO_TOKENIZERS_PATH_GENAI", tokenizers_path_utf8.c_str(), 1);
#endif
}

// Counts the generated tokens, records the first-token wall time (ttft) and
// the EOS sighting, and stops the generation when the window elapses or the
// caller's Stop predicate fires. GenAI calls write() once per new token on the
// stateful batch-1 greedy path; the base write(vector) routes every token
// through this override. The EOS token itself may or may not reach the
// streamer (GenAI filters it internally in some paths) — eosSeen is
// informational only, never load-bearing (the §5.3 floor uses tokens ≥ 8).
class SynStreamer final : public ov::genai::StreamerBase
{
public:
    explicit SynStreamer(SynGenAiControl *control, std::int64_t eosTokenId)
        : m_control(control), m_eosTokenId(eosTokenId)
    {
    }
    ov::genai::StreamingStatus write(std::int64_t token) override
    {
        const std::int64_t nowNs = steadyNs();
        if (m_control->tokens == 0)
            m_control->firstTokenNs = nowNs;
        ++m_control->tokens;
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

SynGenAiContext *syn_genai_pipeline_create(const char *model_dir_utf8, const char *device,
                                           const char *tokenizers_path_utf8, std::int64_t eos_token_id,
                                           char *errbuf, std::size_t errbuf_len)
{
    if (!model_dir_utf8 || !device) {
        setError(errbuf, errbuf_len, "syn_genai_pipeline_create: null argument");
        return nullptr;
    }
    ensureTokenizersPath(tokenizers_path_utf8 ? tokenizers_path_utf8 : "");
    try {
        // SMOKE: the exact ctor form the Nivedellir smoke proved on this pack
        // and IR — LLMPipeline(model_dir, "NPU") with default properties. GenAI
        // reads the model dir's own generation_config.json + chat template,
        // tokenizes the prompt internally (CPU), staticizes the KV state and
        // compiles on device "NPU". The model dir comes in as UTF-8, converted
        // with u8path so a non-ASCII cache root never round-trips through the
        // Windows ANSI codepage (the engine's L1 rule).
        auto ctx = std::make_unique<PipelineHolder>();
        ctx->pipeline =
            std::make_unique<ov::genai::LLMPipeline>(std::filesystem::u8path(model_dir_utf8), std::string(device));
        ctx->eosTokenId = eos_token_id;
        return reinterpret_cast<SynGenAiContext *>(ctx.release());
    } catch (const std::exception &e) {
        setError(errbuf, errbuf_len,
                 std::string("failed to create the LLM pipeline on device '") + device + "': " + e.what());
    } catch (...) {
        setError(errbuf, errbuf_len, "failed to create the LLM pipeline on device '" + std::string(device)
                                         + "' (unknown error)");
    }
    return nullptr;
}

int syn_genai_generate(SynGenAiContext *ctx, const char *prompt_utf8, std::int64_t max_new_tokens,
                       SynGenAiControl *control, char *errbuf, std::size_t errbuf_len)
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
        // max_new_tokens is the ONLY field set: GenAI merges the model dir's
        // generation_config.json defaults (eos_token_id, sampling) for the rest
        // (resolve_generation_config), exactly like the smoke's Python call
        // generate("The capital of France is", max_new_tokens=N).
        ov::genai::GenerationConfig config;
        config.max_new_tokens = std::size_t(max_new_tokens > 0 ? max_new_tokens : 0);
        // The streamer must be heap-allocated: StreamerVariant holds a
        // shared_ptr<StreamerBase>, and a stack object would be deleted when the
        // variant's copy dies inside generate().
        auto streamer = std::make_shared<SynStreamer>(control, holder->eosTokenId);
        // The DecodedResults text is discarded: the scored signal is the token
        // stream (the streamer) and the wall clock — the "Paris" text check is
        // the smoke/validation cross-check, not the engine's.
        holder->pipeline->generate(std::string(prompt_utf8), config, streamer);
        return 0;
    } catch (const std::exception &e) {
        setError(errbuf, errbuf_len, std::string("generation failed: ") + e.what());
        return -2;
    } catch (...) {
        setError(errbuf, errbuf_len, "generation failed (unknown error)");
        return -3;
    }
}

void syn_genai_pipeline_delete(SynGenAiContext *ctx)
{
    delete reinterpret_cast<PipelineHolder *>(ctx);
}

} // extern "C"
