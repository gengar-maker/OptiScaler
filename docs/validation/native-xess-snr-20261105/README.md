# Full-FP16 native SNR → XeSS validation, 2026-11-05

These compact reports retain the measured processor A/B results, integration
checks and user-confirmed Witcher session. Model, captures, videos, build products
and temporary installation copies are not required in the source repository.

- `processor-ab.json`: captured HDR 1280×720. Full FP16 graph, first/final model
  outputs byte-identical across variants; warmed processor time about 30–31 ms
  before and 20 ms after same-adapter shared heaps and submission batching.
  The `baseline`, `batched`, `local-heap` runs used D3D12 validation. The later
  96-frame variants used no debug layer; their first 16 frames are excluded.
- `native-xess-integration.json` and `native-xess-sequence-hashes.json`: original
  game XeSS SDK versus proxy, twelve changing 320×320 frames per variant, exact
  hidden edit, applied edit visible, temporal history confirmed. Release-mode
  checks do not claim that the original SDK is D3D12-debug clean.
- `witcher-live.json`: 1130×636 → SNR → XeSS → 1920×1080. Thirty periodically
  logged temporal samples, median model 13.486 ms / SNR pass 15.781 ms. The user
  confirmed the optimized test worked. This is not a controlled game-FPS A/B.
  A subsequent launch replaced the raw game log; the earlier measured summary
  is retained, with that limitation recorded in the report.

See [the implementation and contract](../../native-xess-snr.md) and
`tests/xess-nr-input` for the actual test drivers. A reported zero count of
integration errors covers the checked SNR/fence failure messages only.
