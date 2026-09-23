param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$SnrRoot,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$ModelDll,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$OutputRoot,
    [string]$DnnlIncludeDir = "",
    [string]$DnnlLibrary = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Run([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with code $LASTEXITCODE"
    }
}

$snr = (Resolve-Path -LiteralPath $SnrRoot).Path
$dll = (Resolve-Path -LiteralPath $ModelDll).Path
if (-not (Test-Path -LiteralPath (Join-Path $snr "runtime\CMakeLists.txt") -PathType Leaf)) {
    throw "SnrRoot must be the SNR sycl-native-runtime repository root"
}
if ((git -C $snr branch --show-current) -ne "sycl-native-runtime") {
    throw "SNR must be checked out on sycl-native-runtime"
}
if (Test-Path -LiteralPath $OutputRoot) {
    throw "OutputRoot already exists; choose a NEW private directory"
}
foreach ($tool in @("git", "python", "cmake", "ninja", "icx")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool is not on PATH; use an Intel oneAPI command prompt"
    }
}
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$output = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $output | Out-Null
$mlx = Join-Path $output "MLX-DLSS"
$venv = Join-Path $output "venv"
$extract = Join-Path $output "extract"
$weights = Join-Path $output "arc_nr_weights"
$build = Join-Path $output "build-provider"
$inputFrame = Join-Path $output "smoke-input.rgba16f"
$outputFrame = Join-Path $output "smoke-output.rgba16f"

Run "git" @("clone", "https://github.com/iamwavecut/MLX-DLSS.git", $mlx)
Run "git" @("-C", $mlx, "checkout", "0ca2deab092fe6f3e331bf4f616271dbc64521d0")
Run "python" @("-m", "venv", $venv)
$python = Join-Path $venv "Scripts\python.exe"
Run $python @("-m", "pip", "install", (Join-Path $mlx "python"), "numpy")
$weightTool = Join-Path $venv "Scripts\mlxdlss-weights.exe"
Run $weightTool @("sha256", $dll)
Run $weightTool @("all", $dll, $extract)
$logicalTensor = Join-Path $extract "dlssnr-weights-logical.safetensors"
if (-not (Test-Path -LiteralPath $logicalTensor -PathType Leaf)) {
    throw "MLX-DLSS did not produce $logicalTensor; inspect the extraction output"
}
$logicalHash = (Get-FileHash -LiteralPath $logicalTensor -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedLogicalHash = "66348ca3319b36beebc4ccb27c9ec8e90a41d87ce00ff8e6eb0d9c886784cdbe"
Write-Host "Logical model SHA-256: $logicalHash"
if ($logicalHash -ne $expectedLogicalHash) {
    throw "The game DLL decoded to a different model; this SYCL graph/weight contract is pinned to $expectedLogicalHash"
}
Push-Location $snr
try {
    Run $python @("-m", "model_reference.mlx_dlss.prepare_runtime_weights", $logicalTensor, $weights)
} finally {
    Pop-Location
}

$cmakeArgs = @("-S", $source, "-B", $build, "-G", "Ninja",
               "-DCMAKE_CXX_COMPILER=icx", "-DSNR_RUNTIME_DIR=$(Join-Path $snr 'runtime')")
if ($DnnlIncludeDir) { $cmakeArgs += "-DDNNL_INCLUDE_DIR=$DnnlIncludeDir" }
if ($DnnlLibrary) { $cmakeArgs += "-DDNNL_LIBRARY=$DnnlLibrary" }
Run "cmake" $cmakeArgs
Run "cmake" @("--build", $build, "--target", "arc_nr_sycl_provider_smoke")
Run $python @((Join-Path $PSScriptRoot "make_smoke_frame.py"), $inputFrame)
$smoke = Join-Path $build "arc_nr_sycl_provider_smoke.exe"
Run $smoke @((Join-Path $weights "logical"), (Join-Path $weights "derived"),
             "320", "320", $inputFrame, $outputFrame)
Run $python @((Join-Path $PSScriptRoot "check_smoke_frame.py"), $outputFrame)
Write-Host "SYCL provider smoke test passed. Private weights: $weights"
Write-Host "Provider DLL: $(Join-Path $build 'arc_nr_sycl.dll')"
