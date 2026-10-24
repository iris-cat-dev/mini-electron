# WebGL and WebGPU

[Documentation index](README.md) · [Project overview](../README.md)

> Run commands from the repository root unless a section explicitly changes directory.

Output compositing, VSync and measurements are documented separately in [rendering performance](RENDERING_PERFORMANCE.md).

## WebGL 1 and 2

Desktop source builds register real `webgl`, `experimental-webgl`, and `webgl2`
contexts for HTMLCanvasElement and OffscreenCanvas. The restored Blink API and
bindings execute GLSL shaders, buffers, textures, framebuffers and WebGL2
instancing through a direct ANGLE EGL/GLES provider, not a mock or a command
buffer stub. Contexts use exact ES2/ES3 versions, WebGL compatibility, robust
access and resource initialization. Unsupported hardware/backend initialization
returns no context rather than pretending to render.

- macOS: native ANGLE Metal backend, initialized before renderer sandbox
  lockdown; it shares the existing named Metal compiler-service permission.
- Windows: native ANGLE D3D11 static library, built with GN/Ninja before the
  normal MSBuild targets and smoke-tested on Windows 10 x64 with an RTX 3090.
  Calls serialize access to D3D11's shared immediate context and invalidate
  native state when Blink and Worker contexts alternate across threads.
- Presentation: real GPU drawing is read back into owned software bitmap
  resources for the existing cc/Viz resource protocol. The output compositor
  may then upload those resources to Metal or D3D12. This is **not zero-copy**
  and does not restore Chromium's GPU service or GPU tile rasterization.
- Canvas/Image/ImageBitmap interop uses CPU pixels. OffscreenCanvas can transfer
  independent ImageBitmaps or publish subsequent worker frames to its visible
  HTML canvas placeholder. Context loss/restoration creates fresh GL resources.
- Native video-frame conversion operates on CPU-mappable pixels; it does not
  add hardware decoding or GPU-only decoder-frame imports. The CPU VideoFrame
  API is described in the WebGPU section below.
- Extensions depend on the native backend. This is not a Khronos WebGL
  conformance certification or a claim of accelerated Canvas2D or XR support.
  WebGPU uses the separate Dawn implementation below. Page WebGL settings
  remain authoritative; the Chromium GPU service's per-origin GPU-crash
  blocklist is not present in this runtime.

The build fetches ANGLE revision
`ce13a00a2b049a1ef5e0e70a3d333ce70838ef7b` into ignored `third_party/angle`.
Its cached archive in `.build-tools` is SHA-256 verified; subsequent builds
reuse it. Unknown populated ANGLE directories are rejected, not overwritten.
Distribution notices include the shipped ANGLE/Khronos/xxHash licenses.

Build and exercise real rendering:

```sh
python build.py --electron --interactive --electron-main test/webgl-smoke.js
# Reuse the built macOS binary:
out/mac-arm64/mini-electron --app=test/webgl-smoke.js
# Exercise WebGL with the CPU output compositor (WebGL itself still uses ANGLE):
MINI_ELECTRON_DISABLE_GPU=1 out/mac-arm64/mini-electron --app=test/webgl-smoke.js
# Reuse the built Windows binary in both output-compositor modes:
out/windows-x64/mini-electron.exe --app=test/webgl-smoke.js
out/windows-x64/mini-electron.exe --disable-gpu --app=test/webgl-smoke.js
```

The smoke checks shader drawing, default depth/stencil framebuffers, texture
uploads, WebGL2 FBO/blit/instancing, read-buffer state preservation, alpha,
resize, serialization, ImageBitmap ownership, context restoration and Worker
placeholder updates. Windows default and CPU output-compositor modes both
passed, including initial/subsequent `capturePage()` pixel assertions; this
does not verify physical desktop scanout. On macOS the smoke independently
samples PNGs with Core Graphics and captures the owned WindowServer window
before and after updates. Those native captures need Xcode tools, an unlocked
graphical session and OS capture authorization.

## WebGPU

Desktop source builds expose real `navigator.gpu` in secure contexts and
register `webgpu` contexts for HTMLCanvasElement and OffscreenCanvas. Blink's
WebGPU bindings execute through a renderer-local Dawn wire client/server and
native backend, not a simulated adapter or a WebGL translation.

- macOS uses Dawn/Metal, warmed before renderer sandbox lockdown. Windows uses
  Dawn/D3D12, compiled and smoke-tested on Windows 10 x64 (build 19045) with an
  NVIDIA RTX 3090. No D3D11/Vulkan WebGPU fallback is built.
- WGSL compilation, render/compute pipelines, buffers, textures, copies,
  mapping, validation/error scopes and device loss use Dawn's implementations.
  Callback delivery stays on the owning Blink/Worker sequence.
- Canvas textures are genuine GPU textures. Presentation and snapshots copy
  GPU pixels to owned CPU images/shared bitmaps for the existing cc/Viz
  resource protocol. The output compositor may upload them again. This is
  **not zero-copy**, Chromium's GPU service or GPU tile rasterization.
- Opaque snapshots change alpha only in their owned CPU copy. Snapshot-only
  operations preserve the live application texture; frame presentation and
  `transferToImageBitmap()` expire it. Offscreen images own independent pixels.
- External image uploads and CPU-mappable video imports use CPU pixels.
  CPU VideoFrame/VideoColorSpace constructors and cropped, strided RGB/YUV
  `VideoFrame.copyTo()` conversions are exposed in Window and DedicatedWorker
  contexts. Hardware decoding, texture-only decoder imports, accelerated
  Canvas2D and GPU-service-backed XR projection layers are not added.
- Adapter features/limits depend on the backend and shader compiler; the Windows
  build does not bundle DXC. Unsupported adapters return `null`; native
  validation failures remain errors. This is not Metal feature parity, WebGPU
  conformance certification or a claim of support for newer Chromium APIs.

Building requires Python 3.12+, CMake 3.24+ and the platform's existing native
toolchain. The bootstrap fetches the SHA-256-verified source tree for Dawn
revision `73dbf565079c89a531e6e01c4e8fc048a8a9660b`, its pinned Abseil and
SPIRV-Headers, and the build-time Jinja2/MarkupSafe dependencies into ignored
directories. Source hashes cover paths, entry types, permissions, symlink
targets and file bytes; they exclude archive timestamps and ownership because
Gitiles regenerates that metadata. Only the selected native backend and
WGSL/Tint components are built. Dawn's Abseil symbols use a private namespace
to avoid ABI collisions with Chromium's independently pinned Abseil; verified
source files are unchanged. Distribution notices include the shipped
Dawn/Tint, Abseil and SPIRV licenses.

```sh
python build.py --electron --interactive --electron-main test/webgpu-smoke.js
# Reuse the built macOS binary:
out/mac-arm64/mini-electron --app=test/webgpu-smoke.js
# CPU output composition, still executing WebGPU on native Metal:
MINI_ELECTRON_DISABLE_GPU=1 out/mac-arm64/mini-electron --app=test/webgpu-smoke.js
# Reuse the built Windows binary (native D3D12 WebGPU in both modes):
out/windows-x64/mini-electron.exe --app=test/webgpu-smoke.js
out/windows-x64/mini-electron.exe --disable-gpu --app=test/webgpu-smoke.js
```

The smoke uses a localhost secure origin and real shaders/compute, validates
mapped results, error scopes, detached inputs, snapshot lifetime/alpha,
OffscreenCanvas images, Worker ownership and device loss. It decodes actual
`capturePage()` PNGs and checks initial and subsequent composed pixels.
Windows default and CPU output-compositor modes both passed on the RTX 3090
machine above, including CPU VideoFrame RGB/YUV conversion, cropped/strided
copies, alpha preservation and Worker VideoFrame lifecycle. The original
rendering/snapshot cases passed in both output-compositor modes on Apple M4
Pro; these Windows compatibility changes have not been rerun on macOS.
The original OMP Desktop 0.5.2 homepage WASM also rendered and animated in a
local macOS packaged app, without changing the application or replacing its
effect assets. These are correctness checks, not throughput, physical-scanout
or energy measurements.
