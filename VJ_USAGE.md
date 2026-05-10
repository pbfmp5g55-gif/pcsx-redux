# VJ fork — usage notes

This fork wires the [ps1-primitive-vj] subsystem (`third_party/libvj/`) into
PCSX-Redux as a primitive interceptor. The hooks live in `src/core/gpu.cc`
and the bridge / interceptor lifecycle lives in `src/core/vj_bridge.{h,cc}`.

The upstream README and license still apply to everything else.

[ps1-primitive-vj]: https://github.com/pbfmp5g55-gif/ps1-primitive-vj

## What this fork does differently

- `src/core/gpu.cc` — three call sites in Poly / Line / Rect's `processWrite`
  call `PCSX::vj::onPrimitive(*this)` immediately after `m_gpuLogger->addNode(...)`.
  When that returns `false` the matching `m_gpu->write0(this)` is skipped, so
  the primitive never reaches the renderer.
- `src/core/vj_bridge.cc` owns one process-wide `vj::PrimitiveInterceptor`,
  installs an `Events::GPU::VSync` listener that pumps `endFrame()` /
  `beginFrame()` boundaries, and wires libvj's `SubmitFn` to a `thread_local`
  closure set per intercept call.
- `vsprojects/pcsx-redux.sln` — `testsupport`, `pcsxrunner`, and
  `memoryleakdetector` have their `ReleaseWithClangCL|x64` Build entries
  removed, because the v140 googletest NuGet's auto-link does not match the
  ClangCL+v143 combination on `windows-2022` runners. They still appear in the
  solution but are skipped at build time.
- `vsprojects/support/support.vcxproj` — the `/bigobj` `AdditionalOptions` for
  `gnu-c++-demangler.cc` covers `ReleaseWithClangCL|x64` and
  `ReleaseWithTracy|x64` on top of the upstream Debug / Release entries. The
  file generates more than 65535 COFF sections under any toolset.
- `.github/workflows/windows-build.yml` — a translation of upstream's Azure
  Pipeline targeting `windows-2022`. `/p:PlatformToolset=v143` overrides the
  `v145` declarations in `tracy.vcxproj` / `uriparser.vcxproj` because the
  GitHub-hosted runner image only ships VS 2022. Upstream's Azure runs on
  `windows-2025-vs2026` where v145 is available natively.

## Building

Either let the GitHub Actions workflow build it (push to any branch triggers
"Windows CI (fork)"), or build locally with VS 2022:

```powershell
cd vsprojects
nuget restore pcsx-redux.sln
msbuild pcsx-redux.sln /p:Configuration=ReleaseWithClangCL /p:Platform=x64 `
                       /p:PlatformToolset=v143 /m
```

Outputs land in `vsprojects/x64/ReleaseWithClangCL/`.

## Running with the VJ subsystem active

Stage the binaries and DLLs the way the workflow's "Stage binaries" step does:
rename `pcsx-redux.exe` → `pcsx-redux.main`, then rename `pcsx-wrapper.exe` →
`pcsx-redux.exe`. Drop in the assets and DLLs (`gamecontrollerdb.txt`, fonts,
`avcodec-*.dll`, `glfw3.dll`, etc.).

`pcsx-redux.main` is built as a Windows GUI subsystem binary, so its
`stderr` is not connected to a usable handle when launched normally. The VJ
bridge logs to a file (`vj.log` in the working directory by default; override
with the `VJ_LOG` env var) and that's the diagnostic channel:

```
[VJ] Phase 4 bridge live (master=0.50 chance=0.80 geom=0.70 ...)
[VJ] frame#0 prim#1 kind=1 v=4 tex=0 submitted=1
[VJ] frame#1 prim#1 kind=1 v=4 tex=0 submitted=0    <- libvj dropped this prim
[VJ] frame#2 prim#1 kind=1 v=4 tex=0 submitted=1
...
```

`submitted=0` lines mean the renderer never saw that primitive — the visible
effect is "missing pieces" of the scene. `submitted=1` lines mean libvj
approved the primitive (possibly after mutating its vertices, UVs, or
colors).

The eight glitch parameters are read once at startup from environment
variables. All default to `0.0` (passthrough mode, identical behaviour to an
unmodified emulator). To enable effects:

```powershell
$env:VJ_MASTER   = "1.0"
$env:VJ_CHANCE   = "0.8"
$env:VJ_GEOMETRY = "0.7"
$env:VJ_MISSING  = "0.3"

# pcsx-redux defaults to paused; -run resumes the emulator on launch
.\pcsx-redux.main.exe -bios .\bios\openbios.bin -run

# In another terminal:
Get-Content .\vj.log -Wait -Tail 5
```

| env var       | range  | effect when raised |
|---------------|--------|--------------------|
| `VJ_MASTER`   | `0..1` | Overall scale on every other effect |
| `VJ_CHANCE`   | `0..1` | Per-primitive probability of being touched at all |
| `VJ_GEOMETRY` | `0..1` | Vertex jitter (px) per vertex per primitive |
| `VJ_TEXTURE`  | `0..1` | UV offset on textured primitives |
| `VJ_MISSING`  | `0..1` | Probability of dropping a primitive |
| `VJ_COLOR`    | `0..1` | Per-vertex RGB scaling |
| `VJ_DEPTH`    | `0..1` | Draw-order delay (async submission queue) |
| `VJ_CHAOS`    | `0..1` | Random-hold update / spike rate |

The `-loadexe` and `-iso` options (upstream features) load homebrew or game
images. OpenBIOS alone draws very few primitives per frame, so a ROM is
required to actually *see* the glitches.

## Disabling the VJ subsystem

`PCSX::vj::setEnabled(false)` (declared in `core/vj_bridge.h`) makes
`onPrimitive()` short-circuit to `return true` without ever consulting the
interceptor — equivalent to an unmodified emulator at runtime cost.
