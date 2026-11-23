# npu-pack

The NPU test's model-pack tooling (Phase 4c design §6.1, D5/D6). This is where
the benchmark's NPU models are turned into the versioned, SHA-256-pinned tree
that the test downloads on first use — and where that tree is checked before it
goes out.

There are two scripts and they have one job each:

- **`build.py`** — reads the model files you put in a source tree, hashes each
  one, copies them into the exact layout the downloader expects, and writes the
  `manifest.json` (plus its `manifest.sha256` pin). Run it whenever a model
  changes. It fails fast if a declared model's files are missing; pass
  `--partial` to skip the absent models and publish only what's fully present
  (handy for publishing an intermediate subset mid-build).
- **`verify.py`** — the host-side "is this pack correct" check. It re-reads the
  manifest, re-hashes every served file against it, and checks the pin. This is
  what you run on the pack server before and after publishing, to be sure the
  tree on disk is exactly what the manifest claims.

A model is only ever published whole: if even one of its files is missing,
`--partial` skips the whole model, never half-lists it.

The model files themselves are big and live *outside* the repo (never commit
them — `P-PATH`), under `~/.cache/syntheon/npu-pack/`. Only the two scripts,
`manifest_common.py` (shared helpers), `config.json` and this README are in
the tree.

## The layout

`build.py` turns a flat source tree into a served tree:

```
~/.cache/syntheon/npu-pack/
  src/                          what you download or quantise (your working files)
    intel/classify/resnet50-int8/resnet50.xml
    intel/classify/resnet50-int8/resnet50.bin
    intel/detect/yolov8n-int8/yolov8n.xml
    ...
  served/                       the published tree — copy this to the pack server
    npu-models/
      manifest.json
      manifest.sha256
      1/                         pack_version
        intel/classify/resnet50-int8/1/resnet50.xml
        ...
```

The `1/` inside each model path is the model's own version (the `version` field
in `config.json`); the top-level `1/` is the `pack_version`. The app fetches
`<base_url>/npu-models/manifest.json` and then
`<base_url>/npu-models/<pack_version>/<path>`, so the server's static directory
just points at the `served/` folder.

`manifest.json` matches the §6.1 schema exactly — `schema`, `pack_version`, and
one entry per model with `id`/`vendor`/`workload`/`precision`/`files`, each file
carrying `path`, `size` and a 64-char lowercase `sha256`. The parser in
`src/tests/npu/models/manifest.cpp` is the contract; `build.py` validates its own
output with the same rules before writing it, and `verify.py` does the same on
read.

## The model set (`config.json`)

For the Intel v1 pack the config lists three models, per the design's D5/D9:

| id | workload | precision | files |
|---|---|---|---|
| `resnet50-int8` | classify | INT8 | `resnet50.xml`, `resnet50.bin` |
| `yolov8n-int8` | detect | INT8 | `yolov8n.xml`, `yolov8n.bin` |
| `llm-1b-int4` | llm | INT4 | `openvino_model.xml`/`.bin`, `openvino_tokenizer.xml`/`.bin`, `openvino_detokenizer.xml`/`.bin`, `openvino_config.json`, `config.json`, `generation_config.json`, `tokenizer.json`, `tokenizer_config.json`, `chat_template.jinja` |

AMD and Qualcomm packs are separate configs, added later; the tooling already
accepts `amd` and `qualcomm` vendors and all three workloads.

## Where the model files come from

The design says the files are "pulled from the vendor's published NPU models"
or "compiled/quantised ahead of time by us". In practice there is no
vendor-published INT8 IR to just download for these exact models, so they are
produced with the OpenVINO + NNCF toolkit — the exact (and now-tested) commands
are in [`QUANTIZE.md`](QUANTIZE.md).

**Status: the full Intel pack (pack_version "2") is built and live on the pack
server.** ResNet-50 INT8 (25.7 MB), YOLOv8n INT8 (3.3 MB) and the ~1 B LLM INT4
*stateful* (640 MB) are quantised. Vision loads + runs on the NPU; the LLM
decoder is a stateful IR (44 `ReadValue`/`Assign` KV pairs, symmetric INT4).
Total ~669 MB. The 3-model `manifest.json` and the calibration pin `49c667e5…`
are recorded in §"The calibration SHA-pin" below.

The engine loads the stateful LLM through the OpenVINO GenAI C API
(GenAiCApi in `src/tests/npu/engine/genai_c_api.h`), pinned to the NPU
(`NPUW_DEVICES=NPU`, `NPUW_FALLBACK_EXEC=false`) — the compile-first smoke
rejected the raw-C decode loop for this IR, so GenAI is the shipped path. That
decision doesn't change the pack.

## Adding or changing a model

1. Drop the files in `~/.cache/syntheon/npu-pack/src/<vendor>/<workload>/<id>/`.
2. Add or edit the matching entry in `config.json` (id, vendor, workload,
   precision, version, and the list of bare filenames).
3. `python3 tools/npu-pack/build.py`, then `python3 tools/npu-pack/verify.py`.

The scripts take `--source-root`, `--output-root`, `--config`, `--pack-version`
and `--partial` if you need to point them somewhere else. Re-running is cheap:
unchanged files aren't re-hashed or re-copied. With some models still being
produced, `--partial` lets you publish a working subset without editing the
config.

## Bumping pack_version

`pack_version` ties the score to the model set (§20 N5): the anchors are
calibrated against a specific pack. So:

- Any change to any model file → `--pack-version N+1` → re-calibrate the score.
- `pack_version` is a single safe path component (no `/`, no `..`); the parser
  enforces the same rule.

## How the pack is served

The pack is the download host (a NAS appliance). The `odin/syntheon-npu`
dataset is served read-only over HTTPS by a raw k3s nginx on hostNetwork port
`8443` with a self-signed CA, so the base URL is that host's HTTPS address.
Copy the contents of `served/` into that dataset, and the Settings value
`npuModelsBaseUrl` points at it. Run `verify.py --output-root <served>` on the
host after the copy to confirm the tree arrived intact.

## The calibration SHA-pin

At calibration the manifest is frozen, and its SHA-256 is compiled into the app
so a tampered index can't redirect a verified download (§6.1). `build.py`
writes that digest to `npu-models/manifest.sha256`; regenerate it any time by
re-running `build.py` (or read it back with `verify.py`).

The current full-pack (3-model, Intel, pack_version "2") pin is:

    49c667e586648da8e80f161e9173c4494f27452eb607221834a951a50e8bf6dd

This is the **frozen, calibrated** pin (task 19, formulaVersion 1): the app's
`calibratedManifestSha256()` in `src/tests/npu/models/manifest.cpp` returns it,
and the downloader fails closed on anything else. Re-exporting any model bumps
`pack_version` and re-emits a fresh manifest + pin, which requires a fresh
calibration before the new pin can be frozen.
