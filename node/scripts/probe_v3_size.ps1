param([ValidateSet('counter_reed','binary','binary_sht40','climate_tmp112')][string]$Environment = 'counter_reed')
$ErrorActionPreference = 'Stop'
$nodeRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$config = @'
[platformio]
extra_configs = platformio.ini
build_dir = .pio/v3-size-build
default_envs = v3_size_probe

[env:v3_size_probe]
extends = counter_reed
lib_ldf_mode = deep+
build_flags =
    ${counter_reed.build_flags}
    -Wl,--defsym=__TEXT_REGION_LENGTH__=32768
    -Wl,-Map,.pio/v3-size-build/v3_size_probe/firmware.map
'@
$config = $config.Replace('counter_reed',$Environment)
Set-Content -LiteralPath (Join-Path $nodeRoot '.pio/v3-size-probe.ini') -Value $config -Encoding ascii
Push-Location $nodeRoot
try {
    # Only the linker region expands for inspecting an oversized ELF.
    # Release sources, MCU and PlatformIO's 16 KiB check stay intact.
    $elf = Join-Path $nodeRoot '.pio/v3-size-build/v3_size_probe/firmware.elf'
    if (Test-Path -LiteralPath $elf) { Remove-Item -LiteralPath $elf }
    & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -c .pio/v3-size-probe.ini -e v3_size_probe
    $probeExit = $LASTEXITCODE
    if ($probeExit -ne 0 -and !(Test-Path -LiteralPath $elf)) { throw 'Size probe failed before producing an ELF.' }
    $sections = & "$env:USERPROFILE\.platformio\packages\toolchain-atmelavr\bin\avr-size.exe" -A $elf
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect size-probe ELF.' }
    $sizes = @{}
    foreach ($line in $sections) {
        if ($line -match '^\s*(\.\w+)\s+(\d+)\s+') { $sizes[$Matches[1]] = [int]$Matches[2] }
    }
    $flash = $sizes['.text'] + $sizes['.rodata'] + $sizes['.data']
    $ram = $sizes['.data'] + $sizes['.bss'] + $sizes['.noinit']
    [pscustomobject]@{ Environment = $Environment; FlashBytes = $flash; FlashLimit = 16384; FreeFlash = 16384 - $flash; StaticRamBytes = $ram; RamLimit = 2048 } | Format-List
} finally { Pop-Location }
