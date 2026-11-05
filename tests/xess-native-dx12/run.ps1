param(
    [Parameter(Mandatory=$true)][string]$XeSSDll,
    [Parameter(Mandatory=$true)][string]$OptiScalerDll,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference = 'Stop'
$sdk = (Resolve-Path -LiteralPath $XeSSDll).Path
$dll = (Resolve-Path -LiteralPath $OptiScalerDll).Path
$base = [IO.Path]::GetFullPath($WorkDirectory)
$baseline = Join-Path $base 'baseline'
$hooked = Join-Path $base 'hooked'
New-Item -ItemType Directory -Path $baseline,$hooked -Force | Out-Null
if (Test-Path -LiteralPath (Join-Path $baseline 'dxgi.dll')) { throw 'Baseline directory contains a proxy DLL' }
& $MSBuild (Join-Path $PSScriptRoot 'smoke.vcxproj') /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$baseline\" "/p:IntDir=$base\obj\" /v:minimal
if ($LASTEXITCODE) { throw 'Compilation failed' }
Copy-Item -LiteralPath (Join-Path $baseline 'xess_native_smoke.exe') -Destination $hooked
Copy-Item -LiteralPath $dll -Destination (Join-Path $hooked 'dxgi.dll')
@'
[Inputs]
EnableXeSSInputs=true
EnableDlssInputs=false
EnableFsr2Inputs=false
EnableFsr3Inputs=false
EnableFfxInputs=false
[Menu]
OverlayMenu=false
[Spoofing]
Dxgi=false
[FrameGen]
Enabled=false
[Log]
LogLevel=2
LogToFile=true
LogToConsole=false
LogAsync=false
[DlssNr]
NativeXeSS=true
Enabled=false
NativeAfterSubmit=false
'@ | Set-Content -LiteralPath (Join-Path $hooked 'OptiScaler.ini') -Encoding ascii
$records = @()
foreach ($mode in @('scalar','texture','auto','ldr')) {
    foreach ($variant in @('baseline','hooked')) {
        $exe = Join-Path $base "$variant\xess_native_smoke.exe"
        $output = Join-Path $base "$variant-$mode.bin"
        & $exe $sdk $output $mode > (Join-Path $base "$variant-$mode.txt")
        if ($LASTEXITCODE -ne 0) { throw "$variant $mode failed: $LASTEXITCODE" }
        if ($variant -eq 'hooked') {
            $log = Join-Path $hooked 'OptiScaler.log'
            $text = Get-Content -LiteralPath $log -Raw
            if ($text -notmatch 'XESS-NATIVE frame=7' -or $text -match '\[E\]') { throw "Native routing missing or errors: $mode" }
            Copy-Item -LiteralPath $log -Destination (Join-Path $base "hooked-$mode.log")
        }
    }
    $a = (Get-FileHash -LiteralPath (Join-Path $base "baseline-$mode.bin")).Hash
    $b = (Get-FileHash -LiteralPath (Join-Path $base "hooked-$mode.bin")).Hash
    if ($a -ne $b) { throw "Pixels differ for $mode" }
    $aText = Get-Content -LiteralPath (Join-Path $base "baseline-$mode.txt") | Where-Object { $_ -match '^(query|properties|returnedInit|PASS)' }
    $bText = Get-Content -LiteralPath (Join-Path $base "hooked-$mode.txt") | Where-Object { $_ -match '^(query|properties|returnedInit|PASS)' }
    if (Compare-Object $aText $bText) { throw "SDK query results differ for $mode" }
    $records += [pscustomobject]@{ mode=$mode; framesPerPath=12; sdkQueriesMatch=$true; pixelsBitwiseEqual=$true; sha256=$a }
    Write-Output "PASS $mode : native SDK queries and GPU pixels are identical"
}
[pscustomobject]@{
    sdkPath=$sdk; sdkSha256=(Get-FileHash -LiteralPath $sdk).Hash
    proxyPath=$dll; proxySha256=(Get-FileHash -LiteralPath $dll).Hash
    results=$records
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $base 'ab-results.json')
