# SNR settings and live controls

Open OptiScaler with its configured menu key (normally Insert), then select the
**SNR** tab beside **OptiScaler**. The page scrolls independently and contains
the enable/apply switches, status, SNR pass/model times and all NR settings.
The main OptiScaler tab retains upscaler/FG and general settings. The existing
Save settings action remains below both tabs and persists the same INI keys.

## Live image controls

With the SYCL/OpenVINO backend (`DlssNr.Backend=1`), Style, Intensity, Local tone,
Local structure, Skin structure, automatic conditioning, Detail strength and
Colour strength update as the slider moves. They do not recreate/compile the
model. Changing model conditioning resets its temporal history; the next
unchanged frame resumes it. Presentation strengths do not feed into raw history.

The NVIDIA backend retains release-to-apply model sliders because its recovered
host contract reads those settings at feature creation. Its model presets A/B/C
are shown only for that backend. They are not SNR Style profiles.

| Reference profile | Style index | Local tone | Local structure | Intensity | Auto mask |
|---|---:|---:|---:|---:|---|
| Standard | 0 | 1 | 1 | 1 | off |
| Natural | 1 | 1 | 1 | 1 | off |
| Cinematic | 2 | 1 | 1 | 1 | off |
| Neutral (diagnostic) | 0 | 0 | 0 | 1 | off |

All profiles retain the same model/weights, and set skin strength to -1. Neutral
is not a model bypass. Individual changes select Custom. Reset image controls
restores Standard plus Detail/Colour strength 1. Skin strength is editable when
automatic conditioning is enabled; -1 follows local structure. This recovered
conditioning is not a new external segmentation model.

The shared `SnrControls.h` follows the pinned MLX-DLSS 0ca2dea reference:
`normalized_style = style_index / 128`, and AutoMask off means **both** skin and
automatic structure selectors are -1. Zero automatic strength is not absence of
the automatic-mask configuration. These corrections apply to the native XeSS,
D3D11/Tekken and generic D3D12 SNR callers. Existing INI keys are retained.

## Pipeline and HDR settings

The advanced section holds working resolution, HDR and comparison tools for
paths that support them. Working resolution applies after releasing its slider
because a different shape requires model preparation. On the native XeSS path,
the game supplies input resolution and exposure; change resolution/quality in
the game. The page explains this instead of offering unrelated HDR controls.

## Validation, 2026-11-06

- CPU regression checks reference style indices 0/1/2/64, auto-mask absence,
  independent skin strength, unrelated frame-field preservation and every
  history-invalidating model control.
- Full FP16 OpenVINO on B70, captured 1280×720 Witcher HDR: 19 frames exercise
  changing and stable control pairs, all six model controls, apply off/on and
  zero intensity. History resets/resumes as expected; provider generation stays
  unchanged, alpha and finite output pass, zero intensity/hidden edit reproduce
  original HDR byte for byte; D3D12 validation passes.
- Full proxy → OpenVINO → original native XeSS → GPU consumer: 12 changing frames
  per variant with Natural and AutoMask off. The log confirms style 0.0078125 and
  skin/automatic -1. Hidden edit matches native XeSS exactly; applied edit and
  temporal history execute, and consumed frames are distinct.

The new DLL is installed in Witcher with a backup of the previous build. Visual
inspection of the new menu and a user live-control check remain pending. Test
artifacts are in `work/snr-live-controls-20261106`. The graph and weights are
unchanged; no attention approximation was introduced.
