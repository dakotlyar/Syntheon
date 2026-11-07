# gpu-shaders

The GPU test's shader toolchain (Phase 4a design §13.1, D9). Every shader is
one HLSL source in `src/tests/gpu/worker/shaders/` (`<name>.hlsl` with
`VSMain`/`PSMain`, plus `common.hlsli`). One source feeds every API:

| Output | Made by | Where | Used by |
|---|---|---|---|
| SPIR-V (Vulkan 1.3) | `build.py`: DXC `-spirv` | `generated/<name>_<vs\|ps>.spv.inc`, committed | Vulkan |
| GLSL 330 core | `build.py`: SPIRV-Cross from that SPIR-V | `generated/<name>_<vs\|ps>.glsl.inc`, committed | OpenGL |
| GL bindings | `build.py`: SPIRV-Cross `--reflect` | `generated/bindings.inc`, committed | OpenGL |
| DXIL (SM 6.0) | the Windows SDK's `dxc.exe` | build tree, `gpu_shaders/<name>_<vs\|ps>.dxil.inc` | D3D12 |
| DXBC (SM 5.0) | the Windows SDK's `fxc.exe` | build tree, `gpu_shaders/<name>_<vs\|ps>.dxbc.inc` | D3D11 |

`generated/STAMP` is the SHA-256 of the HLSL sources, `versions.json` and
`build.py` itself.
CTest's `gpu_shaders_stamp` (`build.py --check`, on every job, no shader
tools needed) fails when a source changed and the outputs weren't
regenerated.

## After changing a shader

```
python3 tools/gpu-shaders/build.py              # every shader
python3 tools/gpu-shaders/build.py --only NAME  # one shader (and bindings.inc; STAMP only from a full run)
```

and commit the source with `generated/`. The outputs are deterministic: a
second run changes nothing. A change to `versions.json` changes `STAMP` too,
so rerun the script after editing it.

Linux x64 only. The first run downloads the pinned tools into
`~/.cache/synbench/tools/` (checked against the SHA-256 in `versions.json`)
and builds SPIRV-Cross there with CMake (a C++ compiler is needed, about half
a minute); later runs take a few seconds. Nothing from them ships.

The cache folder still has the project's old name on purpose. `build.py` hashes
itself into `STAMP`, whose first four characters are stored with every GPU result
as the `shaders` metric, so changing even a comment in it would change that value
when no shader had changed. `build.py` and `font5x7.py` share the folder, and they
can both move the next time the shaders really change.

## A new shader

1. `src/tests/gpu/worker/shaders/<name>.hlsl`, with `VSMain` and `PSMain`
   (a post pass's `VSMain` returns `fullscreenTriangle(id)`), resources on
   the slots of `common.hlsli`'s binding table with its `SB_BIND_*` macros.
2. `syntheon_gpu_shader(NAME <name>)` in `shaders/CMakeLists.txt` (Windows).
3. `build.py`, then commit.

Rules (the design's pitfalls):

- **SH-1:** the HLSL must compile as SM 5.0 (FXC) and SM 6.0 (DXC): no wave
  intrinsics, no 16-bit types, no `[[vk::...]]` other than the bindings
  (`SB_VK_BINDING`, empty outside DXC's SPIR-V mode).
- **SH-2:** every cbuffer member 16-byte aligned, with explicit padding.
- **SH-3:** GLSL 330 has no `binding=`. SPIRV-Cross turns each texture and
  sampler pair into a combined sampler named
  `SPIRV_Cross_Combined<texture><sampler>` (a texture read with `Load` gets
  `SPIRV_Cross_DummySampler`), and `bindings.inc` gives every block and
  sampler its binding point or texture unit.
- Short file names (P-PATH, the 200-character limit).

## What build.py runs

```
dxc -spirv -fspv-target-env=vulkan1.3 -T vs_6_0|ps_6_0 -E VSMain|PSMain <name>.hlsl
spirv-cross <spv> --reflect
spirv-cross <spv> --version 330 --no-es --no-420pack-extension \
    --no-support-nonzero-baseinstance --fixup-clipspace \
    --rename-interface-variable out|in <location> sb_v<location> ...
```

- `--no-420pack-extension`: no `binding=` and no extension a 3.3 driver may lack.
- `--no-support-nonzero-baseinstance`: GL 3.3 has no base instance (pitfall GL-3).
- `--rename-interface-variable`: GLSL 330 links the vertex shader's outputs
  to the pixel shader's inputs by name, and DXC names them differently per
  stage (`out_var_X` / `in_var_X`), so both become `sb_v<location>`.
- The GLSL is embedded as adjacent raw strings of at most 12,000 bytes each
  (MSVC's 16,380-byte limit per literal piece).

## Pins (`versions.json`)

| Tool | Pin | Licence |
|---|---|---|
| DXC | `v1.8.2505.1`, the Linux x64 release tarball (the newest v1.8 release) | NCSA / Apache-2.0 with LLVM exception |
| SPIRV-Cross | tag `vulkan-sdk-1.4.304.0`, GitHub's source archive | Apache-2.0 |

Other entries in `versions.json` (the bitmap font's source, task 8a) are
ignored by `build.py`, but still count in `STAMP`.
