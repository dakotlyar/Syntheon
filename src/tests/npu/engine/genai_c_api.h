// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The resolve-by-name surface over OpenVINO GenAI's own C ABI (DD-1 §5.4
// Option A, C-API rewrite 2026-11-22): openvino_genai_c.{dll,so} is a thin C
// wrapper over the C++ openvino_genai runtime, and the engine dlopens it
// directly — no C++ shim module, no GenAI headers/import-lib as a build input,
// no find_package, nothing bundled (P-VENDOR restored). The engine compiles as
// plain C++ and reaches openvino_genai_c only at run time, mirroring
// OpenVinoApi + resolveApi in engine_openvino.cpp exactly.
//
// The opaque handles and the streamer struct/status are our own declarations
// of the runtime's C ABI (the real definitions live in the vendor headers);
// every signature below is verbatim from the 2026.4.1 SDK C headers
// (…/genai/c/{llm_pipeline,generation_config,perf_metrics}.h), verified at
// the C-API rewrite. Nothing here throws; every resolved symbol is
// null-checked, so a missing/damaged openvino_genai_c is a clean "LLM runtime
// missing" error on the llm workload only — never a launch failure.
//
// Header-only (inline): the resolver can't be unit-tested without the DLL on
// CI, so a separate .cpp would carry only tidiness; kept beside
// Library/resolve in engine_dlopen.h, which this header includes for the
// Library type.

#include "tests/npu/engine/engine_dlopen.h"

#include <QString>

#include <cstddef>

namespace sb::npu::engine_internal {

// Opaque handles (the real defs live in the runtime).
struct ov_genai_llm_pipeline;
struct ov_genai_generation_config;
struct ov_genai_decoded_results;
struct ov_genai_perf_metrics;

// ov_genai_streaming_status_e (…/c/llm_pipeline.h) — the exact integer
// meanings, as returned by the streamer callback.
enum class GenAiStreamStatus : int { Running = 0, Stop = 1, Cancel = 2 };

// streamer_callback — a C struct of a callback fnptr + user arg; __cdecl
// (Windows) / default (Linux), ABI-irrelevant on the project's x64/ARM64
// targets.
using GenAiStreamerFn = GenAiStreamStatus (*)(const char *str, void *args);
struct GenAiStreamer {
    GenAiStreamerFn callback_func;
    void *args;
};

// ov_status_e == int, OK == 0. All 13 members are resolved by name from
// openvino_genai_c; each is null-checked at resolve time.
struct GenAiCApi {
    // C variadic: the trailing ... is part of the type and must not be dropped
    // (calling through a non-variadic fnptr is UB) — the NPUW property pairs
    // ride through it.
    int (*pipelineCreate)(const char *models_path, const char *device, std::size_t property_args_size,
                          ov_genai_llm_pipeline **pipe, ...) = nullptr;
    void (*pipelineFree)(ov_genai_llm_pipeline *pipe) = nullptr;
    int (*pipelineGenerate)(ov_genai_llm_pipeline *pipe, const char *inputs,
                            const ov_genai_generation_config *config,
                            const GenAiStreamer *streamer, ov_genai_decoded_results **results) = nullptr;
    int (*configCreate)(ov_genai_generation_config **config) = nullptr;
    int (*configSetMaxNewTokens)(ov_genai_generation_config *config, std::size_t value) = nullptr;
    void (*configFree)(ov_genai_generation_config *config) = nullptr;
    void (*resultsFree)(ov_genai_decoded_results *results) = nullptr;
    int (*resultsGetPerfMetrics)(const ov_genai_decoded_results *results, ov_genai_perf_metrics **metrics) = nullptr;
    void (*perfMetricsFree)(ov_genai_perf_metrics *metrics) = nullptr;
    int (*perfNumGenerationTokens)(const ov_genai_perf_metrics *metrics, std::size_t *tokens) = nullptr;
    int (*perfGenerateDuration)(const ov_genai_perf_metrics *metrics, float *mean, float *std) = nullptr;
    int (*perfTtft)(const ov_genai_perf_metrics *metrics, float *mean, float *std) = nullptr;
    int (*perfAddInPlace)(ov_genai_perf_metrics *left, const ov_genai_perf_metrics *right) = nullptr;
};

// Resolves all 13 by name from the loaded C DLL, short-circuiting on the FIRST
// missing symbol so *error always names the first failure (mirrors resolveApi
// in engine_openvino.cpp). Returns false (and sets *error) on any miss.
inline bool resolveGenAiCApi(const Library &capi, GenAiCApi *api, QString *error)
{
    api->pipelineCreate = resolve<decltype(api->pipelineCreate)>(capi, "ov_genai_llm_pipeline_create", error);
    if (!api->pipelineCreate)
        return false;
    api->pipelineFree = resolve<decltype(api->pipelineFree)>(capi, "ov_genai_llm_pipeline_free", error);
    if (!api->pipelineFree)
        return false;
    api->pipelineGenerate = resolve<decltype(api->pipelineGenerate)>(capi, "ov_genai_llm_pipeline_generate", error);
    if (!api->pipelineGenerate)
        return false;
    api->configCreate = resolve<decltype(api->configCreate)>(capi, "ov_genai_generation_config_create", error);
    if (!api->configCreate)
        return false;
    api->configSetMaxNewTokens =
        resolve<decltype(api->configSetMaxNewTokens)>(capi, "ov_genai_generation_config_set_max_new_tokens", error);
    if (!api->configSetMaxNewTokens)
        return false;
    api->configFree = resolve<decltype(api->configFree)>(capi, "ov_genai_generation_config_free", error);
    if (!api->configFree)
        return false;
    api->resultsFree = resolve<decltype(api->resultsFree)>(capi, "ov_genai_decoded_results_free", error);
    if (!api->resultsFree)
        return false;
    api->resultsGetPerfMetrics =
        resolve<decltype(api->resultsGetPerfMetrics)>(capi, "ov_genai_decoded_results_get_perf_metrics", error);
    if (!api->resultsGetPerfMetrics)
        return false;
    api->perfMetricsFree =
        resolve<decltype(api->perfMetricsFree)>(capi, "ov_genai_decoded_results_perf_metrics_free", error);
    if (!api->perfMetricsFree)
        return false;
    api->perfNumGenerationTokens =
        resolve<decltype(api->perfNumGenerationTokens)>(capi, "ov_genai_perf_metrics_get_num_generation_tokens", error);
    if (!api->perfNumGenerationTokens)
        return false;
    api->perfGenerateDuration =
        resolve<decltype(api->perfGenerateDuration)>(capi, "ov_genai_perf_metrics_get_generate_duration", error);
    if (!api->perfGenerateDuration)
        return false;
    api->perfTtft = resolve<decltype(api->perfTtft)>(capi, "ov_genai_perf_metrics_get_ttft", error);
    if (!api->perfTtft)
        return false;
    api->perfAddInPlace = resolve<decltype(api->perfAddInPlace)>(capi, "ov_genai_perf_metrics_add_in_place", error);
    if (!api->perfAddInPlace)
        return false;
    return true;
}

} // namespace sb::npu::engine_internal
