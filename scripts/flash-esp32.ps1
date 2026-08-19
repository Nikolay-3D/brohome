[CmdletBinding()]
param(
    [string]$Port,
    [switch]$PrepareOnly
)

$ErrorActionPreference = 'Stop'

function Find-ArduinoCli {
    $fromPath = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'Arduino CLI was not found. Install Arduino IDE 2 from https://www.arduino.cc/en/software and run this wizard again.'
}

function Read-RequiredText([string]$Prompt, [string]$Default = '') {
    while ($true) {
        $suffix = if ($Default) { " [$Default]" } else { '' }
        $value = Read-Host "$Prompt$suffix"
        if (-not $value) { $value = $Default }
        if ($value) { return $value }
        Write-Host 'This value cannot be empty.' -ForegroundColor Yellow
    }
}

function Read-Password([string]$Prompt, [int]$MinimumLength = 1) {
    while ($true) {
        $secure = Read-Host $Prompt -AsSecureString
        $value = [System.Net.NetworkCredential]::new('', $secure).Password
        if ($value.Length -ge $MinimumLength) { return $value }
        Write-Host "The password must contain at least $MinimumLength characters." -ForegroundColor Yellow
    }
}

function ConvertTo-CString([string]$Value) {
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

function Invoke-Arduino([string[]]$Arguments) {
    Write-Host "arduino-cli $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $script:ArduinoCli @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Arduino CLI exited with code $LASTEXITCODE. Fix the error shown above and run the wizard again."
    }
}

function Test-ArduinoList([string[]]$Arguments, [string]$Pattern) {
    $output = (& $script:ArduinoCli @Arguments 2>$null | Out-String)
    return $LASTEXITCODE -eq 0 -and $output -match $Pattern
}

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$SketchDir = Join-Path $ProjectRoot 'esp32\esp32-s3-devkitc-1\brohome_control_hub'
$SecretsPath = Join-Path $SketchDir 'secrets.h'
$script:ArduinoCli = Find-ArduinoCli
$EspressifIndex = 'https://espressif.github.io/arduino-esp32/package_esp32_index.json'
$Fqbn = 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,EraseFlash=none'

Write-Host 'BroHome: prepare ESP32-S3 N16R8' -ForegroundColor Cyan
Write-Host "Arduino CLI: $script:ArduinoCli"

if (Test-ArduinoList -Arguments @('core', 'list') -Pattern '(?m)^esp32:esp32\s+3\.3\.8\b') {
    Write-Host 'ESP32 core 3.3.8 is already installed.' -ForegroundColor Green
} else {
    Invoke-Arduino @('core', 'update-index', '--additional-urls', $EspressifIndex)
    Invoke-Arduino @('core', 'install', 'esp32:esp32@3.3.8', '--additional-urls', $EspressifIndex)
}

if (Test-ArduinoList -Arguments @('lib', 'list') -Pattern '(?m)^IRremote\s+4\.7\.1\b') {
    Write-Host 'IRremote 4.7.1 is already installed.' -ForegroundColor Green
} else {
    Invoke-Arduino @('lib', 'install', 'IRremote@4.7.1')
}

if (Test-ArduinoList -Arguments @('lib', 'list') -Pattern '(?m)^RadioLib\s+7\.6\.0\b') {
    Write-Host 'RadioLib 7.6.0 is already installed.' -ForegroundColor Green
} else {
    Invoke-Arduino @('lib', 'install', 'RadioLib@7.6.0')
}

$wifiName = Read-RequiredText 'Home Wi-Fi name (2.4 GHz)'
$wifiPassword = Read-Password 'Home Wi-Fi password'
$serverHost = Read-RequiredText 'BroHome Linux server IP address' '192.168.1.100'
$deviceName = Read-RequiredText 'Short name for this hub' 'kitchen'
$apName = Read-RequiredText 'ESP recovery access-point name' 'BroHome-Control'
$apPassword = Read-Password 'ESP recovery access-point password (at least 8 characters)' 8

$secrets = @"
#pragma once

// Created locally by the BroHome wizard. Git ignores this file.
#define BROHOME_WIFI_SSID "$(ConvertTo-CString $wifiName)"
#define BROHOME_WIFI_PASSWORD "$(ConvertTo-CString $wifiPassword)"
#define BROHOME_SERVER_HOST "$(ConvertTo-CString $serverHost)"
#define BROHOME_DEVICE_NAME "$(ConvertTo-CString $deviceName)"
#define BROHOME_AP_SSID "$(ConvertTo-CString $apName)"
#define BROHOME_AP_PASSWORD "$(ConvertTo-CString $apPassword)"
"@
[System.IO.File]::WriteAllText($SecretsPath, $secrets, [System.Text.UTF8Encoding]::new($false))
Write-Host "Local settings saved: $SecretsPath" -ForegroundColor Green

Invoke-Arduino @('compile', '--fqbn', $Fqbn, $SketchDir)

if ($PrepareOnly) {
    Write-Host 'Preparation and compilation completed. Upload skipped because -PrepareOnly was used.' -ForegroundColor Green
    exit 0
}

$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if ($availablePorts.Count -eq 0) {
    throw 'No COM ports were found. Connect the ESP32 with a USB data cable.'
}
Write-Host "Available COM ports: $($availablePorts -join ', ')"
if (-not $Port) {
    $Port = Read-RequiredText 'ESP32 COM port' $availablePorts[-1]
}
if ($availablePorts -notcontains $Port) {
    throw "Port $Port was not found. Available ports: $($availablePorts -join ', ')"
}

Write-Host 'Starting upload. Do not disconnect USB.' -ForegroundColor Cyan
Invoke-Arduino @('upload', '--fqbn', $Fqbn, '--port', $Port, $SketchDir)
Write-Host 'Done. ESP32 was flashed without a full flash erase.' -ForegroundColor Green
Write-Host 'Wait 15 seconds, then find the ESP in your router client list.'
