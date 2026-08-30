"""
NPU / AI inference benchmark.

Attempts to run a small MLP through ONNX Runtime and selects the best
available execution provider (NPU → GPU → CPU).

Scoring reference:
  - Dedicated NPU (e.g. Ryzen AI / Intel NPU) at ~50 TOPS ≈ 200 points
  - Discrete GPU via DirectML / CUDA ≈ 100–150 points
  - CPU inference ≈ 30–80 points

If onnxruntime is not installed the test reports as unavailable.
"""

import io
import time
from typing import Callable, List, Optional, Tuple

from . import BenchmarkResult

# ---------------------------------------------------------------------------
# Calibration
# ---------------------------------------------------------------------------
_INF_REF    = 5_000   # inferences/sec on reference GPU/NPU
_MAX_SCORE  = 200
_DURATION   = 5.0


# ---------------------------------------------------------------------------
# Minimal ONNX graph builder (no onnx package required)
# Builds: input(1,64) → MatMul(W1 64×256) → Relu → MatMul(W2 256×64) → output(1,64)
# ---------------------------------------------------------------------------

def _f32_tensor(name: str, dims: List[int], data: bytes) -> bytes:
    """Encode a float32 ONNX TensorProto."""

    # ONNX TensorProto fields (field numbers from onnx.proto3):
    #  1: dims (repeated int64, packed)
    #  2: data_type (INT32: 1=float)
    #  8: name (STRING)
    #  9: raw_data (BYTES)

    def _varint(v: int) -> bytes:
        out = []
        while v > 0x7F:
            out.append((v & 0x7F) | 0x80)
            v >>= 7
        out.append(v)
        return bytes(out)

    def _field(number: int, wire: int, payload: bytes) -> bytes:
        tag = _varint((number << 3) | wire)
        if wire == 2:
            return tag + _varint(len(payload)) + payload
        return tag + payload  # varint (wire 0): no length prefix

    dims_payload = b"".join(_varint(d) for d in dims)
    proto  = _field(1, 2, dims_payload)   # dims (packed varint)
    proto += _field(2, 0, _varint(1))     # data_type = FLOAT
    proto += _field(8, 2, name.encode())  # name
    proto += _field(9, 2, data)           # raw_data
    return proto


def _build_onnx_mlp() -> bytes:
    """Return a minimal ONNX model bytes for a tiny MLP."""
    import numpy as np

    rng = np.random.default_rng(42)
    W1 = rng.standard_normal((64, 256)).astype(np.float32)
    W2 = rng.standard_normal((256, 64)).astype(np.float32)

    def _varint(v: int) -> bytes:
        out = []
        while v > 0x7F:
            out.append((v & 0x7F) | 0x80)
            v >>= 7
        out.append(v)
        return bytes(out)

    def _field(number: int, wire: int, payload: bytes) -> bytes:
        tag = _varint((number << 3) | wire)
        if wire == 2:
            return tag + _varint(len(payload)) + payload
        return tag + payload  # varint (wire 0): no length prefix

    def _string(s: str) -> bytes:
        return s.encode()

    # ---- NodeProto: MatMul ----
    def _node(op: str, inputs: List[str], outputs: List[str], name: str) -> bytes:
        n = b""
        for i in inputs:
            n += _field(1, 2, _string(i))
        for o in outputs:
            n += _field(2, 2, _string(o))
        n += _field(3, 2, _string(name))
        n += _field(4, 2, _string(op))
        return n

    # ---- ValueInfoProto ----
    def _type_proto_tensor(elem_type: int, dims: List[Optional[int]]) -> bytes:
        # shape
        shape = b""
        for d in dims:
            dim = b""
            if d is None:
                dim = _field(2, 2, b"batch")  # dim_param
            else:
                dim = _field(1, 0, _varint(d))  # dim_value
            shape += _field(1, 2, dim)
        tensor_type = _field(1, 0, _varint(elem_type)) + _field(2, 2, shape)
        return _field(1, 2, tensor_type)  # tensor_type inside type_proto

    def _value_info(name: str, elem_type: int, dims: List[Optional[int]]) -> bytes:
        vi = _field(1, 2, _string(name))
        vi += _field(2, 2, _type_proto_tensor(elem_type, dims))
        return vi

    # ---- Initializers ----
    init_w1 = _f32_tensor("W1", list(W1.shape), W1.tobytes())
    init_w2 = _f32_tensor("W2", list(W2.shape), W2.tobytes())

    # ---- GraphProto ----
    graph = (
        _field(1, 2, _node("MatMul", ["input", "W1"], ["h"],   "mm1"))
        + _field(1, 2, _node("Relu",  ["h"],           ["hr"],  "relu"))
        + _field(1, 2, _node("MatMul", ["hr", "W2"],   ["out"], "mm2"))
    )
    # GraphProto fields (onnx.proto3): name=2, initializer=5, input=11, output=12
    graph += _field(2, 2, _string("synbench_graph"))
    graph += _field(5, 2, init_w1)
    graph += _field(5, 2, init_w2)
    graph += _field(11, 2, _value_info("input", 1, [1, 64]))
    graph += _field(12, 2, _value_info("out",   1, [1, 64]))

    # ---- ModelProto ----
    # ir_version=7 (field 1), opset_import domain="" version=13 (field 8), graph (field 7)
    opset = _field(1, 2, b"") + _field(2, 0, _varint(13))
    model = (
        _field(1, 0, _varint(7))        # ir_version
        + _field(8, 2, opset)           # opset_import
        + _field(7, 2, graph)           # graph
    )
    return model


# ---------------------------------------------------------------------------
# Provider selection
# ---------------------------------------------------------------------------

_PROVIDER_PRIORITY = [
    "QNNExecutionProvider",        # Qualcomm NPU
    "OpenVINOExecutionProvider",   # Intel NPU / iGPU
    "TensorrtExecutionProvider",   # NVIDIA TensorRT
    "CUDAExecutionProvider",       # NVIDIA CUDA
    "ROCMExecutionProvider",       # AMD ROCm
    "DmlExecutionProvider",        # DirectML (Windows)
    "CoreMLExecutionProvider",     # Apple CoreML / ANE
    "CPUExecutionProvider",        # fallback
]


def _best_provider(available: List[str]) -> str:
    avail_set = set(available)
    for p in _PROVIDER_PRIORITY:
        if p in avail_set:
            return p
    return "CPUExecutionProvider"


# ---------------------------------------------------------------------------
# Benchmark class
# ---------------------------------------------------------------------------

class NPUBenchmark:
    def __init__(self, callback: Optional[Callable[[str], None]] = None):
        self.cb = callback or (lambda msg: None)

    def run(self) -> BenchmarkResult:
        self.cb("Starting NPU / AI inference benchmark …")

        try:
            import onnxruntime as ort  # type: ignore
        except ImportError:
            self.cb("onnxruntime not installed – NPU test skipped.")
            return BenchmarkResult(
                name="npu",
                score=0,
                available=False,
                summary="onnxruntime not installed",
                error="Install onnxruntime: pip install onnxruntime",
            )

        try:
            import numpy as np

            self.cb("Building ONNX model …")
            model_bytes = _build_onnx_mlp()

            available_providers = ort.get_available_providers()
            best = _best_provider(available_providers)
            self.cb(f"Using provider: {best}")

            sess_opts = ort.SessionOptions()
            sess_opts.inter_op_num_threads = 1
            sess_opts.intra_op_num_threads = 4
            sess = ort.InferenceSession(model_bytes, sess_opts, providers=[best])

            inp_name = sess.get_inputs()[0].name
            dummy = np.random.rand(1, 64).astype(np.float32)

            # Warm-up
            for _ in range(50):
                sess.run(None, {inp_name: dummy})

            self.cb(f"Running inference loop ({_DURATION:.0f} s) …")
            count = 0
            start = time.perf_counter()
            while time.perf_counter() - start < _DURATION:
                for _ in range(100):
                    sess.run(None, {inp_name: dummy})
                    count += 1

            elapsed = time.perf_counter() - start
            inf_per_sec = count / elapsed

            score = min(_MAX_SCORE, int(inf_per_sec / _INF_REF * _MAX_SCORE))

            provider_label = best.replace("ExecutionProvider", "")
            summary = f"{inf_per_sec:.0f} inferences/s  [{provider_label}]"
            self.cb(f"NPU done — score {score}")

            return BenchmarkResult(
                name="npu",
                score=score,
                metrics={
                    "inferences_per_sec": inf_per_sec,
                    "provider":           best,
                    "available_providers": available_providers,
                },
                summary=summary,
            )

        except Exception as exc:
            self.cb(f"NPU benchmark error: {exc}")
            return BenchmarkResult(
                name="npu",
                score=0,
                available=False,
                summary="Error",
                error=str(exc),
            )
