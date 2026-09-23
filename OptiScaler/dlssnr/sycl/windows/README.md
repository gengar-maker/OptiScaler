# Windows SYCL provider test (native D3D12 integration gate)

This package gives you a **runnable Windows test of `arc_nr_sycl.dll`**, using
the exact `nvngx_dlssnr.dll` you supply. It does **not** enable the SYCL backend
inside a native D3D12 game yet. `DlssNr_Dx12::Dispatch` still records encode,
model evaluation and resolve in the game's open command list. Submitting or
resetting that list from OptiScaler would violate game ownership. The
OptiScaler-owned-list bridge is for D3D11/Vulkan translation paths only.

## Prerequisites

- Windows x64, an Intel GPU with a working Level Zero SYCL driver, Visual
  Studio 2022 with the C++ desktop workload, Intel oneAPI Toolkit 2026.1
  (compiler and oneDNN GPU/SYCL development library), CMake 3.27+ and Ninja.
- Python 3.10+ and Git on `PATH`.
- The **exact** `nvngx_dlssnr.dll` that the target game loads. Keep it and the
  extracted weights outside either Git checkout.
- Enough free space for the 165 MB model DLL, private extraction, hundreds of
  weight files, a Python venv and a full optimized C++ build.

Get the two source branches in separate directories:

```powershell
git clone --branch sycl-native-backend --recurse-submodules https://github.com/gengar-maker/OptiScaler.git C:\src\OptiScaler-SYCL
git clone --branch sycl-native-runtime https://github.com/gengar-maker/SNR.git C:\src\SNR-SYCL
```

Open an **Intel oneAPI command prompt for x64**, so `icx`, the Visual Studio
toolchain and oneAPI DLLs are on `PATH`. In that prompt run this single line:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File C:\src\OptiScaler-SYCL\OptiScaler\dlssnr\sycl\windows\test_provider.ps1 -SnrRoot C:\src\SNR-SYCL -ModelDll C:\path\to\game\nvngx_dlssnr.dll -OutputRoot C:\arc-nr-private-test
```

`OutputRoot` must be a new directory. The script pins MLX-DLSS to commit
`0ca2deab`, creates a private Python venv, extracts the DLL's logical
safetensors, validates and exports all 649 logical tensors, prepares 98
derived tensors, builds `arc_nr_sycl.dll`, and evaluates a deterministic
320×320 RGBA16F image. Success prints finite, non-black RGB statistics and
leaves the input/output raw frames and provider DLL under `OutputRoot`.
The script fails instead of silently falling back to NVIDIA.
It also requires the decoded logical-weight SHA-256 to match the model used
for the SYCL graph; a different game DLL will stop with a clear error instead
of being run with an incompatible model.

If CMake cannot locate oneDNN, pass the actual SYCL-enabled oneDNN install
paths as `-DnnlIncludeDir C:\...\include` and
`-DnnlLibrary C:\...\lib\dnnl.lib`. A CPU-only oneDNN build is insufficient.
If the Intel GPU is absent or only another SYCL backend is available, provider
creation will fail with `no Intel Level Zero GPU is available`.

Send back the full console output, the SHA-256 printed for the DLL and
`OutputRoot\arc_nr_weights\manifest.json`. Do **not** upload the private
weight files unless you intend to share the extracted model. The two
`smoke-*.rgba16f` files are safe test images and useful for visual comparison.

For a Black Myth: Wukong installation made by DLSS5 Swapper, collect the
actual installed filenames and hashes before replacing anything:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File C:\src\OptiScaler-SYCL\OptiScaler\dlssnr\sycl\windows\collect_wukong_preflight.ps1 -GameRoot "C:\path\to\BlackMythWukong" -OutputJson "C:\arc-nr-private-test\wukong-preflight.json"
```

The game root may also be its `b1\Binaries\Win64` directory. Review the JSON
before sharing it: it includes local file paths, filenames, sizes and hashes,
but no model binaries or weight bytes. Do not overwrite the Swapper-installed
OptiScaler files with this branch yet; the native D3D12 SYCL dispatch is not
wired and the two package layouts may differ.

## Native D3D12 in-game gate

The provider test above establishes only that the model DLL and weights run
on your Intel GPU. To safely run it *in the same frame* inside your native
D3D12 game, we still need a proven host-controlled boundary after the game's
upscaler work has been submitted, before its UI/final output is recorded, and
the exact output resource state at that boundary. The existing
`ExecuteCommandLists` hook sees submission, but by itself cannot separate
commands already recorded together in the game-owned list. No menu toggle or
DLL swap in this branch claims to solve that scheduling problem.
