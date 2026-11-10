# Native SNR / XeSS checkpoint, 2026-11-10

The source checkpoint includes the separate SNR controls page, corrected live
conditioning, native XeLL ownership during frame generation, and support for
texture exposure / ordinary shader-readable D3D12 resources before native XeSS.
The complete FP16 OpenVINO model and weights are unchanged.

`native-input-contract.json` records full-model tests with changing R8 and R32
exposure, ordinary resources, and the existing scalar/simultaneous-access path.
Each path compares 12 changing consumed frames against the original XeSS SDK:
hidden edit is byte-exact, applied edit is visible, and history runs. Original
SDK diagnostics are retained and their complete normalized multisets match.
The new adapter's independent GPU smoke is validation-clean.

`native-frame-generation.json` records the old XeLL blocking regression, SDK
on/off/on tests, and Witcher PresentMon observations. Physical presentation was
measured; stable 60 FPS was not achieved or claimed. Those captures were not a
controlled camera/scene A/B benchmark.

The first Cyberpunk launch used an unsupported input contract and bypassed SNR.
The corrected DLL is installed and passes standalone tests in debug and release
driver modes. **A repeat live Cyberpunk scene check is still pending.** SDK tests
do not establish complete game compatibility or subjective image quality.

Reproduce with `tests/snr-controls/smoke.vcxproj`,
`tests/xess-nr-input/smoke.vcxproj`,
`tests/xess-nr-input/native-input-contract.ps1`, and
`tests/native-xefg/run.ps1`. Their READMEs describe the required provider and SDK
paths. Graph/profile preparation and original-colour fallback remain asynchronous
and GPU-only in production; CPU image readback is limited to explicit tests.

DLLs, model weights, compiler caches, raw captures and profiler data are excluded
from this checkpoint. The compact reports identify tested binaries by SHA256.
