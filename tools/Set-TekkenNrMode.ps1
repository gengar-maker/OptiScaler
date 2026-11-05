[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Native', 'XeSSBeforeSNR', 'SNRBeforeXeSS')]
    [string] $Mode,
    [Parameter(Mandatory = $true)]
    [string] $GameDirectory,
    [string] $EngineConfigPath = (Join-Path $env:LOCALAPPDATA 'TekkenGame\Saved\Config\WindowsNoEditor\Engine.ini'),
    [string] $BackupDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (Get-Process -Name '*Tekken*' -ErrorAction SilentlyContinue) {
    throw 'Close Tekken before switching render modes. No files changed.'
}
$gameRoot = (Resolve-Path -LiteralPath $GameDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $gameRoot 'TekkenGame-Win64-Shipping.exe')) -or
    !(Test-Path -LiteralPath (Join-Path $gameRoot 'dxgi.dll'))) {
    throw 'GameDirectory must be the Tekken Win64 folder containing the game EXE and OptiScaler dxgi.dll.'
}
$enginePath = (Resolve-Path -LiteralPath $EngineConfigPath).Path
if (!$BackupDirectory) { $BackupDirectory = Join-Path $gameRoot 'OptiScaler-mode-backups' }
$snapshot = Join-Path $BackupDirectory (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $snapshot -Force | Out-Null
Copy-Item -LiteralPath $enginePath -Destination (Join-Path $snapshot 'Engine.ini')
$markers = @('OptiScaler.snr-xess.enable', 'OptiScaler.snr-xess.native.enable',
    'OptiScaler.snr-xess.hdr.enable', 'OptiScaler.xess-snr.enable', 'OptiScaler.snr-native.enable')
$previous = @{}
foreach ($marker in $markers) {
    $path = Join-Path $gameRoot $marker
    $previous[$marker] = Test-Path -LiteralPath $path
    if ($previous[$marker]) { Copy-Item -LiteralPath $path -Destination (Join-Path $snapshot $marker) }
}
$previous | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $snapshot 'markers-before.json')

$percentage = if ($Mode -eq 'Native') { 100 } else { 50 }
$lines = [System.Collections.Generic.List[string]]::new()
$inSystem = $false
$foundSystem = $false
$wroteValue = $false
foreach ($line in [IO.File]::ReadAllLines($enginePath)) {
    if ($line -match '^\s*\[([^]]+)\]\s*$') {
        if ($inSystem -and !$wroteValue) {
            $lines.Add("r.ScreenPercentage=$percentage")
            $wroteValue = $true
        }
        $inSystem = $Matches[1] -ieq 'SystemSettings'
        $foundSystem = $foundSystem -or $inSystem
    }
    if ($inSystem -and $line -match '^\s*r\.ScreenPercentage\s*=') {
        if (!$wroteValue) { $lines.Add("r.ScreenPercentage=$percentage"); $wroteValue = $true }
    } else { $lines.Add($line) }
}
if (!$foundSystem) { $lines.Add(''); $lines.Add('[SystemSettings]') }
if (!$wroteValue) { $lines.Add("r.ScreenPercentage=$percentage") }

try {
    [IO.File]::WriteAllLines($enginePath, $lines, [Text.UTF8Encoding]::new($false))
    foreach ($marker in $markers) {
        $enabled = switch ($marker) {
            'OptiScaler.xess-snr.enable' { $Mode -eq 'XeSSBeforeSNR' }
            'OptiScaler.snr-native.enable' { $Mode -eq 'Native' }
            default { $true }
        }
        $path = Join-Path $gameRoot $marker
        if ($enabled) { [IO.File]::WriteAllText($path, "Render mode: $Mode`r`n") }
        elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
} catch {
    Copy-Item -LiteralPath (Join-Path $snapshot 'Engine.ini') -Destination $enginePath -Force
    foreach ($marker in $markers) {
        $path = Join-Path $gameRoot $marker
        if ($previous[$marker]) { Copy-Item -LiteralPath (Join-Path $snapshot $marker) -Destination $path -Force }
        elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    throw
}
[pscustomobject]@{ Mode = $Mode; ScreenPercentage = $percentage; GameDirectory = $gameRoot;
    EngineConfig = $enginePath; Backup = $snapshot; RequiresRestart = $true }
