param(
    [Parameter(Mandatory=$true)][string]$XeSSDll,
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string]$MSBuild='C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference='Stop'
$stage=[IO.Path]::GetFullPath($WorkDirectory)
$sdk=(Resolve-Path -LiteralPath $XeSSDll).Path
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach($variant in @('true','false')) {
    & $MSBuild (Join-Path $PSScriptRoot 'smoke.vcxproj') /p:Configuration=Release /p:Platform=x64 /p:Splice=$variant "/p:OutDir=$stage\$variant\" "/p:IntDir=$stage\obj-$variant\" /v:minimal
    if($LASTEXITCODE) { throw "Build failed: $variant" }
}
& $MSBuild (Join-Path $PSScriptRoot 'state-smoke.vcxproj') /p:Configuration=Release /p:Platform=x64 /p:Splice=true "/p:OutDir=$stage\state\" "/p:IntDir=$stage\obj-state\" /v:minimal
if($LASTEXITCODE) { throw 'State test build failed' }
& (Join-Path $stage 'state\splice_state_smoke.exe')
if($LASTEXITCODE) { throw 'State test failed' }
$records=@()
foreach($mode in @('scalar','texture','auto','ldr','scalar-sa','texture-sa','auto-sa','ldr-sa')) {
    foreach($variant in @('false','true')) {
        $output=Join-Path $stage "$variant-$mode.bin"
        & (Join-Path $stage "$variant\splice_smoke.exe") $sdk $output $mode > (Join-Path $stage "$variant-$mode.txt")
        if($LASTEXITCODE) { throw "$variant $mode failed: $LASTEXITCODE" }
    }
    $baseline=(Get-FileHash -LiteralPath (Join-Path $stage "false-$mode.bin")).Hash
    $split=(Get-FileHash -LiteralPath (Join-Path $stage "true-$mode.bin")).Hash
    if($baseline -ne $split) { throw "GPU pixels differ for $mode" }
    $a=Get-Content -LiteralPath (Join-Path $stage "false-$mode.txt")
    $b=Get-Content -LiteralPath (Join-Path $stage "true-$mode.txt")
    if(Compare-Object $a $b) { throw "SDK/debug outputs differ for $mode" }
    $unexpected=@($a | Where-Object {$_ -match '^D3D12-DIAGNOSTIC' -and $_ -notmatch "^D3D12-DIAGNOSTIC 527 .*'XeSS_iN_root_constants(?:_[A-Za-z0-9_]+)?'"})
    if($unexpected.Count) { throw "Unexpected baseline diagnostics in $mode : $unexpected" }
    $diagnostics=@($a | Where-Object {$_ -match '^D3D12-DIAGNOSTIC'}).Count
    $records += [pscustomobject]@{mode=$mode; frames=12; bitwiseEqual=$true; originalSdkDiagnostics=$diagnostics; newDebugErrors=0; sha256=$baseline}
    Write-Output "PASS $mode : bit-identical producer -> XeSS -> consumer; SDK baseline diagnostics=$diagnostics; new errors=0"
}
$records | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'splice-ab-results.json')
