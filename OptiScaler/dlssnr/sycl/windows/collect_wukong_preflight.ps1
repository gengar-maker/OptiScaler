param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$GameRoot,
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$OutputJson
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path -LiteralPath $GameRoot).Path
$win64 = Join-Path $root "b1\Binaries\Win64"
if (-not (Test-Path -LiteralPath $win64 -PathType Container)) {
    $win64 = $root
}
if (Test-Path -LiteralPath $OutputJson) {
    throw "OutputJson already exists; choose a new file to preserve the previous report"
}
$outputParent = Split-Path -Path ([System.IO.Path]::GetFullPath($OutputJson)) -Parent
if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
    New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
}

$candidateNames = @(
    "OptiScaler.dll", "nvngx.dll", "nvngx_dlss.dll", "nvngx_dlssnr.dll",
    "nvngx.dll_dlssnr.dll", "dxgi.dll", "d3d12.dll", "version.dll",
    "winmm.dll", "d3d9.dll"
)
$files = @(
    Get-ChildItem -LiteralPath $win64 -File |
        Where-Object { $candidateNames -contains $_.Name } |
        Sort-Object Name |
        ForEach-Object {
            [pscustomobject]@{
                name = $_.Name
                path = $_.FullName
                bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                fileVersion = $_.VersionInfo.FileVersion
                productName = $_.VersionInfo.ProductName
            }
        }
)
$executables = @(
    Get-ChildItem -LiteralPath $win64 -File -Filter "*.exe" |
        Select-Object -ExpandProperty Name
)
$logs = @(
    Get-ChildItem -LiteralPath $win64 -File |
        Where-Object { $_.Name -match 'OptiScaler|dlssnr' -and $_.Extension -in @('.log', '.ini') } |
        Select-Object -ExpandProperty FullName
)
try {
    $gpus = @(Get-CimInstance Win32_VideoController | Select-Object -ExpandProperty Name)
} catch {
    $gpus = @("GPU inventory unavailable: $($_.Exception.Message)")
}
$report = [ordered]@{
    schema = "arc-nr-wukong-preflight-v1"
    gameDirectory = $win64
    executables = $executables
    graphicsAdapters = $gpus
    installedFiles = $files
    logAndIniPaths = $logs
    intelCompilerOnPath = [bool](Get-Command icx -ErrorAction SilentlyContinue)
}
$json = $report | ConvertTo-Json -Depth 6
$json | Set-Content -LiteralPath $OutputJson -Encoding UTF8
Write-Host "Wrote $OutputJson"
Write-Host "Review paths before sharing; this report contains hashes and metadata, not model weights."
