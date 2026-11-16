# npu-pack

The NPU test's model-pack tooling (Phase 4c design §6.1, D5/D6). This is where
the benchmark's NPU models are turned into the versioned, SHA-256-pinned tree
that the test downloads on first use — and where that tree is checked before it
goes out.

There are two scripts and they have one job each:

- **`build.py`** — reads the model files you put in a source tree, hashes each
  one, copies them into the exact layout the downloader expects, and writes the
  `manifest.json` (plus its `manifest.sha256` pin). Run it whenever a model
  changes.
- **`verify.py`** — the host-side "is this pack correct" check. It re-reads the
  manifest, re-hashes every served file against it, and checks the pin. This is
  what you run on Mjolnir before and after publishing, to be sure the tree on
  disk is exactly what the manifest claims.

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
  served/                       the published tree — Hera copies this to Mjolnir
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
`<base_url>/npu-models/<pack_version>/<path>`, so Hera's static directory just
points at the `served/` folder.

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
| `llm-1b-int4` | llm | INT4 | `openvino_model.xml`, `openvino_model.bin`, `tokenizer.json`, `tokenizer_config.json` |

AMD and Qualcomm packs are separate configs, added later; the tooling already
accepts `amd` and `qualcomm` vendors and all three workloads.

## Where the model files come from

The design says the files are "pulled from the vendor's published NPU models"
or "compiled/quantised ahead of time by us". In practice there is no
vendor-published INT8 IR to just download for these exact models, so they are
produced with the OpenVINO + NNCF toolkit — the exact commands are in
[`QUANTIZE.md`](QUANTIZE.md).

**Status: none of the three model files have been produced on this box yet.**
The vision models need a one-time toolchain install plus a quantise pass; the
LLM (the pack's bulk, ≤ ~1.5 GB) needs the same plus the weight download and a
longer run. The important part here is that `build.py` and `verify.py` are
tested end-to-end and will slot the real files in without change. Until they
land, `build.py` fails with a clear "missing source file" list, which doubles as
the to-do list.

## Adding or changing a model

1. Drop the files in `~/.cache/syntheon/npu-pack/src/<vendor>/<workload>/<id>/`.
2. Add or edit the matching entry in `config.json` (id, vendor, workload,
   precision, version, and the list of bare filenames).
3. `python3 tools/npu-pack/build.py`, then `python3 tools/npu-pack/verify.py`.

The scripts take `--source-root`, `--output-root`, `--config` and
`--pack-version` if you need to point them somewhere else. Re-running is cheap:
unchanged files aren't re-hashed or re-copied.

## Bumping pack_version

`pack_version` ties the score to the model set (§20 N5): the anchors are
calibrated against a specific pack. So:

- Any change to any model file → `--pack-version N+1` → re-calibrate the score.
- `pack_version` is a single safe path component (no `/`, no `..`); the parser
  enforces the same rule.

## How Mjolnir serves it

Mjolnir is the download host (a TrueNAS SCALE appliance). The plan is a
`odin/syntheon-npu` dataset served read-only over HTTPS by a small Caddy Docker
app on port 8443 with `tls internal` (self-signed), so the base URL is
`https://192.168.100.160:8443/`. Hera copies the contents of `served/` into that
dataset, and the Settings value `npuModelsBaseUrl` points at it. Run
`verify.py --output-root <served>` on Mjolnir after the copy to confirm the tree
arrived intact.

## The calibration SHA-pin

At calibration the manifest is frozen, and its SHA-256 is compiled into the app
so a tampered index can't redirect a verified download (§6.1, AP-5's
recommendation). `build.py` writes that digest to `npu-models/manifest.sha256`;
regenerate it any time by re-running `build.py` (or read it back with
`verify.py`). When the v1 pack is frozen, that digest is the one that gets
pinned in source.
