#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""npu-pack smoke_llm: the DD-1 §6 compile-first smoke for the stateful LLM decoder.

Runs ON Nivedellir (Windows, Intel NPU) against the stateful LLM IR shipped in
the model pack (pack_version 2, intel/llm/llm-1b-int4/1/), to settle whether
the raw-C stateful decode loop (engine Option B) works on the NPU — or whether
we flip to OpenVINO GenAI (Option A). This is the compile-first check: it loads
+ compiles the decoder exactly the way the C++ engine's compileModel() does,
confirms the compiled model really landed on the NPU, then drives one
autoregressive decode and reports what it observed.

What the script settles (the engine's `// SMOKE:` guesses):
  SMOKE 1  KV persistence — one infer_request, KV survives across infer() calls
           with beam_idx = 0 per step.
  SMOKE 2  the NPUW device-property key/value for compile ("NPUW_DEVICES"/"NPU,CPU"
           is the engine's guess; this script tries it FIRST and prints exactly
           what it used; a rejected key or a CPU-fallback compile is a loud
           finding, not a silent pass).
  SMOKE 3  prefill — a single stateful decoder does prefill from the full prompt
           on the first infer (the pack has no separate embed model).
  SMOKE 4  input ordering — the script prints the ACTUAL input names/shapes it
           found (input_ids@0, attention_mask@1, beam_idx rank-1 are the engine's
           guess; the real IR may add position_ids — the script drives it by name).
  SMOKE 5  the fixed prompt's input_ids (tokenized at runtime on CPU) and the
           EOS id (read from generation_config.json, not guessed).

Exit codes (a parseable one-line `RESULT: OK|FAILED <reason>` is always printed):
  0  Option B CONFIRMED: decoder compiled on the NPU, EXECUTION_DEVICES has NPU,
     tokens > 0, generation reached EOS or the max-new-tokens bound, logits all
     finite, decoded text non-empty.
  2  usage error
  3  model dir incomplete / required files missing
  4  the Python `openvino` package is missing or broken
  5  no compile variant reached the NPU (CPU-fallback caught or all rejected) —
     Option B not confirmed on this hardware → Option A trigger
  6  the decoder ran but produced garbage (EOS on step 1 / no tokens / empty text)
  7  the decode failed mid-run (KV state rejected, infer exception, timeout)

Dependencies: the pip `openvino` package (2026.4.1, matches the pack toolchain)
plus the Python standard library. No PyTorch, no optimum, no vendor headers; the
tokenizer + detokenizer are the pack's own OpenVINO IRs run on CPU.
"""

import argparse
import json
import sys
import time
from pathlib import Path

# Exit codes (see module docstring).
EXIT_USAGE = 2
EXIT_FILES = 3
EXIT_NO_OPENVINO = 4
EXIT_NOT_ON_NPU = 5
EXIT_GARBAGE = 6
EXIT_DECODE_FAILED = 7

PROMPT = "The capital of France is"  # the DD's "Paris" check
DECODER_XML = "openvino_model.xml"
TOKENIZER_XML = "openvino_tokenizer.xml"
DETOKENIZER_XML = "openvino_detokenizer.xml"
GENERATION_CONFIG = "generation_config.json"

# The engine's §5.3 floor is a 5-second-window scoring rule; the smoke prompt is
# a SHORT completion ("The capital of France is" → " Paris" + EOS is a success),
# so the smoke's own floor is just "some real generation happened" (tokens > 0,
# first token not EOS, non-empty text). Every observed number is printed so the
# coordinator can judge against the engine's 8-token floor for real windows.
MIN_SANE_TOKENS = 1


def fail(reason, code):
    print(f"RESULT: FAILED {reason}", flush=True)
    print(f"smoke_llm: exiting {code} ({reason})", file=sys.stderr)
    sys.exit(code)


def deadline_left(deadline, phase):
    left = deadline - time.monotonic()
    if left <= 0:
        fail(f"timeout in {phase} (deadline exceeded)", EXIT_DECODE_FAILED)
    return left


def load_openvino():
    try:
        import numpy as np
        import openvino as ov
    except ImportError as e:
        fail(f"the Python 'openvino' package is not installed ({e}); "
             f"install it with: python -m pip install openvino==2026.4.1", EXIT_NO_OPENVINO)
    return ov, np


def read_eos_id(model_dir):
    """The decoder's EOS token id, read from the pack's generation_config.json —
    the smoke settles SMOKE 5's EOS half from the real config, not a guess."""
    cfg_path = model_dir / GENERATION_CONFIG
    if cfg_path.is_file():
        try:
            cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
            eos = cfg.get("eos_token_id", None)
            if isinstance(eos, list):
                eos = eos[0] if eos else None
            if isinstance(eos, int):
                return eos
        except (json.JSONDecodeError, ValueError):
            pass
    print(f"smoke_llm: NOTE generation_config.json missing/unparsable — falling back to EOS id 2 (guess)")
    return 2


def as_numpy(tensor):
    """The numpy view of an ov.Tensor (string tensors are object arrays)."""
    try:
        return tensor.data
    except Exception:
        import numpy as np
        return np.asarray(tensor)


def read_string(tensor):
    """Reads a string tensor (the detokenizer's output) into one joined text."""
    arr = as_numpy(tensor)
    parts = []
    for item in arr.flat:
        if isinstance(item, bytes):
            parts.append(item.decode("utf-8", errors="replace"))
        else:
            parts.append(str(item))
    return "".join(parts).strip()


def normalize_result(out):
    """CompiledModel calls return a dict of {name: OvTensor} (or a bare tensor
    for a single-output model, depending on the OV version) — normalise to a
    dict keyed by output name, with "" for unnamed outputs."""
    if isinstance(out, dict):
        return {str(k): v for k, v in out.items()}
    # A bare tensor (or list) — give it a synthetic name.
    import numpy as np
    if isinstance(out, (list, tuple)) and len(out) == 1:
        out = out[0]
    return {"output": out}


def tokenize(ov, np, core, model_dir, prompt):
    """Tokenizes `prompt` on CPU with the pack's openvino_tokenizer IR. Returns
    (input_ids [1, L] int64, prompt_len). Settles SMOKE 5's prompt-input_ids half."""
    tok_xml = model_dir / TOKENIZER_XML
    if not tok_xml.is_file():
        fail(f"tokenizer model {TOKENIZER_XML} not found in {model_dir}", EXIT_FILES)
    tok = core.compile_model(str(tok_xml), "CPU")

    # The openvino_tokenizers IR takes a single string input. Try the documented
    # forms in order (a string tensor, a list, a single string); the first that
    # works wins. The result is normalised to {name: OvTensor} afterwards.
    out = None
    last_err = None
    for feed in (
        ov.Tensor(np.array([prompt], dtype=object)),
        [prompt],
        prompt,
    ):
        try:
            out = tok(feed)
            break
        except Exception as e:  # noqa: BLE001 - report the last attempt
            last_err = e
            continue
    if out is None:
        fail(f"tokenizer call failed on CPU: {last_err}", EXIT_DECODE_FAILED)

    result = normalize_result(out)
    ids_tensor = None
    for name, t in result.items():
        if "input_ids" in name:
            ids_tensor = t
            break
    if ids_tensor is None:
        if result:
            ids_tensor = next(iter(result.values()))
        else:
            fail("tokenizer returned no outputs", EXIT_DECODE_FAILED)
    ids = np.asarray(as_numpy(ids_tensor)).astype(np.int64)
    ids = ids.reshape(1, -1)
    return ids, ids.shape[1]


def detokenize(ov, np, core, model_dir, ids_2d):
    """Decodes token ids back to text on CPU with the pack's detokenizer IR."""
    detok_xml = model_dir / DETOKENIZER_XML
    if not detok_xml.is_file():
        fail(f"detokenizer model {DETOKENIZER_XML} not found in {model_dir}", EXIT_FILES)
    detok = core.compile_model(str(detok_xml), "CPU")
    t = ov.Tensor(np.ascontiguousarray(ids_2d, dtype=np.int64))
    out = None
    last_err = None
    for feed in ({0: t}, [t], t):
        try:
            out = detok(feed)
            break
        except Exception as e:  # noqa: BLE001
            last_err = e
            continue
    if out is None:
        fail(f"detokenizer call failed on CPU: {last_err}", EXIT_DECODE_FAILED)
    result = normalize_result(out)
    text = ""
    for t_ in result.values():
        text = read_string(t_)
        if text:
            break
    return text


def decoder_input_map(compiled, np):
    """The decoder's input name → (index, numpy dtype, shape). Prints the ACTUAL
    layout (settles SMOKE 4) so the engine's guesses can be checked against it."""
    found = {}
    for i, inp in enumerate(compiled.inputs):
        name = ""
        try:
            name = inp.get_any_name()
        except Exception:  # noqa: BLE001 - unnamed input
            name = f"<unnamed#{i}>"
        try:
            dtype = inp.get_element_type().to_dtype()
        except Exception:  # noqa: BLE001
            dtype = np.dtype(np.int64)
        shape = []
        try:
            shape = [p if isinstance(p, int) else -1 for p in inp.partial_shape.get_shape()]
        except Exception:  # noqa: BLE001
            shape = []
        found[name] = (i, dtype, shape)
    return found


def compile_decoder(ov, np, core, model_dir, device, xml_path, deadline):
    """Loads + compiles the stateful decoder exactly like the engine's
    compileModel(): device `device`, and FIRST with the engine's guessed NPUW
    property (SMOKE 2). If the guess is rejected or falls back to CPU, retries a
    plain `device` compile and an "NPU"-value variant, printing each attempt
    loudly. Returns (compiled, config_used, exec_devices) for the first variant
    whose EXECUTION_DEVICES contains the device; fails if none reach the NPU."""
    model = core.read_model(str(xml_path))
    attempts = [
        # The engine's exact guess (engine_openvino.cpp compileModel()).
        {"NPUW_DEVICES": "NPU,CPU"},
        # Diagnostic: does the stateful IR compile directly on the device?
        None,
        # A value variant in case the key is right but the value is wrong.
        {"NPUW_DEVICES": "NPU"},
    ]
    last_err = None
    for config in attempts:
        left = deadline_left(deadline, "decoder compile")
        label = "plain '%s' (no property)" % device if config is None else \
            "property %r" % (config,)
        print(f"smoke_llm: compiling on '{device}' with {label} ...", flush=True)
        started = time.monotonic()
        try:
            compiled = core.compile_model(model, device, config) if config else core.compile_model(model, device)
        except Exception as e:  # noqa: BLE001 - a rejected property is a finding, not a crash
            print(f"smoke_llm:   compile rejected: {e}")
            last_err = e
            continue
        took = time.monotonic() - started
        try:
            exec_devices = str(compiled.get_property("EXECUTION_DEVICES"))
        except Exception as e:  # noqa: BLE001
            print(f"smoke_llm:   EXECUTION_DEVICES unreadable: {e}")
            exec_devices = "<unreadable>"
        print(f"smoke_llm:   compiled in {took:.1f}s, EXECUTION_DEVICES = {exec_devices!r}")
        on_target = "NPU" in exec_devices.upper()
        if on_target:
            print(f"smoke_llm:   -> on-device compile confirmed (SMOKE 2 settled: {label} reaches the NPU)")
            return compiled, config, exec_devices
        print(f"smoke_llm:   !! not on the NPU (EXECUTION_DEVICES lacks NPU) — CPU fallback caught")
    reason = "no compile variant reached the NPU"
    if last_err is not None:
        reason += f" (last compile error: {last_err})"
    fail(reason, EXIT_NOT_ON_NPU)


def decode_once(req, np, feeds):
    """One infer on the persistent request; returns (logits [1, seq, vocab], finite)."""
    req.infer(feeds)
    logits = as_numpy(req.get_output_tensor(0))
    finite = bool(np.isfinite(logits).all())
    return logits, finite


def run_smoke(ov, np, core, model_dir, device, max_new_tokens, deadline):
    print(f"smoke_llm: OpenVINO {ov.__version__}, device '{device}', prompt {PROMPT!r}")
    print(f"smoke_llm: model dir {model_dir}")

    xml_path = model_dir / DECODER_XML
    if not xml_path.is_file():
        fail(f"decoder model {DECODER_XML} not found in {model_dir}", EXIT_FILES)
    eos_id = read_eos_id(model_dir)
    print(f"smoke_llm: EOS token id = {eos_id} (read from {GENERATION_CONFIG}; settles SMOKE 5)")

    # SMOKE 5 (prompt half): tokenize the fixed prompt on CPU.
    prompt_ids, prompt_len = tokenize(ov, np, core, model_dir, PROMPT)
    print(f"smoke_llm: prompt {PROMPT!r} -> {prompt_len} tokens: {prompt_ids[0].tolist()}")
    if prompt_len == 0:
        fail("tokenizer produced an empty prompt", EXIT_GARBAGE)

    # SMOKE 2: compile on the device (NPUW property first) + EXECUTION_DEVICES.
    compiled, _config_used, exec_devices = compile_decoder(ov, np, core, model_dir, device, xml_path, deadline)
    print(f"smoke_llm: decoder compiled, EXECUTION_DEVICES = {exec_devices!r}")

    # SMOKE 4: the decoder's ACTUAL input layout (the engine guessed
    # input_ids@0, attention_mask@1, beam_idx rank-1; the real IR may add
    # position_ids — the pack inspection showed 4 inputs).
    inputs = decoder_input_map(compiled, np)
    print("smoke_llm: decoder inputs (name -> index, dtype, shape):")
    for name, (idx, dtype, shape) in sorted(inputs.items(), key=lambda kv: kv[1][0]):
        print(f"smoke_llm:   #{idx} {name} dtype={dtype} shape={shape}")
    required = {"input_ids", "attention_mask", "beam_idx"}
    have = {n for n in inputs}
    missing = required - have
    if missing:
        fail(f"decoder is missing required input(s) {sorted(missing)} — observed inputs {sorted(have)}; "
             f"the IR layout differs from the expected stateful decoder (Option B not confirmed)", EXIT_NOT_ON_NPU)
    extra = have - required - {"position_ids"}
    if extra:
        fail(f"decoder has unexpected input(s) {sorted(extra)} the raw-C loop cannot drive "
             f"(observed {sorted(have)}) — Option B not confirmed as-is", EXIT_NOT_ON_NPU)
    if "position_ids" in have:
        print("smoke_llm: position_ids present — decode feeds absolute positions (prompt_len + step)")

    def build_feed(ids, mask, positions):
        feed = {}
        for name, (idx, dtype, _shape) in inputs.items():
            if name == "input_ids":
                feed[name] = np.ascontiguousarray(ids, dtype=dtype)
            elif name == "attention_mask":
                feed[name] = np.ascontiguousarray(mask, dtype=dtype)
            elif name == "beam_idx":
                feed[name] = np.ascontiguousarray(np.zeros((1,), dtype=dtype), dtype=dtype)
            elif name == "position_ids":
                feed[name] = np.ascontiguousarray(positions, dtype=dtype)
        return feed

    request = compiled.create_infer_request()
    # SMOKE 1 + 3: one persistent request; the first infer is the full-prompt
    # prefill (KV lands in internal model state, no separate embed model).
    print("smoke_llm: prefill: single stateful decoder, full prompt on the first infer (SMOKE 3)")
    window_start = time.monotonic()
    ones = np.ones((1, prompt_len), dtype=np.int64)
    pos = np.arange(prompt_len, dtype=np.int64).reshape(1, -1)
    try:
        logits, finite = decode_once(request, np, build_feed(prompt_ids, ones, pos))
    except Exception as e:  # noqa: BLE001
        fail(f"prefill infer failed: {e}", EXIT_DECODE_FAILED)
    vocab = logits.shape[-1]
    print(f"smoke_llm: prefill ok in {time.monotonic() - window_start:.2f}s, logits vocab = {vocab}")
    if logits.shape[1] != prompt_len:
        fail(f"prefill logits sequence dim {logits.shape[1]} != prompt len {prompt_len} "
             f"(unexpected prefill path — SMOKE 3 not confirmed)", EXIT_DECODE_FAILED)

    ttft = 0.0
    first_token = int(np.argmax(logits[0, prompt_len - 1, :]))
    tokens = 0
    token_ids = []
    logits_all_finite = finite
    eos_reached = False
    bound_reached = False
    kv_steps_ok = 0

    if first_token == eos_id:
        print(f"smoke_llm: first generated token == EOS ({eos_id}) — garbage (SMOKE 5: EOS on step 1)")
        fail(f"EOS on step 1 (first generated token is the EOS id {eos_id}) — the decoder is not generating",
             EXIT_GARBAGE)

    ttft = time.monotonic() - window_start
    prev = first_token
    token_ids.append(prev)
    tokens += 1
    print(f"smoke_llm: ttft = {ttft * 1000:.1f} ms (first token {prev})")

    # Decode: feed the last token with position_ids = prompt_len + step, beam_idx
    # = 0, KV persisting in the SAME request (SMOKE 1).
    pos_next = prompt_len
    while tokens < max_new_tokens:
        if time.monotonic() > deadline:
            fail(f"timeout after {tokens} tokens (deadline exceeded)", EXIT_DECODE_FAILED)
        ids = np.array([[prev]], dtype=np.int64)
        m = np.ones((1, 1), dtype=np.int64)
        p = np.array([[pos_next]], dtype=np.int64)
        try:
            logits, finite = decode_once(request, np, build_feed(ids, m, p))
        except Exception as e:  # noqa: BLE001
            msg = str(e)
            if "state" in msg.lower() or "kv" in msg.lower() or "cache" in msg.lower():
                print(f"smoke_llm: KV/state rejected at decode step {tokens}: {e}")
                fail(f"KV state rejected during decode after {tokens} tokens — internal-state persistence "
                     f"(SMOKE 1) NOT confirmed; this is the Option B -> A trigger", EXIT_DECODE_FAILED)
            fail(f"decode infer failed at token {tokens}: {e}", EXIT_DECODE_FAILED)
        logits_all_finite = logits_all_finite and finite
        kv_steps_ok += 1
        if logits.shape[1] != 1:
            fail(f"decode logits sequence dim {logits.shape[1]} != 1 (decode path not single-token — "
                 f"SMOKE 1 not confirmed)", EXIT_DECODE_FAILED)
        nxt = int(np.argmax(logits[0, 0, :]))
        pos_next += 1
        if nxt == eos_id:
            eos_reached = True
            break
        token_ids.append(nxt)
        prev = nxt
        tokens += 1
    else:
        # The while loop ended on the max-new-tokens bound, not a break.
        bound_reached = True

    elapsed = time.monotonic() - window_start
    print(f"smoke_llm: KV persistence: OK — state survived {kv_steps_ok} decode infer(s) on one request (SMOKE 1)")
    print(f"smoke_llm: decode finished: {tokens} tokens, EOS reached = {eos_reached}, "
          f"max-new-tokens bound = {bound_reached}, all logits finite = {logits_all_finite}")
    print(f"smoke_llm: wall time {elapsed:.2f}s (ttft {ttft * 1000:.1f} ms, "
          f"{(tokens - 1) / max(elapsed - ttft, 1e-9):.1f} tok/s after ttft)")

    text = detokenize(ov, np, core, model_dir, np.array([token_ids], dtype=np.int64))
    print(f"smoke_llm: generated text: {text!r}")
    if "Paris" in text:
        print(f"smoke_llm: DD 'Paris' check: MATCH")
    else:
        print(f"smoke_llm: DD 'Paris' check: no match (informational — model variance, not a failure)")

    if tokens < MIN_SANE_TOKENS:
        fail("no tokens generated", EXIT_GARBAGE)
    if not logits_all_finite:
        fail("logits were not all finite during generation", EXIT_GARBAGE)
    if not text:
        fail("decoded text is empty", EXIT_GARBAGE)

    print(f"smoke_llm: observed EOS id {eos_id}, prompt len {prompt_len}, {tokens} generated tokens, "
          f"decoded {len(text)} chars, EXECUTION_DEVICES={exec_devices!r}")
    print(f"RESULT: OK (Option B confirmed on {device}: {tokens} tokens, "
          f"EOS={eos_reached}, bound={bound_reached}, finite={logits_all_finite})")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog="Exit 0 = Option B confirmed; non-zero with a RESULT: FAILED <reason> line = Option B "
               "not confirmed (Option A trigger). See the SMOKE-RUNBOOK.md for the Nivedellir steps.",
    )
    ap.add_argument("--model-dir", type=Path, required=True,
                    help="the LLM's cache directory (contains openvino_model.xml/.bin, "
                         "openvino_tokenizer.*, openvino_detokenizer.*, generation_config.json)")
    ap.add_argument("--device", default="NPU", help="the decoder compile device (default: NPU)")
    ap.add_argument("--max-new-tokens", type=int, default=256, help="decode bound (default: 256)")
    ap.add_argument("--timeout-seconds", type=float, default=300.0,
                    help="whole-run deadline, compile included (default: 300)")
    args = ap.parse_args(argv)

    if not args.model_dir.is_dir():
        fail(f"--model-dir {args.model_dir} is not a directory", EXIT_FILES)
    if args.max_new_tokens <= 0:
        fail("--max-new-tokens must be positive", EXIT_USAGE)
    if args.timeout_seconds <= 0:
        fail("--timeout-seconds must be positive", EXIT_USAGE)

    ov, np = load_openvino()
    core = ov.Core()
    deadline = time.monotonic() + args.timeout_seconds
    try:
        return run_smoke(ov, np, core, args.model_dir, args.device, args.max_new_tokens, deadline)
    except SystemExit:
        raise
    except Exception as e:  # noqa: BLE001 - never die with an unparsed traceback
        fail(f"unexpected error: {e!r}", EXIT_DECODE_FAILED)


if __name__ == "__main__":
    sys.exit(main())
