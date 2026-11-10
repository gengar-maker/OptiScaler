param(
    [Parameter(Mandatory=$true)][string]$XeSSDll,
    [Parameter(Mandatory=$true)][string]$OptiScalerDll,
    [Parameter(Mandatory=$true)][string]$ProviderDirectory,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string[]]$Modes=@('texture-r8-dynamic','texture-dynamic','ldr-sa-dynamic'),
    [switch]$NoDebug,
    [string]$MSBuild='C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference='Stop'
$env:SNR_TEST_NO_DEBUG=if($NoDebug){'1'}else{$null}
$root=[IO.Path]::GetFullPath($WorkDirectory)
$sdk=(Resolve-Path -LiteralPath $XeSSDll).Path
$dll=(Resolve-Path -LiteralPath $OptiScalerDll).Path
$provider=(Resolve-Path -LiteralPath $ProviderDirectory).Path
$driver=Join-Path $root 'driver'
New-Item -ItemType Directory -Path $driver -Force | Out-Null
& $MSBuild (Join-Path $PSScriptRoot 'integration-smoke.vcxproj') /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$driver\" "/p:IntDir=$root\obj\" /m:2 /v:minimal
if($LASTEXITCODE){throw 'Integration driver build failed'}
$ini=@'
[Inputs]
EnableXeSSInputs=true
EnableDlssInputs=false
EnableFsr2Inputs=false
EnableFsr3Inputs=false
EnableFfxInputs=false
[Spoofing]
Dxgi=false
[Menu]
OverlayMenu=false
[FrameGen]
Enabled=false
FGInput=nofg
FGOutput=nofg
[Log]
LogLevel=2
LogToFile=true
LogToConsole=false
LogAsync=false
[DlssNr]
NativeXeSS=true
Enabled=true
Backend=1
NativeAfterSubmit=false
Style=1
AutoMask=false
ColourStrength=1
TransferStrength=1
PreserveBaseTone=false
'@
function Diagnostics($file) {
    @(Get-Content -LiteralPath $file | Where-Object {$_ -match '^D3D12-DIAGNOSTIC'} |
      Group-Object | ForEach-Object {"$($_.Count) $($_.Name)"} | Sort-Object)
}
$results=@()
foreach($mode in $Modes) {
    if($mode -notin @('texture-r8-dynamic','texture-dynamic','ldr-sa-dynamic')) {throw "Unknown test mode: $mode"}
    $folder=Join-Path $root $mode
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    foreach($variant in @('baseline','hidden','applied')) {
        $target=Join-Path $folder $variant
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        $exe=Join-Path $target 'xess_nr_integration_smoke.exe'
        Copy-Item -LiteralPath (Join-Path $driver 'xess_nr_integration_smoke.exe') -Destination $exe
        if($variant -eq 'baseline') {
            if(Test-Path -LiteralPath (Join-Path $target 'dxgi.dll')) {throw 'Original SDK baseline contains proxy DLL'}
            $env:SNR_TEST_WARMUP=$null
        } else {
            Copy-Item -LiteralPath $dll -Destination (Join-Path $target 'dxgi.dll')
            Copy-Item -LiteralPath (Join-Path $provider 'arc_nr_sycl.dll') -Destination $target
            $link=Join-Path $target 'snr_openvino'
            if(!(Test-Path -LiteralPath $link)) {New-Item -ItemType Junction -Path $link -Target (Join-Path $provider 'snr_openvino') | Out-Null}
            $apply=if($variant -eq 'applied'){'true'}else{'false'}
            ($ini+"`nApplyModel=$apply`n") | Set-Content -LiteralPath (Join-Path $target 'OptiScaler.ini') -Encoding ascii
            $env:SNR_TEST_WARMUP='1'
        }
        & $exe $sdk (Join-Path $folder "$variant.bin") $mode *> (Join-Path $folder "$variant.txt")
        if($LASTEXITCODE){throw "$mode/$variant failed: $LASTEXITCODE"}
        if($variant -ne 'baseline') {
            $log=Get-Content -LiteralPath (Join-Path $target 'OptiScaler.log') -Raw
            if($log -notmatch 'XESS-NR frame=.*neural=true' -or $log -notmatch 'XESS-NR frame=.*temporal=1' -or $log -match '\[E\]') {throw "$mode/$variant full temporal SNR missing or failed"}
        }
        Write-Output "Completed $mode/$variant"
    }
    $diagnostics=Diagnostics (Join-Path $folder 'baseline.txt')
    $hashes=@{}
    foreach($variant in @('baseline','hidden','applied')) {
        if($variant -ne 'baseline' -and ((Diagnostics (Join-Path $folder "$variant.txt")) -join "`n") -ne ($diagnostics -join "`n")) {throw "$mode/$variant changed D3D12 diagnostics or their multiplicities"}
        $bytes=[IO.File]::ReadAllBytes((Join-Path $folder "$variant.bin.frames.bin"))
        $frameSize=320*320*8
        if($bytes.Length -ne 12*$frameSize){throw 'Incomplete sequence'}
        $sha=[Security.Cryptography.SHA256]::Create()
        $hashes[$variant]=@(for($i=0;$i -lt 12;$i++){[Convert]::ToHexString($sha.ComputeHash($bytes,$i*$frameSize,$frameSize))})
        $sha.Dispose()
        if(@($hashes[$variant]|Sort-Object -Unique).Count -ne 12){throw "$mode/$variant repeated frames"}
    }
    for($i=0;$i -lt 12;$i++) {
        if($hashes.baseline[$i] -ne $hashes.hidden[$i]){throw "$mode hidden edit changed frame $i"}
        if($i -gt 0 -and $hashes.hidden[$i] -eq $hashes.applied[$i]){throw "$mode model edit missing in prepared frame $i"}
    }
    $hashes|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $folder 'sequence-hashes.json')
    $results+=[pscustomobject]@{mode=$mode;frames=12;hiddenBitExact=$true;appliedVisible=$true;temporal=$true;diagnosticMultiplicitiesMatch=$true}
    $results|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $root 'results.json')
    Write-Output "PASS ${mode}: 12 distinct frames; hidden bit exact; applied full-model edit visible; no additional D3D12 diagnostics"
}
