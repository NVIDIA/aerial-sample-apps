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
  After turning airplane mode off, poll dumpsys telephony.registry every second
  and print a compact summary from mServiceState and mCellInfo (last known state).

.PARAMETER IntervalSeconds
  Delay between dumpsys calls (default: 1).

.PARAMETER Count
  Number of samples; 0 = until Ctrl+C.

.PARAMETER Serial
  adb -s <serial>. Alias: -s.

.PARAMETER Serial2
  Second device serial; when set with Serial, polls both devices each interval.
  With -Watch, the screen clears once per refresh and both devices are redrawn (stacked).

.PARAMETER AdbPath
  Path to adb (default: adb on PATH).

.PARAMETER PhoneId
  Which Phone Id block to show from last known state (default: 0).

.PARAMETER Watch
  Like GNU watch: clear the screen each refresh and redraw the summary at the top (no scrolling log).

.PARAMETER DebugCellInfoNr
  Print each CellInfoNr block and show key fields side-by-side for correlation checks.
#>
[CmdletBinding()]
param(
    [double] $IntervalSeconds = 2,
    [int] $Count = 0,
    [int] $PhoneId = 0,
    [switch] $DebugCellInfoNr,
    [switch] $Watch,

    [Alias('s')]
    [string] $Serial,
    [string] $Serial2,
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
    $null = & $Adb @argv 2>&1
    return $LASTEXITCODE
}

function Set-AirplaneModeConnectivityCmd {
    param([string] $Adb, [string] $Serial, [bool] $Enable)
    $sub = if ($Enable) { 'enable' } else { 'disable' }
    $null = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "cmd connectivity airplane-mode $sub"
    return ($LASTEXITCODE -eq 0)
}

function Set-AirplaneModeLegacy {
    param([string] $Adb, [string] $Serial, [bool] $Enable)
    $val = if ($Enable) { '1' } else { '0' }
    $state = if ($Enable) { 'true' } else { 'false' }
    $code1 = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "settings put global airplane_mode_on $val"
    $code2 = Invoke-AdbShell -Adb $Adb -Serial $Serial -ShellArgs "am broadcast -a android.intent.action.AIRPLANE_MODE --ez state $state"
    if ($code1 -ne 0 -or $code2 -ne 0) {
        Write-Error "Legacy airplane mode commands failed (exit $code1 / $code2)."
    }
}

function Set-AirplaneMode {
    param([string] $Adb, [string] $Serial, [bool] $Enable)
    if (Set-AirplaneModeConnectivityCmd -Adb $Adb -Serial $Serial -Enable $Enable) {
        return
    }
    Set-AirplaneModeLegacy -Adb $Adb -Serial $Serial -Enable $Enable
}

function Get-DumpsysTelephonyRegistryText {
    param([string] $Adb, [string] $Serial)
    $argv = @()
    if ($Serial) { $argv += '-s', $Serial }
    $argv += 'shell', 'dumpsys', 'telephony.registry'
    $out = & $Adb @argv 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Error "adb dumpsys telephony.registry failed (exit $LASTEXITCODE): $out"
    }
    return $out
}

function Get-LastKnownStateSection {
    param([string] $DumpText)
    $startToken = 'last known state:'
    $idx = $DumpText.IndexOf($startToken, [StringComparison]::Ordinal)
    if ($idx -lt 0) {
        return $null
    }
    $fromStart = $DumpText.Substring($idx)
    $m = [regex]::Match($fromStart, '(?m)^local logs:')
    if ($m.Success) {
        return $fromStart.Substring(0, $m.Index)
    }
    return $fromStart
}

function Truncate-BeforeNextPhoneSiblingField {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return $Text
    }
    $rm = [regex]::Match(
        $Text,
        '(?m)\r?\n\s*(mSignalStrength|mCellInfo|mCallQuality|mCallState|mVoiceActivationState|mUserMobileDataState|mPreciseDataConnectionState|mDataActivity|mPreciseCallState|mRingingCallState|mForegroundCallState|mBackgroundCallState|mEmergencyNumberList|mBarringInfo|mCarrierNetworkChangeState|mTelephonyDisplayInfo)\s*='
    )
    if ($rm.Success) {
        return $Text.Substring(0, $rm.Index)
    }
    return $Text
}

function Get-SpanLengthBraceBalanced {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return 0
    }
    $depth = 0
    $saw = $false
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $c = $Text[$i]
        if ($c -ceq '{') {
            $depth++
            $saw = $true
        } elseif ($c -ceq '}') {
            $depth--
        }
        if ($saw -and $depth -eq 0) {
            return $i + 1
        }
    }
    if ($saw) {
        return (Truncate-BeforeNextPhoneSiblingField -Text $Text).Length
    }
    return 0
}

function Get-SpanLengthBracketArray {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return -1
    }
    $t = $Text.TrimStart()
    $offset = $Text.Length - $t.Length
    if ($t.Length -eq 0 -or $t[0] -cne '[') {
        return -1
    }
    $depth = 0
    for ($i = 0; $i -lt $t.Length; $i++) {
        if ($t[$i] -ceq '[') {
            $depth++
        } elseif ($t[$i] -ceq ']') {
            $depth--
            if ($depth -eq 0) {
                return $offset + $i + 1
            }
        }
    }
    return (Truncate-BeforeNextPhoneSiblingField -Text $Text).Length
}

function Get-FirstLineLength {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return 0
    }
    $nl = $Text.IndexOf("`n", [StringComparison]::Ordinal)
    if ($nl -lt 0) {
        return $Text.Length
    }
    return $nl
}

function Get-MultiLineFieldFromPhoneBlock {
    param(
        [string] $Block,
        [string] $FieldName
    )
    if ([string]::IsNullOrEmpty($Block) -or [string]::IsNullOrEmpty($FieldName)) {
        return $null
    }
    $esc = [regex]::Escape($FieldName)
    $m = [regex]::Match($Block, '(?m)^\s*' + $esc + '=(.*)$')
    if (-not $m.Success) {
        return $null
    }
    $eqIdx = $Block.IndexOf('=', $m.Index, [StringComparison]::Ordinal)
    if ($eqIdx -lt 0) {
        return $null
    }
    $afterEq = $Block.Substring($eqIdx + 1)

    if ($FieldName -eq 'mCellInfo') {
        $len = Get-SpanLengthBracketArray -Text $afterEq
        if ($len -lt 0) {
            $trunc = Truncate-BeforeNextPhoneSiblingField -Text $afterEq
            $len = Get-FirstLineLength -Text $trunc
        }
        return $afterEq.Substring(0, $len).TrimEnd()
    }

    $lenB = Get-SpanLengthBraceBalanced -Text $afterEq
    if ($lenB -gt 0) {
        return $afterEq.Substring(0, $lenB).TrimEnd()
    }

    # No wrapping `{`…`}` (or only a partial scan): take text until the next top-level m* field line.
    $trunc2 = Truncate-BeforeNextPhoneSiblingField -Text $afterEq
    return $trunc2.TrimEnd()
}

function Parse-TelephonyLastKnownFields {
    param([string] $SectionText)
    $list = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrEmpty($SectionText)) {
        return @()
    }
    $rx = [regex]'(?m)^\s*Phone Id=(\d+)\s*$'
    $mc = $rx.Matches($SectionText)
    if ($mc.Count -eq 0) {
        return @()
    }
    for ($i = 0; $i -lt $mc.Count; $i++) {
        $start = $mc[$i].Index
        $end = if (($i + 1) -lt $mc.Count) { $mc[$i + 1].Index } else { $SectionText.Length }
        $block = $SectionText.Substring($start, $end - $start)
        $parsedPhoneId = $mc[$i].Groups[1].Value
        $svc = Get-MultiLineFieldFromPhoneBlock -Block $block -FieldName 'mServiceState'
        $sig = Get-MultiLineFieldFromPhoneBlock -Block $block -FieldName 'mSignalStrength'
        $cell = Get-MultiLineFieldFromPhoneBlock -Block $block -FieldName 'mCellInfo'
        $cq = Get-MultiLineFieldFromPhoneBlock -Block $block -FieldName 'mCallQuality'
        $list.Add([pscustomobject]@{
                PhoneId         = $parsedPhoneId
                mServiceState   = $svc
                mSignalStrength = $sig
                mCellInfo       = $cell
                mCallQuality    = $cq
            })
    }
    # No leading comma: "return , $arr" wraps a multi-element array in a 1-element outer array,
    # so callers enumerate once and [string]$_.Name joins all field names onto one line.
    return @($list.ToArray())
}

function Add-ExtractedField {
    param(
        [System.Collections.Generic.List[object]] $List,
        [string] $Text,
        [string] $Pattern
    )
    if ([string]::IsNullOrEmpty($Text)) {
        return
    }
    $m = [regex]::Match($Text, $Pattern)
    if (-not $m.Success) {
        return
    }
    $full = $m.Value.Trim()
    $eq = $full.IndexOf('=')
    if ($eq -lt 0) {
        $List.Add([pscustomobject]@{ Name = ''; Value = $full })
        return
    }
    $name = $full.Substring(0, $eq).Trim()
    $value = $full.Substring($eq + 1).Trim()
    $List.Add([pscustomobject]@{ Name = $name; Value = $value })
}

# mServiceState often lists multiple transports; the first accessNetworkTechnology can be IWLAN while
# cellular NR/LTE appears later. Prefer a non-IWLAN RAT when present.
function Add-ExtractedFieldAccessNetworkTechnologyPreferCellular {
    param(
        [System.Collections.Generic.List[object]] $List,
        [string] $Text
    )
    if ([string]::IsNullOrEmpty($Text)) {
        return
    }
    $rx = [regex]'accessNetworkTechnology\s*=\s*(\w+)'
    $all = @($rx.Matches($Text))
    if ($all.Count -eq 0) {
        return
    }
    $rank = @{
        'NR_SA'   = 100
        'NR'      = 95
        'LTE_CA'  = 85
        'LTE'     = 80
        'UMTS'    = 50
        'HSPA'    = 50
        'HSUPA'   = 50
        'HSDPA'   = 50
        'EDGE'    = 40
        'GPRS'    = 30
        'eHRPD'   = 45
        'TD_SCDMA' = 45
    }
    $nonIwlan = [System.Collections.Generic.List[object]]::new()
    foreach ($m in $all) {
        $v = [string] $m.Groups[1].Value
        if ($v -eq 'IWLAN' -or $v -eq 'WLAN' -or $v -eq 'ETHERNET') {
            continue
        }
        $null = $nonIwlan.Add($m)
    }
    $pick = $null
    if ($nonIwlan.Count -gt 0) {
        $bestRank = -1
        foreach ($m in $nonIwlan) {
            $v = [string] $m.Groups[1].Value
            $r = 0
            if ($rank.ContainsKey($v)) {
                $r = $rank[$v]
            }
            if ($r -gt $bestRank) {
                $bestRank = $r
                $pick = $m
            }
        }
        if ($null -eq $pick) {
            $pick = $nonIwlan[0]
        }
    }
    else {
        $pick = $all[0]
    }
    $val = [string] $pick.Groups[1].Value
    $List.Add([pscustomobject]@{ Name = 'accessNetworkTechnology'; Value = $val })
}

# First rRplmn= in the blob may be empty; use the first assignment with a non-empty PLMN-like value.
function Add-ExtractedFieldRplmnPreferNonEmpty {
    param(
        [System.Collections.Generic.List[object]] $List,
        [string] $Text
    )
    if ([string]::IsNullOrEmpty($Text)) {
        return
    }
    $rx = [regex]'rRplmn\s*=\s*([^\s,}\r\n]*)'
    foreach ($m in $rx.Matches($Text)) {
        $val = ([string] $m.Groups[1].Value).Trim()
        if ($val.Length -eq 0 -or $val -eq '*' -or $val -eq 'null') {
            continue
        }
        $List.Add([pscustomobject]@{ Name = 'rRplmn'; Value = $val })
        return
    }
    $m0 = $rx.Match($Text)
    if ($m0.Success) {
        $val = ([string] $m0.Groups[1].Value).Trim()
        $List.Add([pscustomobject]@{ Name = 'rRplmn'; Value = $val })
    }
}

# MobileData / MobileDataRat can repeat per transport; use last match on full ServiceState, or PS block if only there.
function Add-ExtractedFieldMobileDataPairPreferCellular {
    param(
        [System.Collections.Generic.List[object]] $List,
        [string] $Text,
        [string] $PsBlock
    )
    $pairs = @(
        @{ Pattern = 'MobileData\s*=\s*(\w+)'; Name = 'MobileData' },
        @{ Pattern = 'MobileDataRat\s*=\s*([\w]+)'; Name = 'MobileDataRat' }
    )
    foreach ($pair in $pairs) {
        $ms = $null
        if (-not [string]::IsNullOrEmpty($Text)) {
            $ms = [regex]::Matches($Text, $pair.Pattern)
        }
        if (($null -eq $ms -or $ms.Count -eq 0) -and -not [string]::IsNullOrEmpty($PsBlock)) {
            $ms = [regex]::Matches($PsBlock, $pair.Pattern)
        }
        if ($null -eq $ms -or $ms.Count -eq 0) {
            continue
        }
        $m = $ms[$ms.Count - 1]
        $List.Add([pscustomobject]@{ Name = $pair.Name; Value = [string] $m.Groups[1].Value })
    }
}

function Get-BalancedBraceSpan {
    param(
        [string] $Text,
        [int] $OpenBraceIndex
    )
    if ([string]::IsNullOrEmpty($Text) -or $OpenBraceIndex -lt 0 -or $OpenBraceIndex -ge $Text.Length) {
        return $null
    }
    if ($Text[$OpenBraceIndex] -cne '{') {
        return $null
    }
    $depth = 0
    for ($i = $OpenBraceIndex; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($ch -ceq '{') {
            $depth++
        } elseif ($ch -ceq '}') {
            $depth--
            if ($depth -eq 0) {
                return $Text.Substring($OpenBraceIndex, $i - $OpenBraceIndex + 1)
            }
        }
    }
    return $null
}

function Get-AllNetworkRegistrationInfoSpans {
    param([string] $Text)
    $list = [System.Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrEmpty($Text)) {
        return @()
    }
    $openBracesSeen = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($m in [regex]::Matches($Text, '(?:android\.telephony\.)?NetworkRegistrationInfo\s*\{')) {
        $openIdx = $m.Index + $m.Length - 1
        if (-not $openBracesSeen.Add($openIdx)) {
            continue
        }
        if ($openIdx -ge $Text.Length -or $Text[$openIdx] -cne '{') {
            continue
        }
        $span = Get-BalancedBraceSpan -Text $Text -OpenBraceIndex $openIdx
        if ($null -ne $span) {
            $list.Add($span)
        }
    }
    return @($list.ToArray())
}

function Get-AllCellInfoNrSpans {
    param([string] $Text)
    $list = [System.Collections.Generic.List[string]]::new()
    if ([string]::IsNullOrEmpty($Text)) {
        return @()
    }
    $openBracesSeen = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($m in [regex]::Matches($Text, '(?:android\.telephony\.)?CellInfoNr\s*:?\s*\{')) {
        $openIdx = $m.Index + $m.Length - 1
        if (-not $openBracesSeen.Add($openIdx)) {
            continue
        }
        if ($openIdx -ge $Text.Length -or $Text[$openIdx] -cne '{') {
            continue
        }
        $span = Get-BalancedBraceSpan -Text $Text -OpenBraceIndex $openIdx
        if ($null -ne $span) {
            $list.Add($span)
        }
    }
    return @($list.ToArray())
}

function Test-CellInfoNrLikelyServing {
    param([string] $Block)
    if ([string]::IsNullOrEmpty($Block)) {
        return $false
    }
    if ([regex]::IsMatch($Block, 'isRegistered\s*=\s*(?:true|1)\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        return $true
    }
    if ([regex]::IsMatch($Block, 'registered\s*=\s*(?:YES|true|1)\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        return $true
    }
    if ([regex]::IsMatch($Block, '(?:m)?[Cc]onnectionStatus\s*=\s*\d+\([^)]*(?:PRIMARY|SECONDARY)_SERVING', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        return $true
    }
    if ([regex]::IsMatch($Block, '(?:m)?[Cc]onnectionStatus\s*=\s*(?:PRIMARY|SECONDARY)_SERVING\b', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
        return $true
    }
    return $false
}

function Get-NrBlockSsrpForSort {
    param([string] $Block)
    $m = [regex]::Match($Block, 'ssRsrp\s*=\s*(-?\d+)')
    if ($m.Success) {
        return [int] $m.Groups[1].Value
    }
    return -32768
}

function Get-CsiCqiBracketFromNrBlock {
    param([string] $Block)
    if ([string]::IsNullOrEmpty($Block)) {
        return $null
    }
    $m = [regex]::Match($Block, 'csiCqiReport\s*=\s*(\[[^\]]*\])')
    if ($m.Success) {
        return ([string] $m.Groups[1].Value).Trim()
    }
    return $null
}

# Pick csiCqiReport from the right CellInfoNr: OEMs differ on isRegistered; fall back to non-empty CQI + ssRsrp.
function Get-BestCsiCqiBracketFromMCellInfo {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return $null
    }
    $blocks = @(Get-AllCellInfoNrSpans -Text $Text)
    if ($blocks.Count -eq 0) {
        return $null
    }

    $best = $null
    $bestRsrp = -32769
    foreach ($blk in $blocks) {
        if (-not (Test-CellInfoNrLikelyServing -Block $blk)) {
            continue
        }
        $br = Get-CsiCqiBracketFromNrBlock -Block $blk
        if ($null -eq $br -or $br.Length -le 2) {
            continue
        }
        $r = Get-NrBlockSsrpForSort -Block $blk
        if ($r -gt $bestRsrp) {
            $bestRsrp = $r
            $best = $br
        }
    }
    if ($null -ne $best) {
        return $best
    }

    $bestRsrp = -32769
    foreach ($blk in $blocks) {
        $br = Get-CsiCqiBracketFromNrBlock -Block $blk
        if ($null -eq $br -or $br.Length -le 2) {
            continue
        }
        $r = Get-NrBlockSsrpForSort -Block $blk
        if ($r -gt $bestRsrp) {
            $bestRsrp = $r
            $best = $br
        }
    }
    if ($null -ne $best) {
        return $best
    }

    foreach ($blk in $blocks) {
        if (-not (Test-CellInfoNrLikelyServing -Block $blk)) {
            continue
        }
        $br = Get-CsiCqiBracketFromNrBlock -Block $blk
        if ($null -ne $br) {
            return $br
        }
    }

    $m0 = [regex]::Match($Text, 'csiCqiReport\s*=\s*(\[[^\]]*\])')
    if ($m0.Success) {
        return ([string] $m0.Groups[1].Value).Trim()
    }
    return $null
}

function Test-CellIdentityNrArfcnUsable {
    param([string] $S)
    return [regex]::IsMatch($S, 'mNrArfcn\s*=\s*(?!2147483647\b)\d+')
}
function Test-CellIdentityEarfcnUsable {
    param([string] $S)
    return [regex]::IsMatch($S, 'mEarfcn\s*=\s*(?!2147483647\b)\d+')
}
function Test-CellIdentityTacUsable {
    param([string] $S)
    return [regex]::IsMatch($S, 'mTac\s*=\s*(?!2147483647\b)(?!\*)([^\s,}]+)')
}
function Test-CellIdentityPciUsable {
    param([string] $S)
    return [regex]::IsMatch($S, 'mPci\s*=\s*(?!\*)\d+')
}
function Test-CellIdentityNonEmptyBands {
    param([string] $S)
    return [regex]::IsMatch($S, 'mBands\s*=\s*\[[^\]]*[^\s,\]][^\]]*\]')
}

function Get-PsBlockCellIdentityScore {
    param([string] $PsBlock)
    if ([string]::IsNullOrEmpty($PsBlock)) {
        return 0
    }
    $inner = Get-InnerCellIdentitySpan -RegistrationBlock $PsBlock
    if ([string]::IsNullOrEmpty($inner)) {
        return 0
    }
    $score = 0
    if (Test-CellIdentityNrArfcnUsable $inner) { $score += 4 }
    if (Test-CellIdentityEarfcnUsable $inner) { $score += 3 }
    if (Test-CellIdentityNonEmptyBands $inner) { $score += 2 }
    if (Test-CellIdentityTacUsable $inner) { $score += 2 }
    if (Test-CellIdentityPciUsable $inner) { $score += 1 }
    return $score
}

function Get-BestPsNetworkRegistrationBlock {
    param([string] $Text)
    $spans = @(Get-AllNetworkRegistrationInfoSpans -Text $Text)
    $ps = @(
        $spans | Where-Object {
            [regex]::IsMatch($_, 'domain\s*=\s*PS', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        }
    )
    if ($ps.Count -eq 0) {
        return $null
    }

    function Test-PsBlockHasDataService {
        param([string] $S)
        return [regex]::IsMatch($S, 'availableServices\s*=\s*\[[^\]]*\bDATA\b[^\]]*\]')
    }
    function Test-PsBlockHome {
        param([string] $S)
        return [regex]::IsMatch(
            $S,
            'registrationState\s*=\s*(HOME|REG_HOME)\b',
            [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
    }
    function Test-PsBlockNrArfcnUsable {
        param([string] $S)
        return [regex]::IsMatch($S, 'mNrArfcn\s*=\s*(?!2147483647\b)\d+')
    }
    function Test-PsBlockNonEmptyBands {
        param([string] $S)
        return [regex]::IsMatch($S, 'mBands\s*=\s*\[[^\]]*[^\s,\]][^\]]*\]')
    }

    # Prefer PS block whose inner CellIdentity has real ARFCN/TAC/PCI/bands (avoids *1 / MAX_INT placeholders).
    $bestIdx = -1
    $bestScore = -1
    for ($i = 0; $i -lt $ps.Count; $i++) {
        $sc = Get-PsBlockCellIdentityScore -PsBlock $ps[$i]
        if ($sc -gt $bestScore) {
            $bestScore = $sc
            $bestIdx = $i
        }
    }
    if ($bestScore -gt 0) {
        return $ps[$bestIdx]
    }

    $picked = $ps | Where-Object { Test-PsBlockHome $_ } | Select-Object -First 1
    if ($null -ne $picked) {
        return $picked
    }
    $picked = $ps | Where-Object { Test-PsBlockHasDataService $_ } | Select-Object -First 1
    if ($null -ne $picked) {
        return $picked
    }
    $picked = $ps | Where-Object { Test-PsBlockNrArfcnUsable $_ } | Select-Object -First 1
    if ($null -ne $picked) {
        return $picked
    }
    $picked = $ps | Where-Object { Test-PsBlockNonEmptyBands $_ } | Select-Object -First 1
    if ($null -ne $picked) {
        return $picked
    }
    # Data registration is often the last PS entry in mNetworkRegistrationInfos
    return $ps[$ps.Count - 1]
}

function Get-BestAvailableServicesAssignment {
    param([string] $Text)
    if ([string]::IsNullOrEmpty($Text)) {
        return $null
    }
    $spans = @(Get-AllNetworkRegistrationInfoSpans -Text $Text)
    $ps = @(
        $spans | Where-Object {
            [regex]::IsMatch($_, 'domain\s*=\s*PS', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        }
    )
    foreach ($b in $ps) {
        $m = [regex]::Match($b, 'availableServices\s*=\s*\[[^\]]*\]')
        if (-not $m.Success) {
            continue
        }
        $full = $m.Value.Trim()
        if ($full -match 'availableServices\s*=\s*\[\s*\]') {
            continue
        }
        return $full
    }
    foreach ($b in $ps) {
        $m = [regex]::Match($b, 'availableServices\s*=\s*\[[^\]]*\]')
        if ($m.Success) {
            return $m.Value.Trim()
        }
    }
    return $null
}

function Get-InnerCellIdentitySpan {
    param([string] $RegistrationBlock)
    if ([string]::IsNullOrEmpty($RegistrationBlock)) {
        return $null
    }
    $m = [regex]::Match(
        $RegistrationBlock,
        '(?:android\.telephony\.)?CellIdentityNr\w*\s*:?\s*\{'
    )
    if (-not $m.Success) {
        $m = [regex]::Match(
            $RegistrationBlock,
            '(?:android\.telephony\.)?CellIdentityLte\w*\s*:?\s*\{'
        )
    }
    if (-not $m.Success) {
        return $null
    }
    $openIdx = $m.Index + $m.Length - 1
    if ($openIdx -ge $RegistrationBlock.Length -or $RegistrationBlock[$openIdx] -cne '{') {
        return $null
    }
    return Get-BalancedBraceSpan -Text $RegistrationBlock -OpenBraceIndex $openIdx
}

function Test-IsSentinelCellIdentityValue {
    param(
        [string] $FieldKey,
        [string] $Value
    )
    if ([string]::IsNullOrEmpty($Value)) {
        return $true
    }
    $v = $Value.Trim()
    if ($FieldKey -in @('mPci', 'mTac') -and $v.StartsWith('*')) {
        return $true
    }
    if ($FieldKey -in @('mTac', 'mNrArfcn', 'mEarfcn') -and $v -eq '2147483647') {
        return $true
    }
    if ($FieldKey -eq 'mBands' -and $v -match '^\[\s*\]$') {
        return $true
    }
    return $false
}

function Try-ExtractFirstAssignmentRow {
    param(
        [string] $Text,
        [string] $Pattern,
        [string] $FieldKey
    )
    if ([string]::IsNullOrEmpty($Text)) {
        return $null
    }
    $m = [regex]::Match($Text, $Pattern)
    if (-not $m.Success) {
        return $null
    }
    $full = $m.Value.Trim()
    $eq = $full.IndexOf('=')
    if ($eq -lt 0) {
        return $null
    }
    $name = $full.Substring(0, $eq).Trim()
    $value = $full.Substring($eq + 1).Trim()
    if (Test-IsSentinelCellIdentityValue -FieldKey $FieldKey -Value $value) {
        return $null
    }
    return [pscustomobject]@{ Name = $name; Value = $value }
}

function Add-CellIdentityFieldsBestEffort {
    param(
        [System.Collections.Generic.List[object]] $List,
        [string[]] $Sources
    )
    $defs = @(
        @{ Key = 'mPci'; Pattern = 'mPci\s*=\s*[^\s,}]+' },
        @{ Key = 'mTac'; Pattern = 'mTac\s*=\s*[^\s,}]+' },
        @{ Key = 'mNrArfcn'; Pattern = 'mNrArfcn\s*=\s*\d+' },
        @{ Key = 'mBands'; Pattern = 'mBands\s*=\s*\[[^\]]*\]' },
        @{ Key = 'mEarfcn'; Pattern = 'mEarfcn\s*=\s*\d+' }
    )
    foreach ($d in $defs) {
        $row = $null
        foreach ($src in $Sources) {
            if ([string]::IsNullOrEmpty($src)) {
                continue
            }
            $row = Try-ExtractFirstAssignmentRow -Text $src -Pattern $d.Pattern -FieldKey $d.Key
            if ($null -ne $row) {
                break
            }
        }
        if ($null -ne $row) {
            $List.Add($row)
        }
    }
}

function Get-ServiceStateSummaryFields {
    param(
        [string] $Text,
        [string] $CellInfoText = ''
    )
    $out = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrEmpty($Text) -and [string]::IsNullOrEmpty($CellInfoText)) {
        return @($out.ToArray())
    }
    if ([string]::IsNullOrEmpty($Text)) {
        $cellSpanCiOnly = if (-not [string]::IsNullOrEmpty($CellInfoText)) {
            Get-InnerCellIdentitySpan -RegistrationBlock $CellInfoText
        } else {
            $null
        }
        $srcOnly = [System.Collections.Generic.List[string]]::new()
        foreach ($x in @($cellSpanCiOnly, $CellInfoText)) {
            if ([string]::IsNullOrEmpty($x)) {
                continue
            }
            if ($srcOnly.Contains($x)) {
                continue
            }
            $null = $srcOnly.Add($x)
        }
        Add-CellIdentityFieldsBestEffort -List $out -Sources @($srcOnly.ToArray())
        return @($out.ToArray())
    }
    Add-ExtractedField -List $out -Text $Text -Pattern 'mDataRegState=\d+\([^)]*\)'
    Add-ExtractedField -List $out -Text $Text -Pattern 'duplexMode\(\)=\d+'
    Add-ExtractedField -List $out -Text $Text -Pattern 'mCellBandwidths=\[[^\]]*\]'
    Add-ExtractedField -List $out -Text $Text -Pattern 'isManualNetworkSelection=\w+\([^)]*\)'
    Add-ExtractedField -List $out -Text $Text -Pattern 'mNetworkId=[^\s,}]+'
    Add-ExtractedField -List $out -Text $Text -Pattern 'mSystemId=[^\s,}]+'

    $psBlock = Get-BestPsNetworkRegistrationBlock -Text $Text
    # MobileData* may appear once per transport; prefer PS/cellular tail and allow spaces around '='.
    Add-ExtractedFieldMobileDataPairPreferCellular -List $out -Text $Text -PsBlock $psBlock

    Add-ExtractedFieldAccessNetworkTechnologyPreferCellular -List $out -Text $Text
    Add-ExtractedField -List $out -Text $Text -Pattern 'rejectCause=\d+'
    Add-ExtractedFieldRplmnPreferNonEmpty -List $out -Text $Text
    $svcSrc = if ($null -ne $psBlock) { $psBlock } else { $Text }
    $asLine = Get-BestAvailableServicesAssignment -Text $Text
    if ($null -ne $asLine) {
        $eq = $asLine.IndexOf('=')
        if ($eq -ge 0) {
            $asName = $asLine.Substring(0, $eq).Trim()
            $asVal = $asLine.Substring($eq + 1).Trim()
            $out.Add([pscustomobject]@{ Name = $asName; Value = $asVal })
        }
    } else {
        Add-ExtractedField -List $out -Text $svcSrc -Pattern 'availableServices=\[[^\]]*\]'
    }

    $cellSpanSvc = if ($null -ne $psBlock) {
        Get-InnerCellIdentitySpan -RegistrationBlock $psBlock
    } else {
        $null
    }
    $cellSpanSvcWhole = Get-InnerCellIdentitySpan -RegistrationBlock $Text
    $cellSpanCi = if (-not [string]::IsNullOrEmpty($CellInfoText)) {
        Get-InnerCellIdentitySpan -RegistrationBlock $CellInfoText
    } else {
        $null
    }
    $srcList = [System.Collections.Generic.List[string]]::new()
    foreach ($x in @($cellSpanSvc, $cellSpanSvcWhole, $psBlock, $Text, $cellSpanCi, $CellInfoText)) {
        if ([string]::IsNullOrEmpty($x)) {
            continue
        }
        if ($srcList.Contains($x)) {
            continue
        }
        $null = $srcList.Add($x)
    }
    Add-CellIdentityFieldsBestEffort -List $out -Sources @($srcList.ToArray())
    return @($out.ToArray())
}

function Get-CellInfoSummaryFields {
    param([string] $Text)
    $out = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrEmpty($Text)) {
        return @($out.ToArray())
    }
    $bracket = Get-BestCsiCqiBracketFromMCellInfo -Text $Text
    if ($null -ne $bracket) {
        $out.Add([pscustomobject]@{
                Name  = 'csiCqiReport'
                Value = $bracket
            })
    }
    if ([regex]::IsMatch($Text, 'ssRsrp\s*=')) {
        Add-ExtractedField -List $out -Text $Text -Pattern 'ssRsrp\s*=\s*-?\d+'
        Add-ExtractedField -List $out -Text $Text -Pattern 'ssRsrq\s*=\s*-?\d+'
        Add-ExtractedField -List $out -Text $Text -Pattern 'ssSinr\s*=\s*-?\d+'
    } else {
        Add-ExtractedField -List $out -Text $Text -Pattern 'rsrp\s*=\s*-?\d+'
        Add-ExtractedField -List $out -Text $Text -Pattern 'rsrq\s*=\s*-?\d+'
        Add-ExtractedField -List $out -Text $Text -Pattern 'rssnr\s*=\s*-?\d+'
    }
    return @($out.ToArray())
}

function Write-AlignedFieldSection {
    param(
        [string] $Title,
        [object[]] $Fields,
        [string] $TitleColor = 'Cyan'
    )
    Write-Host ""
    Write-Host "  $Title" -ForegroundColor $TitleColor
    $arr = @($Fields)
    if ($arr.Count -eq 0) {
        Write-Host '    (no summary fields matched)' -ForegroundColor DarkGray
        return
    }
    $maxLabel = 0
    foreach ($f in $arr) {
        $n = [string] $f.Name
        if ($n.Length -gt $maxLabel) {
            $maxLabel = $n.Length
        }
    }
    foreach ($f in $arr) {
        $name = [string] $f.Name
        $val = [string] $f.Value
        if ($name.Length -eq 0) {
            Write-Host "    $val"
        } else {
            $padded = $name.PadRight($maxLabel)
            Write-Host "    $padded  $val"
        }
    }
}

function Get-CellInfoNrDebugRows {
    param(
        [string] $NrBlock,
        [int] $Index
    )
    $rows = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrEmpty($NrBlock)) {
        return @()
    }

    $rows.Add([pscustomobject]@{ Name = 'block'; Value = "CellInfoNr#$Index" })
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'isRegistered\s*=\s*\w+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'connectionStatus\s*=\s*\w+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'csiCqiReport\s*=\s*\[[^\]]*\]'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'ssRsrp\s*=\s*-?\d+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'ssRsrq\s*=\s*-?\d+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'ssSinr\s*=\s*-?\d+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'csiRsrp\s*=\s*-?\d+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'csiRsrq\s*=\s*-?\d+'
    Add-ExtractedField -List $rows -Text $NrBlock -Pattern 'csiSinr\s*=\s*-?\d+'

    $ci = Get-InnerCellIdentitySpan -RegistrationBlock $NrBlock
    $src = if ([string]::IsNullOrEmpty($ci)) { @($NrBlock) } else { @($ci, $NrBlock) }
    Add-CellIdentityFieldsBestEffort -List $rows -Sources $src
    return @($rows.ToArray())
}

function Write-CellInfoNrDebugSection {
    param([string] $CellInfoText)
    Write-Host ""
    Write-Host "  mCellInfo CellInfoNr blocks" -ForegroundColor DarkCyan

    $nrBlocks = @(Get-AllCellInfoNrSpans -Text $CellInfoText)
    if ($nrBlocks.Count -eq 0) {
        Write-Host '    (no CellInfoNr blocks found)' -ForegroundColor DarkGray
        return
    }

    for ($i = 0; $i -lt $nrBlocks.Count; $i++) {
        $rows = @(Get-CellInfoNrDebugRows -NrBlock $nrBlocks[$i] -Index ($i + 1))
        Write-AlignedFieldSection -Title ("CellInfoNr #{0}" -f ($i + 1)) -Fields $rows -TitleColor DarkCyan
    }
}

function Write-TelephonyFormattedBlock {
    param(
        [object] $Row,
        [switch] $DebugCellInfoNr
    )
    $svcFields = @(Get-ServiceStateSummaryFields -Text $Row.mServiceState -CellInfoText $Row.mCellInfo)
    $cellFields = @(Get-CellInfoSummaryFields -Text $Row.mCellInfo)
    Write-AlignedFieldSection -Title 'mServiceState' -Fields $svcFields
    Write-AlignedFieldSection -Title 'mCellInfo' -Fields $cellFields
    if ($DebugCellInfoNr) {
        Write-CellInfoNrDebugSection -CellInfoText $Row.mCellInfo
    }
}

$AdbExe = Get-AdbExe -Path $AdbPath

$serialTargets = [System.Collections.Generic.List[string]]::new()
if (-not [string]::IsNullOrWhiteSpace($Serial2)) {
    if ([string]::IsNullOrWhiteSpace($Serial)) {
        Write-Error 'Serial2 requires Serial (-s) for the first device.'
    }
    $null = $serialTargets.Add($Serial.Trim())
    $null = $serialTargets.Add($Serial2.Trim())
}
elseif (-not [string]::IsNullOrWhiteSpace($Serial)) {
    $null = $serialTargets.Add($Serial.Trim())
}
else {
    $null = $serialTargets.Add('')
}

$dualSerial = ($serialTargets.Count -gt 1)

Write-Host 'Turning airplane mode OFF...' -ForegroundColor Yellow
foreach ($s in $serialTargets) {
    Set-AirplaneMode -Adb $AdbExe -Serial $s -Enable $false
}
Write-Host 'Airplane mode -> OFF' -ForegroundColor Green

Write-Host ""
if ($Watch) {
    if ($dualSerial) {
        Write-Host ("Watch mode: two devices (-s {0}, {1}); screen clears each refresh. Ctrl+C to stop." -f $serialTargets[0], $serialTargets[1]) -ForegroundColor DarkGray
    } else {
        Write-Host 'Watch mode: screen clears each refresh (like watch). Ctrl+C to stop.' -ForegroundColor DarkGray
    }
}
elseif ($dualSerial) {
    Write-Host "Dual polling: dumpsys telephony.registry every $IntervalSeconds s on $($serialTargets[0]) and $($serialTargets[1]), Phone Id=$PhoneId. Ctrl+C to stop." -ForegroundColor DarkGray
}
else {
    Write-Host "Polling: adb shell dumpsys telephony.registry (every $IntervalSeconds s), Phone Id=$PhoneId. Ctrl+C to stop." -ForegroundColor DarkGray
}
if ($Count -gt 0) {
    Write-Host "Samples: $Count" -ForegroundColor DarkGray
}
if ($DebugCellInfoNr) {
    Write-Host 'Debug mode: print each CellInfoNr block with key fields.' -ForegroundColor DarkGray
}

function Get-WatchHeaderSeparatorWidth {
    try {
        $w = [Console]::WindowWidth
        if ($w -gt 2) {
            return [math]::Max(40, $w - 1)
        }
    } catch {
        # Non-interactive host
    }
    return 72
}

$n = 0
while ($true) {
    $n++
    $ts = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    if ($Watch) {
        Clear-Host
        $every = if ($IntervalSeconds -eq [math]::Truncate($IntervalSeconds)) {
            '{0}s' -f [int]$IntervalSeconds
        } else {
            '{0}s' -f $IntervalSeconds
        }
        $serialPart = if ($dualSerial) {
            " -s $($serialTargets[0]) / $($serialTargets[1])"
        }
        elseif ($serialTargets[0]) {
            " -s $($serialTargets[0])"
        }
        else {
            ''
        }
        Write-Host ("Every {0}: adb shell dumpsys telephony.registry{1} | Phone Id={2}" -f $every, $serialPart, $PhoneId) -ForegroundColor Yellow
        Write-Host (Get-Date -Format 'ddd MMM d HH:mm:ss yyyy') -ForegroundColor DarkGray
        Write-Host ('-' * (Get-WatchHeaderSeparatorWidth)) -ForegroundColor DarkGray
    } else {
        Write-Host ""
        Write-Host ('=' * 72) -ForegroundColor DarkGray
        Write-Host "  $ts  (sample #$n)" -ForegroundColor White
    }

    foreach ($adbSerial in $serialTargets) {
        $tag = if ($dualSerial) {
            if ($adbSerial) { $adbSerial } else { '(default)' }
        }
        else {
            $null
        }
        try {
            if ($dualSerial) {
                Write-Host ""
                Write-Host ("========== adb -s {0} ==========" -f $tag) -ForegroundColor Cyan
            }
            $dump = Get-DumpsysTelephonyRegistryText -Adb $AdbExe -Serial $adbSerial
            $section = Get-LastKnownStateSection -DumpText $dump
            if ($null -eq $section) {
                $wp = if ($tag) { "[$tag] " } else { '' }
                Write-Warning ($wp + 'Block "last known state:" not found in dumpsys output.')
            } else {
                $rows = Parse-TelephonyLastKnownFields -SectionText $section
                $rows = @($rows)
                if ($rows.Count -eq 0) {
                    $wp = if ($tag) { "[$tag] " } else { '' }
                    Write-Warning ($wp + 'No Phone Id= blocks parsed under last known state.')
                } else {
                    $picked = @($rows | Where-Object { [int] $_.PhoneId -eq $PhoneId })
                    if ($picked.Count -eq 0) {
                        $wp = if ($tag) { "[$tag] " } else { '' }
                        $avail = ($rows | ForEach-Object { $_.PhoneId }) -join ', '
                        Write-Warning ("{0}Phone Id={1} not found in last known state (available: {2})." -f $wp, $PhoneId, $avail)
                    }
                    foreach ($r in $picked) {
                        Write-Host ""
                        Write-Host ("  --- Phone Id={0} ---" -f $r.PhoneId) -ForegroundColor Magenta
                        Write-TelephonyFormattedBlock -Row $r -DebugCellInfoNr:$DebugCellInfoNr
                    }
                }
            }
        } catch {
            Write-Warning $(if ($tag) { "[$tag] $($_.Exception.Message)" } else { $_.Exception.Message })
        }
    }

    if ($Count -gt 0 -and $n -ge $Count) {
        break
    }
    Start-Sleep -Seconds $IntervalSeconds
}
