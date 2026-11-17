# Producing the Intel model files

The `build.py`/`verify.py` tools take the model files as given; this note is the
exact procedure for producing them. **All three models are now produced and
tested on Knowhere** (timings below): ResNet-50 INT8, YOLOv8n INT8, and the
~1 B LLM INT4. The full three-model pack is live on Mjolnir.

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
```

`onnxscript` is needed by torch 2.15's default (Dynamo) ONNX exporter. Versions:
openvino 2026.4.1, nncf 3.4.0, torch 2.15.0+cpu, torchvision 0.29.2+cpu,
ultralytics 8.4.215, onnxscript 0.7.2, transformers 5.23.0.
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

## LLM, ~1 B (llm, INT4) — tested ✅

NNCF 4-bit weight compression of TinyLlama-1.1B, saved as OpenVINO IR. Two
things the original recipe got wrong, now corrected and run for real:

- `compress_weights` on a **torch** model wants a tracing dataset, and
  `ov.convert_model(torch)`'s TorchScript tracer can't capture a transformers
  5.x causal LM at all (returns a `DynamicCache`, then chokes on `aten::diff`).
  The working route is: **wrap the model to return only logits** (`use_cache`
  off), export to **ONNX via the Dynamo exporter**, convert that, and run
  `compress_weights` on the OV graph (weight-only compression needs no
  calibration set there).

`~/.cache/syntheon/npu-pack/work/export_llm.py` (run from `work/`):

```python
#!/usr/bin/env python3
# TinyLlama-1.1B -> ONNX (Dynamo) -> OpenVINO IR -> NNCF INT4
import gc, os
import torch, openvino as ov
from nncf import compress_weights, CompressWeightsMode
from transformers import AutoModelForCausalLM, AutoTokenizer

WORK = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(WORK, "llm-1b-int4")
ONNX = os.path.join(WORK, "llm-fp16.onnx")
MODEL_ID = "TinyLlama/TinyLlama-1.1B-Chat-v1.0"

class LogitsWrapper(torch.nn.Module):
    def __init__(self, model):
        super().__init__(); self.model = model; self.model.config.use_cache = False
    def forward(self, input_ids):
        return self.model(input_ids=input_ids, use_cache=False).logits

tokenizer = AutoTokenizer.from_pretrained(MODEL_ID)
model = AutoModelForCausalLM.from_pretrained(MODEL_ID, torch_dtype=torch.float16).eval()

os.makedirs(OUT, exist_ok=True)
tokenizer.save_pretrained(OUT); model.config.save_pretrained(OUT)
model.generation_config.save_pretrained(OUT)

with torch.no_grad():
    torch.onnx.export(LogitsWrapper(model), torch.ones((1, 32), dtype=torch.long), ONNX,
                      input_names=["input_ids"], output_names=["logits"],
                      opset_version=13, dynamo=True)
del model  # free the torch graph before the ONNX -> OV step
gc.collect()

ov_model = ov.convert_model(ONNX)
compressed = compress_weights(ov_model, mode=CompressWeightsMode.INT4_ASYM, group_size=128)
ov.save_model(compressed, os.path.join(OUT, "openvino_model.xml"), compress_to_fp16=False)
```

Timings (weights cached after first load): download ~38 s (first time only),
ONNX export 22 s, ONNX→OV 2.5 s, INT4 compression 5 s — **~32 s** end to end
excluding the download. Output (~640 MB):

```
openvino_model.bin   634,824,570   # the INT4 weights (~1.1 B params, the bulk)
openvino_model.xml     1,683,438
tokenizer.json         3,618,870
tokenizer_config.json        398
config.json                  725
generation_config.json       124
chat_template.jinja          410
```

Copy all of those into `~/.cache/syntheon/npu-pack/src/intel/llm/llm-1b-int4/`
(the `files` list in `config.json` matches this exact set). Smoke-tested on CPU:
`smoke_llm.py` runs "The capital of France is" and the INT4 IR's top token is
`Paris`, identical to the fp16 torch model (argmax preserved; only the
low-probability tail reorders, which is normal for INT4).

Three things to confirm with the engine side (tasks 8/11) before freezing v1:

- **NPU-runnability.** Whether a plain NNCF-INT4 transformer IR loads on the
  OpenVINO *NPU* device (vs CPU) depends on the model structure; the design's
  D9 points at an "OpenVINO GenAI-NPU" model for Intel. If the NPU plugin needs
  the GenAI/optimum-intel export instead of raw NNCF, that export is the source
  of these files — the pack layout is unchanged.
- **Static sequence length.** The IR was exported with a fixed `[1,32]` input
  (the recipe's `example_input`), so this plain IR is a one-shot 32-token
  forward, not an auto-regressive generator. A stateful/KV-cache export is the
  likely GenAI-path requirement; flag it to the engine.
- **Exact tokenizer file set.** The workload needs whatever the engine reads to
  tokenise the fixed prompt; mirror that in `config.json`.

## After producing them

```
python3 tools/npu-pack/build.py     # full pack (no --partial — all three models)
python3 tools/npu-pack/verify.py
```

`--partial` remains for publishing an intermediate subset while a model is still
being made; once every declared model's files exist, the plain build publishes
the whole pack.
