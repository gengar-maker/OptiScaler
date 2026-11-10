# Native SNR → XeSS tests

Build the corresponding `.vcxproj` with MSBuild (`Release|x64`), setting OutDir
and IntDir to an explicit data-drive directory. No driver installs into a game.

| Driver | Inputs and checks |
|---|---|
| `smoke` | GPU HDR adapter, exact bypass, alpha and motion/jitter coordinates |
| `processor-smoke` | Provider directory, capture directory, output directory; full FP16 model, recurrent history/reset and HDR output |
| `integration-smoke` | Original XeSS SDK DLL, output path, mode; real proxy/SDK GPU producer and consumer |

Processor command: `xess_nr_processor_smoke.exe PROVIDER CAPTURE OUTPUT`.
The provider directory contains `arc_nr_sycl.dll` and its `snr_openvino` runtime.
Capture `color.bin` must be a 1280×720 RGBA16F image. The driver uses zero motion
for this static capture. Dynamic integration tests cover nonzero motion/bases.

For a warmed A/B, set `SNR_BENCH_FRAMES=96` and exclude frames 0–15 from timings.
Run without another GPU workload. `SNR_TEST_NO_DEBUG=1` selects release-driver
timing; unset it for D3D12 validation. The test checks finite pixels, exact alpha,
hidden-edit bypass and temporal transitions on every frame, and saves first and
last full-model HDR outputs for byte comparisons. `NR-GPU` reports GPU timestamp
intervals; `NR-BENCH` reports processor/model wall time.

Test-driver environment controls:

| Mode | SNR_BENCH_SERIAL | SNR_BENCH_CROSS_ADAPTER |
|---|---|---|
| Original baseline | 1 | 1 |
| Batched cross-adapter | unset | 1 |
| Optimized native path | unset | unset |

These environment controls are consumed only by the standalone test driver.
The game uses the optimized processor defaults. Source ONNX and weights are
unchanged. See `docs/validation/native-xess-snr-20261105` for saved results.

## Native input contract regression

`native-input-contract.ps1` accepts `-XeSSDll`, `-OptiScalerDll`,
`-ProviderDirectory` and `-WorkDirectory`. The provider directory contains
`arc_nr_sycl.dll` and `snr_openvino`. Default modes exercise ordinary textures
with changing R8 or R32 exposure, and the legacy scalar/SA path. Scalar exposure
is intentionally 123 in texture mode, demonstrating that it must be ignored.
Twelve changing producer frames are consumed after XeSS on the same game list.
Every hidden edit must match the original SDK byte for byte, the full-model edit
must be visible, history must run, and the complete normalized D3D12 diagnostic
multisets (including multiplicities) must match. Original SDK diagnostics are
retained, not filtered. Use `-NoDebug` only for a separate release-driver run.

The adapter smoke also validates ordinary combined-read states and implicit
promotion, GPU R8 exposure conversion, scalar/texture HDR equivalence, alpha,
exact bypass, and stable resolve after the original exposure resource is rewritten.
