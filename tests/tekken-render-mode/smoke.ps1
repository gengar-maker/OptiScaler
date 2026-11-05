$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '..\..\tools\Set-TekkenNrMode.ps1'
$fixture = Join-Path $PSScriptRoot ('test-artifacts\' + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
foreach ($name in @('TekkenGame-Win64-Shipping.exe','dxgi.dll')) {
    [IO.File]::WriteAllText((Join-Path $fixture $name), 'Test placeholder, never executed')
}
$config = Join-Path $fixture 'Engine.ini'
$original = "[Core.System]`r`nPaths=keep/me`r`n[SystemSettings]`r`nr.ScreenPercentage=50`r`nr.Other=42`r`n[Other]`r`nUnrelated=true`r`n"
[IO.File]::WriteAllText($config, $original)
foreach ($mode in @('Native','XeSSBeforeSNR','SNRBeforeXeSS')) {
    $result = & $script -Mode $mode -GameDirectory $fixture -EngineConfigPath $config
    $text = [IO.File]::ReadAllText($config)
    $expected = if ($mode -eq 'Native') { 100 } else { 50 }
    if ($text -notmatch "r.ScreenPercentage=$expected" -or $text -notmatch 'Paths=keep/me' -or
        $text -notmatch 'r.Other=42' -or $text -notmatch 'Unrelated=true') { throw 'INI content damaged' }
    if ((Test-Path -LiteralPath (Join-Path $fixture 'OptiScaler.snr-native.enable')) -ne ($mode -eq 'Native')) { throw 'Wrong native marker' }
    if ((Test-Path -LiteralPath (Join-Path $fixture 'OptiScaler.xess-snr.enable')) -ne ($mode -eq 'XeSSBeforeSNR')) { throw 'Wrong order marker' }
    if (!(Test-Path -LiteralPath (Join-Path $result.Backup 'Engine.ini'))) { throw 'Missing backup' }
}
[IO.File]::WriteAllText($config, "[Core.System]`r`nPaths=keep/me`r`n")
$null = & $script -Mode Native -GameDirectory $fixture -EngineConfigPath $config
if ([IO.File]::ReadAllText($config) -notmatch '\[SystemSettings\][\r\n]+r.ScreenPercentage=100') {
    throw 'Missing SystemSettings was not created'
}
Write-Output "PASS: all three modes, Engine.ini preservation, marker selection, snapshots, missing section. Fixture: $fixture"
