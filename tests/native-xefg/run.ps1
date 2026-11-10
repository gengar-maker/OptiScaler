param(
    [Parameter(Mandatory=$true)][string]$GameDirectory,
    [Parameter(Mandatory=$true)][string]$OutDirectory,
    [string]$CandidateDll,
    [string]$ReferenceDll,
    [string]$MSBuild='C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(!$CandidateDll) {$CandidateDll=Join-Path $repo 'x64\Release\a\OptiScaler.dll'}
New-Item -ItemType Directory -Path $OutDirectory -Force | Out-Null
$original=Join-Path $OutDirectory 'original'
if(Test-Path -LiteralPath (Join-Path $original 'dxgi.dll')) {throw 'Original SDK baseline contains a proxy'}
& $MSBuild (Join-Path $PSScriptRoot 'smoke.vcxproj') /p:Configuration=Release /p:Platform=x64 "/p:OutDir=$original\" "/p:IntDir=$OutDirectory\obj\" /v:minimal
if($LASTEXITCODE) {throw 'Test build failed'}
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
LogToFile=true
LogToConsole=false
LogLevel=2
LogAsync=false
[DlssNr]
NativeXeSS=true
Enabled=false
'@
$cases=@(
    @{name='original';mode='pure';dll=$null;native=$true},
    @{name='candidate-pure';mode='pure';dll=$CandidateDll;native=$true},
    @{name='candidate-reflex';mode='reflex';dll=$CandidateDll;native=$true},
    @{name='candidate-legacy-pure';mode='pure';dll=$CandidateDll;native=$false}
)
if($ReferenceDll) {$cases+=@{name='reference-reflex';mode='reflex';dll=$ReferenceDll;native=$true}}
$results=@()
foreach($case in $cases) {
    $folder=Join-Path $OutDirectory $case.name
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $exe=Join-Path $folder 'native_fg_smoke.exe'
    if($case.name -ne 'original') {Copy-Item -LiteralPath (Join-Path $original 'native_fg_smoke.exe') -Destination $exe}
    foreach($name in @('libxess.dll','libxess_dx11.dll','libxess_fg.dll','libxell.dll')) {
        Copy-Item -LiteralPath (Join-Path $GameDirectory $name) -Destination $folder
    }
    if($case.dll) {
        Copy-Item -LiteralPath $case.dll -Destination (Join-Path $folder 'dxgi.dll')
        $config=if($case.native){$ini}else{$ini.Replace('NativeXeSS=true','NativeXeSS=false')}
        $config | Set-Content -LiteralPath (Join-Path $folder 'OptiScaler.ini') -Encoding ascii
    }
    & $exe $folder $case.mode *> (Join-Path $folder 'run.txt')
    $code=$LASTEXITCODE
    $output=Get-Content -LiteralPath (Join-Path $folder 'run.txt') -Raw
    if($case.name -eq 'reference-reflex') {
        if($code -ne 1 -or $output -notmatch 'native XeLL enable was silently ignored') {throw 'Reference did not reproduce the XeLL ownership bug'}
    } elseif($code -ne 0 -or $output -notmatch 'PASS: XeLL enable preserved') {throw "$($case.name) failed: $code"}
    $results+=[pscustomobject]@{case=$case.name;exitCode=$code;expected=$true}
    Write-Output "$($case.name): expected result ($code)"
}
$results|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutDirectory 'results.json')
