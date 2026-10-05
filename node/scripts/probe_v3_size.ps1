param([ValidateSet('counter_reed','binary','binary_sht40','climate_tmp112','counter_reed_debug','binary_debug','binary_sht40_debug','climate_tmp112_debug')][string]$Environment = 'counter_reed')
$ErrorActionPreference = 'Stop'
$nodeRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
Push-Location $nodeRoot
try {
    $elf = Join-Path $nodeRoot ".pio/build/$Environment/firmware.elf"
    & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e $Environment
    if ($LASTEXITCODE -ne 0) { throw 'Firmware build failed.' }
    $sections = & "$env:USERPROFILE\.platformio\packages\toolchain-atmelavr\bin\avr-size.exe" -A $elf
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect size-probe ELF.' }
    $sizes = @{}
    foreach ($line in $sections) {
        if ($line -match '^\s*(\.\w+)\s+(\d+)\s+') { $sizes[$Matches[1]] = [int]$Matches[2] }
    }
    $flash = $sizes['.text'] + $sizes['.rodata'] + $sizes['.data']
    $ram = $sizes['.data'] + $sizes['.bss'] + $sizes['.noinit']
    [pscustomobject]@{ Environment = $Environment; FlashBytes = $flash; FlashLimit = 32768; FreeFlash = 32768 - $flash; StaticRamBytes = $ram; RamLimit = 3072 } | Format-List
} finally { Pop-Location }
