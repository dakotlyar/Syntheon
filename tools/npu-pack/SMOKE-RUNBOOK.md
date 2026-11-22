# Nivedellir compile-first smoke — runbook (DD-1 §6)

This settles **Option B** (raw-C stateful decode in `engine_openvino.cpp`) vs
**Option A** (OpenVINO GenAI `LLMPipeline`) for the ~1 B stateful LLM on the
Intel NPU. The smoke (`smoke_llm.py`) loads the pack's `openvino_model.xml`,
compiles it on the NPU the same way the engine does, confirms it really landed
on the NPU, then runs one autoregressive decode and reports what it observed —
including every value the C++ engine had to guess (`// SMOKE:` comments).

Nivedellir is a **Windows** box on the `.10` subnet, IP `192.168.10.39` (the
coordinator's temp-access script targets it). You run the steps below from an
**elevated PowerShell on Nivedellir**; the coordinator needs the
`smoke-out.txt` file afterwards.

## 0. Outcome table (what you're looking for)

| Last `RESULT:` line | exit | Meaning | Action |
|---|---|---|---|
| `RESULT: OK (Option B confirmed …)` | 0 | Decoder compiled on NPU, `EXECUTION_DEVICES` has NPU, tokens generated, finite, sane text | Option B works — hand the log to the coordinator; the engine's `// SMOKE:` guesses get replaced with the observed values |
| `RESULT: FAILED … not on the NPU …` | 5 | Every compile variant fell back to CPU or the NPUW property was rejected | The engine's guessed property key/value is wrong or unneeded — flag it; likely still Option B with an engine fix |
| `RESULT: FAILED … KV state rejected …` | 7 | Internal KV persistence doesn't hold on the NPU | **Option B → A trigger** (GenAI `LLMPipeline`) |
| `RESULT: FAILED EOS on step 1 …` | 6 | Decoder runs but emits garbage | Investigate prompt/EOS before deciding; possibly Option A |
| `RESULT: FAILED … timeout …` | 7 | Compile/decode exceeded the deadline | Re-run with a larger `--timeout-seconds`, or the NPU is too slow → A |
| `RESULT: FAILED … openvino … not installed` | 4 | Install step failed | Fix the Python/OpenVINO install and re-run |

An `OK` with loud `NOTE` lines is **not** ambiguous on its own — read those
notes (e.g. "property rejected, plain compile reached NPU" or "Paris check: no
match") and paste the whole file to the coordinator either way.

---

## 1. Power on Nivedellir

- Power it on, log in, keep it on **AC power** and stop it from sleeping
  (Settings → System → Power → "Never" sleep; nothing else heavy should run —
  this is a benchmark machine).
- Close anything else that could load the NPU or CPU.

## 2. Grant temp SSH access

The coordinator has prepared an updated `nivedellir-temp-access.ps1` (target
`192.168.10.39`). Run that on the Linux side **once**; it installs the temp
key Dave uses to copy files in. Reference it — don't rewrite it.

## 3. Copy the smoke script onto Nivedellir

From the Linux side (Knowhere or Tiamat), push the script over the temp SSH
grant (use the user/port `nivedellir-temp-access.ps1` reports):

```bash
scp -P <port> ~/Nextcloud/Documents/Projects/Syntheon/tools/npu-pack/smoke_llm.py <user>@192.168.10.39:smoke_llm.py
```

If the script is already somewhere on Nivedellir from an earlier copy, reuse
that and skip this step.

## 4. The one PowerShell block (OpenVINO + pack + run)

Paste this whole block into an **elevated PowerShell on Nivedellir**. It
(1) checks for the Python `openvino` package and installs the pinned version if
absent, (2) downloads the LLM pack into `%USERPROFILE%\syntheon-smoke\` — or
reuses a copy the app already cached under `%LOCALAPPDATA%` — and (3) runs the
smoke, saving everything to `smoke-out.txt`.

> If `python` isn't on your PATH but `py -3` is, replace `python` with `py -3`
> in the block below.

```powershell
# --- 1) OpenVINO for Python (pinned to the pack toolchain) --------------------
python -c "import openvino" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host "openvino missing - installing 2026.4.1 ..."
  python -m pip install openvino==2026.4.1
  if ($LASTEXITCODE -ne 0) { Write-Host "pip install failed - install OpenVINO manually and re-run."; exit 1 }
  Write-Host "openvino installed."
}

# --- 2) The LLM model pack ----------------------------------------------------
$base   = "https://192.168.100.160:8443/npu-models/2/intel/llm/llm-1b-int4/1"
$dest   = Join-Path $env:USERPROFILE "syntheon-smoke\llm-1b-int4"
$files  = @("openvino_model.xml","openvino_model.bin","openvino_tokenizer.xml","openvino_tokenizer.bin",
            "openvino_detokenizer.xml","openvino_detokenizer.bin","openvino_config.json","config.json",
            "generation_config.json","tokenizer.json","tokenizer_config.json","chat_template.jinja")

# Reuse the app's own downloader cache if it already holds this pack.
$appCache = Get-ChildItem -Path $env:LOCALAPPDATA -Directory -ErrorAction SilentlyContinue |
  ForEach-Object { Join-Path $_.FullName "npu-models\2\intel\llm\llm-1b-int4\1" } |
  Where-Object { (Test-Path (Join-Path $_ "openvino_model.xml")) -and (Test-Path (Join-Path $_ "openvino_model.bin")) } |
  Select-Object -First 1
if ($appCache) { $dest = $appCache.FullName; Write-Host "reusing the app's pack cache at $dest" }
else {
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  # Mjolnir serves over HTTPS with a SELF-SIGNED CA; the one-off callback below
  # skips cert validation for this download only (the CA-trust alternative is to
  # import mjolnir-ca.crt into the machine's trusted store — either is fine).
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
  foreach ($f in $files) {
    Invoke-WebRequest -UseBasicParsing -Uri "$base/$f" -OutFile (Join-Path $dest $f)
    Write-Host "  fetched $f"
  }
  Write-Host "pack downloaded to $dest (the 631 MB openvino_model.bin is the bulk)."
}

# --- 3) Run the smoke ---------------------------------------------------------
python smoke_llm.py --model-dir $dest 2>&1 | Tee-Object -FilePath smoke-out.txt
$rc = $LASTEXITCODE
Write-Host "`n=== smoke exit code: $rc  (full log saved to smoke-out.txt) ==="
exit $rc
```

The manifest pin for this pack (`pack_version 2`, `manifest.sha256` =
`49c667e5…`) covers the 12 files above; if a download looks truncated, delete
`$dest` and re-run — the smoke itself checks only that the files it needs
(`openvino_model.xml`, both tokenizer IRs, `generation_config.json`) exist.

## 5. What happened / what to send back

- Read the last `RESULT:` line and the `smoke_llm:` notes above it (decoder
  input names/shapes, EXECUTION_DEVICES, EOS id, ttft, token count, decoded
  text, Paris match).
- Send **`smoke-out.txt`** to the coordinator (Antilles). One file, that's it.
- If the smoke hit the timeout on a cold compile (stateful INT4 compile on the
  NPU can take a minute), re-run with
  `python smoke_llm.py --model-dir $dest --timeout-seconds 600`.

## 6. Follow-up (NOT part of this first run)

DD-1 §6 step 4's cross-check — `benchmark_app`'s `llm.tps` figure — is a
separate follow-up once the smoke settles the compile/decode path. Do not run
it as part of this runbook.
