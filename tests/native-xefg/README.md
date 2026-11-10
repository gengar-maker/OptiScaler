# Native XeSS frame generation regression

The native XeSS path must preserve the game's XeLL context, enable state and frame IDs.
OptiScaler's translated Reflex backend previously created its own XeLL context and enabled
`XellHooks::blockExternalContexts(true)`. A subsequent game `xellSetSleepMode(enabled=1)`
returned success without executing the SDK call. `xellGetSleepMode` was similarly suppressed.
Native XeSS FG therefore did not have a functioning low-latency input.

The fix applies when `DlssNr.NativeXeSS=true` and OptiScaler's FG output is `nofg`:

* Native XeLL calls reach the original SDK.
* Translated latency yields to an observed game XeLL context; it does not create or retain a
  competing driver pacing context. Both the shipping and optional low-latency input paths
  follow this ownership rule.
* Successful context destruction clears the tracked pointer instead of re-registering the
  destroyed context.
* Native XeSS FG calls and periodic previous-Present status are logged as `XEFG-NATIVE`.
  Original resources, constants, frame IDs, enable state and SDK return codes are preserved.

This is native **SNR â†’ XeSS SR â†’ game post-processing â†’ XeSS FG**, not OptiScaler injecting
a second frame generator. OptiScaler's `[FrameGen]` can remain disabled.

## Run on Windows / Intel Arc

Build OptiScaler first, then run from PowerShell:

```powershell
& .\tests\native-xefg\run.ps1 `
    -GameDirectory 'E:\SteamLibrary\steamapps\common\The Witcher 3\bin\x64_dx12' `
    -OutDirectory 'K:\Codex\validation\native-xefg'
```

The script builds the test and uses SDK binaries copied from the specified game. It never
changes the installed game. Four isolated runs cover the original SDK, native proxy without
Reflex, native proxy with repeated Reflex calls, and the non-native proxy path. An optional
`-ReferenceDll` verifies the pre-fix failure at the exact XeLL state assertion.

The GPU test creates a real windowed D3D12 swap chain, supplies depth, motion and frame
constants, sends matching XeLL markers and switches FG on/off/on over 72 rendered frames.
It asserts the SDK-reported enabled state, successful interpolation and two frames sent for
presentation during active FG; all 24 disabled frames must send one. First-frame history
warm-up is allowed. This verifies SDK operation, not physical display scanout or game image
quality. Live game validation is still needed for those.

## B70 evidence, 2026-11-10

Original SDK: 46 successful interpolated presents, 24 disabled presents.
Pre-fix proxy with Reflex: `requested=1 actual=0`, regression reproduced.
Fixed proxy with Reflex: `requested=1 actual=1`, 46 successful interpolated presents and
24 disabled presents; translated context was released. DLLs: game XeSS FG 1.2.2.118.

Detailed logs and the pre-fix diagnostic DLL are outside Git in
`K:\Codex\SNR-OptiScaler\work\witcher-native-fg-20261110`.

## Live follow-up

The Witcher live log subsequently confirmed full recurrent SNR and native FG.
PresentMon frame events confirmed that both application frames and XeFG frames
reached presentation. At 1280x720 SNR / 1920x1080 output, observed displayed FPS
was 51.3 with VSync and 48.9–55.3 in two VSync-off samples. VSync-off display-time
p95 was 18.9–21.9 ms versus 33.4 ms with VSync. The camera and ApplyModel toggles
were not controlled: this is not a controlled FPS improvement claim or stable
60-FPS validation. See `docs/validation/native-xess-snr-20261110` for compact data.
