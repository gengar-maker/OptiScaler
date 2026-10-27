# Native SYCL NR backend boundary

This directory contains an *OptiScaler-owned* Intel SYCL model provider. It
does not reuse the NVIDIA NGX forwarder ABI. `ArcNrSyclApi.h` is the versioned
interface to `arc_nr_sycl.dll`; `ArcNrSyclProvider.cpp` builds that module
with Intel oneAPI and the reconstructed SNR model; `ArcNrSyclLoader.cpp` is
compiled into OptiScaler and loads/checks the module ABI. ABI v5 retains
host buffers for standalone tests and D3D12 shared GPU heap handles, and
adds a second shared heap for RG16F/RG32F motion vectors. ABI v5 also accepts
RGBA8/BGRA8 swapchain bytes for the Valheim path; the SYCL GPU converts them
to and from the model's RGBA16F tensors.
The game path copies between tiled textures and row-major shared buffers on
the D3D12 queue. SYCL imports those buffers as device memory; no frame pixels
pass through host memory.

The provider keeps the model, weights and device buffers alive across calls.
On Intel Arc Pro B70 it built and evaluated a captured 1920×1080 frame through
the ABI. Its RGBA16F output was bit-for-bit identical to the existing SNR
offline SYCL runner; the RGB MAE against the captured NVIDIA `model_output`
was 0.00942. A second evaluation through the same session with history and
zero motion also completed. This is not NVIDIA parity.

Build/test on an Intel oneAPI environment:

```text
cmake -S OptiScaler/dlssnr/sycl -B build-sycl \
  -DCMAKE_CXX_COMPILER=icpx -DSNR_RUNTIME_DIR=/path/to/SNR/runtime
cmake --build build-sycl --target arc_nr_sycl_provider_smoke -j
build-sycl/arc_nr_sycl_provider_smoke LOGICAL_DIR DERIVED_DIR \
  1920 1080 COLOR_RGBA.f16 OUTPUT_RGBA.f16 [--temporal]
```

On Windows, pass the Intel oneAPI compiler and oneDNN include/library paths
to CMake, then place `arc_nr_sycl.dll` and its runtime dependencies beside
OptiScaler. The Windows provider build, standalone invocation, and native
D3D12 path in Black Myth: Wukong have passed on Intel Arc Pro B70.
For the full model with fused FFNs, configure
`-DARC_NR_SYCL_EXACT_FUSED_FFN_DEFAULT=ON`. This profile retains the original
network shape, complete QK/PV attention and normalization, and all temporal
inputs. It does not turn on the approximate `ARC_NR_SYCL_FAST_DEFAULT` profile;
the two CMake options are mutually exclusive. Set `SNR_FUSED_DENSE_FFN=0`
and `SNR_FUSED_BRANCHED_FFN=0` to compare with the unfused reference path.
The provider defaults to `ARC_NR_DISABLE_E4M3=ON`: tensors remain FP16 at
publication boundaries instead of being rounded to E4M3 values held in FP16.
All learned layers and temporal inputs still run. Set
`-DARC_NR_DISABLE_E4M3=OFF` for the original numerical model. The FP16 mode
changes image values and must be checked on the target game; it does not
turn the model into a native FP8 or INT8 implementation.
For a reproducible Windows provider build and non-black frame test, follow
[windows/README.md](windows/README.md). That test does not verify a game's
native D3D12 command-list boundary.

The chosen scheduling contract is **same-frame**. The D3D11→D3D12 and
Vulkan→D3D12 bridges own their command allocators, lists and queue. The new
`ArcNrSyclOwnedListBridge` submits the encoded input, waits for a D3D12 fence,
evaluates SYCL over shared GPU buffers, copies the answer into the output
texture, waits for another fence, then resets the owned list so the caller
can record the resolve and copy-back. It rejects non-RGBA16F surfaces.

The D3D11-to-D3D12 upscaler bridge now calls the owned-list primitive when
`[DlssNr] Backend=1`. Put `arc_nr_sycl.dll` and the `arc_nr_weights` directory
(with `logical` and `derived` children) beside OptiScaler. The full Windows
OptiScaler solution builds with this integration. The provider has passed a
standalone GPU frame test, but the D3D11 upscaler bridge path has not been exercised.

Valheim and Tekken 7 have no intercepted temporal upscaler call. In DirectX 11 mode,
`ValheimNr_Dx11` runs before swapchain Present when `[DlssNr] Enabled=true`
and `Backend=1`. It copies the finished RGBA8, BGRA8, RGB10A2 or RGBA16F
frame into a shared D3D11
texture, uses D3D11/D3D12 fences and the owned-list bridge to run the SYCL
model, and copies the answer back to the game backbuffer. Frame pixels do not
pass through CPU memory. The pass runs at the game's display resolution.
With `[DlssNr] EstimateMotion=true`, the provider estimates backward motion
from consecutive pre-NR frames on the GPU and uses confidence-gated temporal
history. This is optical flow, not the engine's velocity texture: the Present
boundary does not expose that texture. The first frame after enabling the pass,
switching the option, or changing resolution starts a new history. The menu
has an "Estimate motion for temporal NR" checkbox for A/B testing.
The game's HUD is part of the processed image; OptiScaler's own overlay is
drawn afterward. Other formats and Vulkan are not supported by this hook.
The RGB10A2 path packs and unpacks pixels in SYCL GPU kernels, with no CPU
pixel staging.
For Tekken 7, the hook matches `TekkenGame-Win64-Shipping.exe`; the game's
launcher is not the render process. Use an offline mode for visual A/B testing.
Tekken 7's observed backbuffer was RGB10A2 at 1920x1080. On Arc Pro B70,
the experimental MegaWindow32 build ran through the whole Present path at
about 102 ms per warmed frame, including about 91 ms of model execution.
At 1280x720 a standalone RGB10A2 provider test measured 41.8 ms with
MegaWindow32 and 38.4 ms without it; this fused prototype is slower on
that input and remains experimental.
Tekken's live 1280x720 Present path measured about 45 ms per warmed frame,
with about 40 ms in the provider. A resolution change recreates the model
session while keeping the provider DLL and its selected Level Zero device
loaded; the live 1920x1080 to 1280x720 transition completed successfully.
The menu reports Present-path errors directly and provides Retry.
On the Arc Pro B70 1280x720 GPU smoke test, estimated motion added about
0.8-1.0 ms per warmed frame over supplied zero vectors. A static input gave
bit-identical output to supplied zero vectors. With a 4-pixel synthetic
shift, estimated and supplied-vector outputs differed by 0.00074 mean
absolute RGB (normalized 0-1), with no sample over 0.05 in the tested
interior. This does not replace a moving-scene visual check in Tekken.
On Arc Pro B70 the live 960x540 Valheim menu path ran without SYCL errors;
29 warmed samples had a median of 33.52 ms for the whole pass and 25.4 ms
for model execution. Scene quality and gameplay still need visual checking.
The provider now retains its previous output on device and uses motion
vectors to reproject it when the next frame index is consecutive and the
game did not request a reset. The bridge copies the motion texture on the
producer list, waits for the same D3D12 fence as color, and normalizes the
vectors on the SYCL GPU. Missing or unsupported motion falls back to an
independent frame. The Wukong game path still needs a moving-scene check;
low-resolution motion currently uses bilinear upsampling without the
depth-guided dilation used by XeSS. Failures are logged and do not silently
fall back to NVIDIA.
The first valid motion texture is logged with format, dimensions and scales;
`NR-PROFILE ... temporal=1 motion=1` confirms that a warmed RG16F frame used
history. A reset or a discontinuous frame index must report `temporal=0`.

Native D3D12 games supply a game-owned open list. OptiScaler cannot close or
reset that list. An opt-in `NativeAfterSubmit` path records the upscaler's
output/depth/motion resources, waits for the game to submit the producing
list, and then runs encode, SYCL inference, GPU copy and resolve on an
OptiScaler-owned list on the same direct queue. The queue hook splits a batch
at the producer list, so later game lists in that batch run after the resolve.
`NativeOutputState` must name the actual D3D12 resource state at the end of
the producer list. A game that consumes or aliases the output later in the
*same* producer list cannot use this path: a queue hook cannot insert work
among commands already recorded in one list. Wukong ran with
`NativeAfterSubmit=true` and `NativeOutputState=8`; verify the boundary and
state separately for other games.

Both owned-list paths use explicit synchronization: copy the encoded RGBA16F
texture into a shared GPU heap, submit and wait for a D3D12 fence, run
synchronous SYCL inference over imported heap memory, copy the answer from
another shared GPU heap on the same queue, and wait for a second fence.
Standalone tests verified SYCL writes visible to D3D12 and bit-identical
320x320 model output through the shared path on Intel Arc Pro B70.
On Windows, `arc_nr_sycl_provider_gpu_smoke` exercises D3D12 shared heaps,
two explicit queue fences and the provider's `EvaluateGpu` entry point.
It injects a raw test image with an upload buffer and writes a readback file
only to verify the result; the OptiScaler game path uses neither host buffer.

## Performance profile on Arc Pro B70

`NR-PROFILE` logs allocation, producer fence, provider, and output fence times
every 30 native D3D12 frames. The provider's stage breakdown includes heap
import, input copy, model execution, and output copy. At 1920x1080, the
original shared-heap output kernel took about 93 ms and model execution about
570 ms per warmed frame. A SYCL device-to-device `memcpy` for packed rows
reduced output copy to about 0.4 ms. A tiled window-attention kernel in the
SNR runtime reduced model execution to about 350 ms by sharing each 8x8
window's keys and values in GPU local memory. Offline output remained bitwise
identical at 1920x1080, 960x540, and 480x270.

The optimized Wukong log measured provider time of 350-353 ms, including
348-351 ms for the model, with 2-5 ms bridge allocation and 10-14 ms producer
work on sampled frames. The first frame can take several seconds for model
weight preparation and SYCL kernel compilation. Full-resolution inference is
still too expensive for real-time play; most remaining time is window
attention. `SNR_PROFILE_MODEL=1` prints per-stage and top-block model timings
for a standalone provider run.
