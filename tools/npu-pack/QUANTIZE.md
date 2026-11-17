# Producing the Intel model files

The `build.py`/`verify.py` tools take the model files as given; this note is the
exact procedure for producing them. The **vision pair is tested and run on
Knowhere** (see the timings below). The **LLM section is still untested** — it
is a multi-GB, multi-hour job that Dave has not approved yet, so it stays a
recipe, not a done thing.

## Toolchain (installed 2026-11-16, Knowhere)

A venv under the build-cache area, CPU-only torch to avoid a ~2–3 GB CUDA
bundle nobody here uses:

```
python3 -m venv ~/.cache/syntheon/npu-venv
~/.cache/syntheon/npu-venv/bin/pip install --upgrade pip
# CPU-only torch + torchvision from PyTorch's own index (no CUDA):
~/.cache/syntheon/npu-venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu torch torchvision
~/.cache/syntheon/npu-venv/bin/pip install openvino nncf ultralytics onnxscript
```

`onnxscript` is needed by torch 2.15's default (Dynamo) ONNX exporter. Versions:
openvino 2026.4.1, nncf 3.4.0, torch 2.15.0+cpu, torchvision 0.29.2+cpu,
ultralytics 8.4.215, onnxscript 0.7.2. `torch.cuda.is_available()` is `False`.

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

## LLM, ~1 B (llm, INT4) — the heavy step, untested ⚠️

NNCF 4-bit weight compression of TinyLlama-1.1B, saved as OpenVINO IR:

```
python3 - <<'PY'
import openvino as ov
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer
from nncf import compress_weights, CompressWeightsMode

model_id = "TinyLlama/TinyLlama-1.1B-Chat-v1.0"
tokenizer = AutoTokenizer.from_pretrained(model_id)
model = AutoModelForCausalLM.from_pretrained(model_id, torch_dtype=torch.float16)

compressed = compress_weights(model, mode=CompressWeightsMode.INT4_ASYM, group_size=128)
ov_model = ov.convert_model(compressed, example_input=torch.ones((1, 32), dtype=torch.long))
ov.save_model(ov_model, "llm-1b-int4/openvino_model.xml", compress_to_fp16=False)
tokenizer.save_pretrained("llm-1b-int4")
PY
```

Move the emitted files into `~/.cache/syntheon/npu-pack/src/intel/llm/llm-1b-int4/`
and make the `files` list in `config.json` match whatever `save_pretrained`
actually wrote (`tokenizer.json`, `tokenizer_config.json`, and possibly
`special_tokens_map.json` / `generation_config.json`). `transformers` is not yet
in the venv — add it (`pip install transformers`) before running this.

Two things to confirm with the engine side (tasks 8/11) before freezing v1:

- **NPU-runnability.** Whether a plain NNCF-INT4 transformer IR loads on the
  OpenVINO *NPU* device (vs CPU) depends on the model structure; the design's
  D9 points at an "OpenVINO GenAI-NPU" model for Intel. If the NPU plugin needs
  the GenAI/optimum-intel export instead of raw NNCF, that export is the source
  of these files — the pack layout is unchanged.
- **Exact tokenizer file set.** The workload needs whatever the engine reads to
  tokenise the fixed prompt; mirror that in `config.json`.

## After producing them

```
python3 tools/npu-pack/build.py --partial    # vision-only until the LLM lands
python3 tools/npu-pack/verify.py
```

`build.py` fails fast with a "missing source file" list until every declared
model's files exist; `--partial` skips the absent models (the LLM) and publishes
the rest. `build.py`'s failure list is the live to-do list for this note.
