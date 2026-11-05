# D3D12 command-list split and input-capture integration tests

These tests exercise the command-list boundary used by the opt-in native
SNR-before-XeSS integration (`DlssNr.NativeXeSS=true`). These boundary tests do
not run the model; full-model processor and proxy tests live in
`tests/xess-nr-input`. See `docs/native-xess-snr.md` for live validation and limits.

Run `run.ps1 -XeSSDll <absolute game SDK DLL> -WorkDirectory <absolute data-drive directory>`.
The runner builds the actual repository implementations and requires Intel Arc,
the Windows D3D12 debug layer, MSVC, and the Windows SDK. Outputs stay in the
explicit work directory. No game installation is modified.

## Checks

- An independent compute workload inherits its PSO/root signature/UAV and
  partially updated constants across the split. Caller arrays are overwritten
  before replay, catching pointer-lifetime errors. Any D3D12 warning/error fails.
- Eight XeSS A/B cases cover scalar exposure, exposure texture, autoexposure,
  and LDR, each with ordinary and simultaneous-access textures. Each path runs
  12 frames with nonzero subrect bases, history resets and changing jitter.
- A producer overwrites the input before the split; a consumer copies the output
  after XeSS on the same application command-list pointer. Output bytes and SDK
  responses must match the unsplit execution exactly.
- Simultaneous-access cases also capture colour/motion/exposure at the submission
  boundary. Every captured colour pixel must match the CPU-known current-frame
  pattern. This prevents silently reading the previous or unsubmitted frame.

The tested Witcher SDK itself emits D3D12 error 527 for named
`XeSS_iN_root_constants` buffers, including `clear_pad_region`. The test stores
all original descriptions and compares normalized descriptions and multiplicity
between baseline and split (only pointer addresses and SDK instance indices are
normalized). Other baseline errors fail. This is **not** a claim that the SDK is
debug-layer clean; the independent state test is clean.

## Limits

The split currently serializes submissions and waits for both prefix and tail.
It is a correctness mechanism, not a throughput benchmark. Prefixes containing
bundles, indirect state mutations, protected sessions, enhanced barriers, open
queries/render passes or split barriers are rejected. A Reset must be observed.
These cases need explicit support before general deployment. Passing these tests
does not validate game-specific resource aliasing, visual quality, SNR colour
normalization, or SNR temporal coordinates.

## Graphics and implementation regressions

`graphics-smoke.vcxproj` covers 18 changing MRT/depth frames, legal RTV/DSV
descriptor recycling, ClearState, partial constants and splice off/on.
`implementation-smoke.vcxproj` creates a separate game-method implementation:
the old probe-only hooks fail on stale pixels; implementation-specific hooks
pass 18 changing frames and wait for complete state coverage after Reset.

Build each project with MSBuild, `Configuration=Release`, `Platform=x64`, and
`OutDir`/`IntDir` pointing to a separate test directory. Run the resulting
`splice_graphics_smoke.exe` and `splice_implementation_smoke.exe`. Run once with
the debug layer and once with `SNR_TEST_NO_DEBUG=1`; neither requires a game.
The original `run.ps1` covers the state and SDK A/B cases above.
