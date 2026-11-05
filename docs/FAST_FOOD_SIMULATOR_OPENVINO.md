# Fast Food Simulator — OpenVINO SNR test

Executable: `ProjectBakery/Binaries/Win64/ProjectBakery-Win64-Shipping.exe`.
The installed Unreal package includes SM5 and SM6 shader data. The initial
test uses D3D11 (`DefaultGraphicsRHI_DX11`), preserving the requested feature
level and real GPU identity. Existing saves are not modified.

The file `OptiScaler.snr-present.enable` opts this game into the generic
post-tonemap D3D11 Present path. It does **not** enable Tekken shader hooks.

1. Copy the finished SDR game frame into shared D3D11/D3D12 resources.
2. Wait for its producer fence and import GPU buffers into the native provider.
3. Estimate motion on the GPU, reject unreliable history and prepare SNR input.
4. Run the full FP16 OpenVINO model with custom SYCL kernels on Arc Pro B70.
5. Compose the result, preserve alpha, copy it back and draw the OptiScaler UI.

This is a first integration test: game HUD is included, motion is estimated
from completed images, and no engine depth or pre-tonemap color is captured.
It is not the engine-integrated Tekken SNR→XeSS pipeline. XeSS is not enabled
by this installation. The game's own upscaling/AA remains under its control.

Initial settings: NR enabled, ApplyModel enabled, motion estimation enabled,
ColourStrength=0 to preserve source hue, TransferStrength=1. SNR operates at
the actual backbuffer resolution. Unsupported profile sizes are prepared in
the background using the configured full-FP16 ONNX profile builder.

`Insert` opens OptiScaler. Compare the same scene with Enable Neural Rendering
off/on, including motion, fine text, UI and bright/dark surfaces. The SNR
model time and synchronized pass time are separate readings.

Rollback: close the game, remove only the files listed in the installation
manifest and restore the saved `Engine.ini`. Do not remove or replace the
game's original TBB/OpenImageDenoise libraries.

## Local validation — 2026-11-05

Installed on Fast Food Simulator Early Access 0.5.2 (Unreal 5.3).
The live log confirms a D3D11 swapchain on Intel Arc Pro B70, the generic
Present marker, OpenVINO full FP16, 352 native SYCL operations and successful
inference. The 1080p graph uses 1920×1088 padding without resizing the image.
The switch from 1920×1080 to 1280×720 rebuilt the session asynchronously and
resumed inference. Temporal history is active with estimated motion; the
`motion=0` log field means there is no engine-provided motion texture.

Warm medians from periodic samples at frame 60 and later:

| Display size | Model | Provider | Synchronized Present SNR pass | Model samples |
| --- | ---: | ---: | ---: | ---: |
| 1920×1080 | 41.19 ms | 43.85 ms | 61.99 ms | 18 |
| 1280×720 | 16.74 ms | 18.29 ms | 22.19 ms | 29 |

These are initial live samples, not a controlled gameplay benchmark. The
Present measurement includes waiting for the game's producer work and must
not be interpreted as isolated model cost or used to promise game FPS.
There were no error-level log entries in this captured run. Visual quality
has not yet been confirmed: the window screenshot tool timed out, and an
in-game off/on comparison was requested from the user.

The original game DLLs passed SHA-256 preservation checks. A dependency probe
also loaded the provider with the original game TBB/OpenImageDenoise already
loaded; all 40 required TBB import references across OpenVINO and its GPU
plugin were present. The live game used the packaged TBB runtime.

Local installation and evidence:

- `K:\Codex\SNR-OptiScaler\work\fastfood-openvino-20261105\installation.json`
- `original\Engine.ini`, `original\GameUserSettings.ini`, `original\game-files.json`
- `dependencies.log`, `loaded-modules.json`
- `live-optiscaler.log`, `live-openvino.log`, `profile-summary.json`
- `snr_openvino\` contains the runtime and profiles on K:, linked into the
  game directory with a directory junction.

For a quick reversible disable, close the game and rename its new `dxgi.dll`
to `dxgi.dll.disabled`. Restore the backed-up `Engine.ini` as well to remove
the D3D11 override. The installation did not alter save files or the game's
original DLLs.

## Saturated-colour correction — 2026-11-05

The first user recording exposed red upholstery becoming locally too bright.
A 40-frame offline GPU replay reproduced this. The provider now keeps neural
history before presentation colour/transfer controls, and the SDR compositor
computes luminance in linear light with a common gamut-limited gain.
The full FP16 model is unchanged; the default full-colour output is bitwise
identical over the same 40-frame replay. RGB10A2 transfer-zero bypass also
preserves all input bits. Corrected replay cost is 34.18 ms versus 34.15 ms
before at 1600×900. Moving-scene quality needs a new in-game comparison.

Implementation and detailed evidence are documented in the SNR repository:
`model_tools/openvino_backend/native/VALIDATION_20261105_FASTFOOD_COLOUR.md`.

## Broad material darkening — second recording, 2026-11-05

Update: the user rejected this presentation policy as removing intended SNR
colour/lighting. It is now disabled (`PreserveBaseTone=false`) and full neural
colour restored (`ColourStrength=1`, `TransferStrength=1`). The following
description records the experiment, not a proven fix or current setting.

The car still darkens with ColourStrength=0 because that setting preserves hue,
not model-predicted luminance. A standard OpenVINO GPU execution without custom
kernels reproduces the darkening (native-vs-standard raw-head relative L2
0.00107509). LocalTone=0 only partially reduces it. This is intrinsic to the
current reconstructed model pipeline, not evidence of an RGB channel swap.

Added an opt-in **Preserve base colour and lighting (SDR)** menu option,
`[DlssNr] PreserveBaseTone=true`. It is enabled for this installation; the
global default is false. It removes the broad component of the display edit
using the existing MLX detail/colour split and, at ColourStrength=0, preserves
source chromaticity in linear light. Full FP16 inference and raw temporal
history remain unchanged. The external HDR compositor does not use it.

On the replayed car frame, mean hood luminance is source 0.39255, old SNR
0.12641, new presentation 0.36211. Disabling the option is bitwise identical
to the preceding display path. B70 unit checks, 40-frame car/red-upholstery
replays and exact RGB10A2 bypass passed. The policy adds ~0.39 ms provider
latency at 720p in offline tests; motion quality still needs in-game review.

The purple close-up contour occurs with the game's interaction prompt before
the first ApplyModel on-toggle. It appears to be a game highlight; a close-up
with NR fully off was requested rather than removing input game content.

The latest dxgi.dll, provider and INI were installed with SHA-256 verification.
Backups and evidence: `fastfood-openvino-20261105/car-artifact/` on K:.
Full report in SNR:
`model_tools/openvino_backend/native/VALIDATION_20261105_FASTFOOD_BASE_TONE.md`.
