# Producing the Intel model files

The `build.py`/`verify.py` tools take the model files as given; this note is the
exact procedure for producing them. **All three models are now produced and
tested on Knowhere** (timings below): ResNet-50 INT8, YOLOv8n INT8, and the
~1 B LLM INT4 (stateful, symmetric). The full three-model pack is live on
Mjolnir at `pack_version "2"`.

## Toolchain (installed 2026-11-16, Knowhere)

A venv under the build-cache area, CPU-only torch to avoid a ~2–3 GB CUDA
bundle nobody here uses:

```
python3 -m venv ~/.cache/syntheon/npu-venv
~/.cache/syntheon/npu-venv/bin/pip install --upgrade pip
# CPU-only torch + torchvision from PyTorch's own index (no CUDA):
~/.cache/syntheon/npu-venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch torchvision
~/.cache/syntheon/npu-venv/bin/pip install openvino nncf ultralytics onnxscript
~/.cache/syntheon/npu-venv/bin/pip install transformers          # for the LLM
~/.cache/syntheon/npu-venv/bin/pip install optimum-intel         # the LLM exporter (optimum-cli)
```

`onnxscript` is needed by torch 2.15's default (Dynamo) ONNX exporter. Versions
(recorded because the optimum-intel emitted file set is version-sensitive, §4.1):

| package | version |
|---|---|
| openvino | 2026.4.1 |
| nncf | 3.4.0 |
| optimum-intel | 2.2.0 |
| optimum | 2.3.0 |
| openvino-tokenizers | 2026.4.1.0 |
| transformers | 5.5.4 (optimum-intel pins this below 5.6) |
| tokenizers | 0.22.2 |
| torch | 2.15.0+cpu |

`torch.cuda.is_available()` is `False`.

**Gotcha:** importing ultralytics writes `~/.config/Ultralytics/settings.json`
with `datasets_dir` pointing at the *current* directory — if that's the repo
root, it drops `datasets/` into the synced tree. Set the dirs out of the repo
before any export:

```
~/.cache/syntheon/npu-venv/bin/yolo settings \
    datasets_dir=~/.cache/syntheon/npu-pack/datasets \
    weights_dir=~/.cache/syntheon/npu-pack/weights \
    runs_dir=~/.cache/syntheon/npu-pack/runs
```

## YOLOv8n (detect, INT8) — tested ✅

Ultralytics does the INT8 quantise itself (a coco8 calibration set it downloads
on demand):

```
cd ~/.cache/syntheon/npu-pack/work
~/.cache/syntheon/npu-venv/bin/yolo export model=yolov8n.pt format=openvino int8=True
# -> yolov8n_int8_openvino_model/yolov8n.xml (455,609 B) + yolov8n.bin (3,293,388 B)
```

5.6 s export, 3.6 MB total. The IR carries 66 `FakeQuantize` layers and 63 `i8`
weight constants (genuinely INT8). Copy the two files into
`~/.cache/syntheon/npu-pack/src/intel/detect/yolov8n-int8/`.

## ResNet-50 (classify, INT8) — tested ✅

`~/.cache/syntheon/npu-pack/work/export_resnet50.py` (reproduced below):
torchvision → ONNX → OpenVINO → NNCF PTQ. Calibration uses the coco128 images
(128 real photos, ~7 MB from the ultralytics GitHub release); for a
throughput-only benchmark the quantizer ranges just need sane activation
statistics, not ImageNet-grade calibration. Run it from `work/`:

```
cd ~/.cache/syntheon/npu-pack/work
~/.cache/syntheon/npu-venv/bin/python export_resnet50.py
# -> resnet50-int8/resnet50.xml (359,237 B) + resnet50.bin (25,723,796 B)
```

```python
#!/usr/bin/env python3
# ResNet-50 INT8 OpenVINO IR: torchvision -> ONNX -> OpenVINO -> NNCF PTQ (coco128 calibration)
import glob, os
import torch, torchvision
from PIL import Image
from torchvision import transforms
import nncf, openvino as ov

WORK = os.path.dirname(os.path.abspath(__file__))
ONNX = os.path.join(WORK, "resnet50.onnx")
INT8 = os.path.join(WORK, "resnet50-int8")
CALIB = os.path.join(WORK, "coco128", "coco128", "images", "train2017")

# 1. torchvision ResNet-50 -> ONNX (IMAGENET1K_V1 weights land in torch's cache)
m = torchvision.models.resnet50(weights=torchvision.models.ResNet50_Weights.IMAGENET1K_V1).eval()
torch.onnx.export(m, torch.randn(1, 3, 224, 224), ONNX,
                  input_names=["input"], output_names=["output"], opset_version=13)

# 2. ONNX -> OpenVINO IR
ov_model = ov.convert_model(ONNX)
input_name = ov_model.input(0).get_any_name()

# 3. NNCF INT8 PTQ against a small calibration subset
transform = transforms.Compose([
    transforms.Resize(256), transforms.CenterCrop(224), transforms.ToTensor(),
    transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])
paths = sorted(glob.glob(os.path.join(CALIB, "*.jpg")))

def gen():
    for p in paths:
        img = Image.open(p).convert("RGB")
        yield {input_name: transform(img).unsqueeze(0).numpy()}

quantized = nncf.quantize(ov_model, nncf.Dataset(gen()))
os.makedirs(INT8, exist_ok=True)
ov.save_model(quantized, os.path.join(INT8, "resnet50.xml"), compress_to_fp16=False)
```

~13 s end to end. 71 `FakeQuantize` layers, 54 `i8` constants. `torch.onnx.export`
prints a non-fatal `version_converter` warning while it falls back to re-export
at opset 13 — harmless. Copy `resnet50.xml` + `resnet50.bin` into
`~/.cache/syntheon/npu-pack/src/intel/classify/resnet50-int8/`.

## Both models smoke-tested ✅

`~/.cache/syntheon/npu-pack/work/smoke_test.py` loads each IR and runs one
forward pass on CPU: ResNet-50 → logits `[1,1000]` (argmax in range),
YOLOv8n → head `[1,84,8400]`, both finite. The NPU-device run is the engine's
job (tasks 8/11) and happens on Nivedellir, not here.

## LLM, ~1 B (llm, INT4, stateful, symmetric) — tested ✅

The LLM is now a **stateful** OpenVINO IR (KV cache held as internal model
state — 44 `ReadValue` + 44 `Assign`, 22 layers × K/V), exported with
optimum-intel and **symmetric** INT4 (the NPU-native layout). This is the D9
"GenAI/stateful" path. The earlier stateless wrap-to-logits recipe is
**superseded** — do not reuse it.

Export (DD-1 §4.1):

```
~/.cache/syntheon/npu-venv/bin/optimum-cli export openvino \
    --model TinyLlama/TinyLlama-1.1B-Chat-v1.0 \
    --weight-format int4 --sym --group-size 128 --ratio 1.0 \
    --trust-remote-code \
    ~/.cache/syntheon/npu-pack/src/intel/llm/llm-1b-int4/
```

~19 s with the weights cached. `--sym` flips the quant to symmetric (the NNCF
bitwidth table reports `int4_sym, group size 128`, 88% of params). The emitted
file set is **version-sensitive** (optimum-intel 2.2.0 + transformers 5.5.4) —
always `ls` the dir and mirror it rather than trusting a list:

```
openvino_model.xml           1,839,103   # stateful decoder (input_ids, attention_mask, position_ids, beam_idx -> logits)
openvino_model.bin         631,214,037   # decoder weights (symmetric INT4, the bulk)
openvino_tokenizer.xml          27,831   # tokenizer model (CPU prefill tokenisation)
openvino_tokenizer.bin       1,869,017
openvino_detokenizer.xml       11,752   # detokenizer model
openvino_detokenizer.bin      466,938
openvino_config.json              467   # the GenAI pipeline config
config.json                       725
generation_config.json            123
tokenizer.json               3,618,870
tokenizer_config.json             369
chat_template.jinja               410
```

This diverges from DD §4.2's *predicted* list: there is **no
`openvino_embed_tokens.*`** (the embed is folded into the decoder/tokenizer) and
**no `special_tokens_map.json`** (transformers 5.5.4 folds it into the tokenizer
files); instead there are `openvino_tokenizer.*`, `openvino_detokenizer.*` and
`openvino_config.json`. `config.json`'s `llm-1b-int4` `files` list is this exact
12-name set — `build.py` ships only listed names and silently drops the rest.

Two things remain with the engine side (tasks 8/11):

- **NPU-runnability.** This is the GenAI-consumable stateful IR (DD-1 F2);
  whether the raw-C loop (Option B) or `LLMPipeline` (Option A) drives it on the
  NPU is settled by the compile-first smoke on Nivedellir (§6), not here.
- **Fixed prompt.** The scored decode loop pre-ships the fixed prompt's
  `input_ids` (DD-1 §5.2.5), so the tokenizer/detokenizer models are present for
  the correctness spot-check, not the timed loop.

## After producing them

```
python3 tools/npu-pack/build.py --pack-version 2   # full pack (no --partial)
python3 tools/npu-pack/verify.py
```

`--partial` remains for publishing an intermediate subset while a model is still
being made; once every declared model's files exist, the plain build publishes
the whole pack. `pack_version` bumps on any model change (N5); the current pack
is "2".
