#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

#Requires -Version 5.1

<#
.SYNOPSIS
  Turn Android airplane mode on or off via ADB.

.DESCRIPTION
  Uses `cmd connectivity airplane-mode` when available, otherwise
  `settings put global airplane_mode_on` plus AIRPLANE_MODE broadcast.

.PARAMETER On
  Enable airplane mode.

.PARAMETER Off
  Disable airplane mode.

.PARAMETER Toggle
  Flip current airplane mode.

.PARAMETER Status
  Print current setting (settings global airplane_mode_on).

.PARAMETER Serial
  Device serial passed to adb as -s (same as adb -s). Alias: -s.

.PARAMETER AdbPath
  Path to adb executable (default: adb on PATH).

.EXAMPLE
  .\adb-airplane-mode.ps1 -On
.EXAMPLE
  .\adb-airplane-mode.ps1 -Off -Serial R58M1234567
.EXAMPLE
  .\adb-airplane-mode.ps1 -On -s R58M1234567
.EXAMPLE
  .\adb-airplane-mode.ps1 -Status
#>
[CmdletBinding()]
param(
    [switch] $On,
    [switch] $Off,
    [switch] $Toggle,
    [switch] $Status,

    [Alias('s')]
    [string] $Serial,
    [string] $AdbPath = 'adb'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-AdbExe {
    param([string] $Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { $Path = 'adb' }
    if ($Path -eq 'adb') {
        if (-not (Get-Command adb -ErrorAction SilentlyContinue)) {
            Write-Error "adb not found on PATH. Install Android SDK platform-tools or pass -AdbPath."
        }
    } elseif (-not (Test-Path -LiteralPath $Path)) {
        Write-Error "AdbPath not found: $Path"
    }
    return $Path
}

function Invoke-AdbShell {
    param(
        [string] $Adb,
        [string] $Serial,
        [string] $ShellArgs
    )
    $argv = @()
    if ($Serial) {
        $argv += '-s', $Serial
    }
    $argv += 'shell', $ShellArgs
    & $Adb @argv
    return $LASTEXITCODE
}

function Get-AirplaneModeSetting {
    param([string] $Adb, [string] $Serial)
    $argv = @()
    if ($Serial) { $argv += '-s', $Serial }
    $argv += 'shell', 'settings', 'get', 'global', 'airplane_mode_on'
    $out = & $Adb @argv 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Error "adb failed: $out"
    }
    return ($out | Out-String).Trim()
}

function Set-AirplaneModeConnectivityCmd {
    param(
        [string] $Adb,
        [string] $Serial,
        [bool] $Enable
    )
    $sub = if ($Enable) { 'enable' } else { 'disable' }
    $null = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "cmd connectivity airplane-mode $sub"
    return ($LASTEXITCODE -eq 0)
}

function Set-AirplaneModeLegacy {
    param(
        [string] $Adb,
        [string] $Serial,
        [bool] $Enable
    )
    $val = if ($Enable) { '1' } else { '0' }
    $state = if ($Enable) { 'true' } else { 'false' }
    $code1 = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "settings put global airplane_mode_on $val"
    $code2 = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "am broadcast -a android.intent.action.AIRPLANE_MODE --ez state $state"
    if ($code1 -ne 0 -or $code2 -ne 0) {
        Write-Error "Legacy airplane mode commands failed (exit $code1 / $code2)."
    }
}

function Set-AirplaneMode {
    param(
        [string] $Adb,
        [string] $Serial,
        [bool] $Enable
    )
    if (Set-AirplaneModeConnectivityCmd -Adb $Adb -Serial $Serial -Enable $Enable) {
        return
    }
    Set-AirplaneModeLegacy -Adb $Adb -Serial $Serial -Enable $Enable
}

$AdbExe = Get-AdbExe -Path $AdbPath

# Pipeline can return a scalar when only one switch is true; @() ensures .Count exists under StrictMode.
$chosen = @(@($On, $Off, $Toggle, $Status) | Where-Object { $_ })
if ($chosen.Count -gt 1) {
    Write-Error 'Specify only one of: -On, -Off, -Toggle, -Status'
}
if ($chosen.Count -eq 0) {
    Write-Host @'
Usage:
  .\adb-airplane-mode.ps1 -On
  .\adb-airplane-mode.ps1 -Off
  .\adb-airplane-mode.ps1 -Toggle
  .\adb-airplane-mode.ps1 -Status

Optional: -s / -Serial <device>  -AdbPath <path\to\adb.exe>
'@
    exit 1
}

if ($Status) {
    $v = Get-AirplaneModeSetting -Adb $AdbExe -Serial $Serial
    Write-Output $v
    exit 0
}

if ($Toggle) {
    $cur = Get-AirplaneModeSetting -Adb $AdbExe -Serial $Serial
    $enable = ($cur -ne '1')
    Set-AirplaneMode -Adb $AdbExe -Serial $Serial -Enable $enable
    Write-Output ("Airplane mode -> {0}" -f ($(if ($enable) { 'ON' } else { 'OFF' })))
    exit 0
}

if ($On) {
    Set-AirplaneMode -Adb $AdbExe -Serial $Serial -Enable $true
    Write-Output 'Airplane mode -> ON'
    exit 0
}

if ($Off) {
    Set-AirplaneMode -Adb $AdbExe -Serial $Serial -Enable $false
    Write-Output 'Airplane mode -> OFF'
    exit 0
}

throw 'Internal error: no action executed.'
