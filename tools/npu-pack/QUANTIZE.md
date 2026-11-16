# Producing the Intel model files

The `build.py`/`verify.py` tools take the model files as given; this note is the
exact procedure for producing them. **These commands have not been run on this
box** (no OpenVINO/NNCF/torch installed, and the LLM pass is a multi-GB,
multi-hour job), so treat them as a starting point that must be executed and
verified before the v1 pack is frozen. They are written from the OpenVINO and
NNCF docs and use versions confirmed to have `cp314` wheels (openvino 2026.4.1,
nncf 3.4.0, torch 2.15.0).

One-time install (to the user site, not system-wide):

```
python3 -m pip install --user openvino nncf torch torchvision ultralytics
```

Then, per model:

## ResNet-50 (classify, INT8)

```
# 1. Export torchvision ResNet-50 to ONNX
python3 - <<'PY'
import torch, torchvision
m = torchvision.models.resnet50(weights=torchvision.models.ResNet50_Weights.IMAGENET1K_V1).eval()
torch.onnx.export(m, torch.randn(1, 3, 224, 224), "resnet50.onnx",
                  input_names=["input"], output_names=["output"], opset_version=13)
PY

# 2. ONNX -> OpenVINO IR
python3 -m ovc resnet50.onnx --output_dir resnet50-fp32

# 3. NNCF INT8 PTQ (needs a small calibration subset)
python3 - <<'PY'
import nncf, openvino as ov
from torchvision import datasets, transforms
from torch.utils.data import DataLoader, Subset

core = ov.Core()
model = core.read_model("resnet50-fp32/resnet50.xml")

transform = transforms.Compose([
    transforms.Resize(256), transforms.CenterCrop(224), transforms.ToTensor(),
    transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
])
dataset = datasets.ImageFolder("/path/to/imagenet/val", transform=transform)
calib = Subset(dataset, range(300))                 # a few hundred images is enough
loader = DataLoader(calib, batch_size=1, shuffle=False)
calibration_dataset = nncf.Dataset(loader, lambda item: item[0].numpy())

quantized = nncf.quantize(model, calibration_dataset)
ov.save_model(quantized, "resnet50-int8/resnet50.xml", compress_to_fp16=False)
PY
```

Move `resnet50-int8/resnet50.xml` and `.bin` into
`~/.cache/syntheon/npu-pack/src/intel/classify/resnet50-int8/`.

## YOLOv8n (detect, INT8)

Ultralytics does the INT8 quantise itself (COCO128 calibration), which is the
closest thing to a vendor-published INT8 export:

```
yolo export model=yolov8n.pt format=openvino int8=True
# -> yolov8n_openvino_model/yolov8n.xml + yolov8n.bin
```

Move those two into `~/.cache/syntheon/npu-pack/src/intel/detect/yolov8n-int8/`.

## LLM, ~1 B (llm, INT4) — the heavy step

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
`special_tokens_map.json` / `generation_config.json`).

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
python3 tools/npu-pack/build.py
python3 tools/npu-pack/verify.py
```

`build.py` failing with a "missing source file" list is the live to-do list for
this note; it goes green as each model lands.
