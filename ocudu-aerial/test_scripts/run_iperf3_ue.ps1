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

param(
    [string]$Serial = "",
    [string]$Serial2 = "",
    [switch]$Dual,
    [string]$IperfPath = "/data/local/tmp/iperf3.20",
    [string]$ServerBindIp = "10.45.0.1",
    [int]$UlPort = 5201,
    [int]$DlPort = 5202,
    [int]$UlPort2 = 5203,
    [int]$DlPort2 = 0,
    [int]$TestSeconds = 300,
    [int]$StopGraceSec = 5,
    [string]$Bandwidth = "200M",
    [int]$PacketLength = 1200,
    [ValidateSet("both", "ul", "dl")]
    [string]$Mode = "both",
    [int]$ReadyTimeoutSec = 120,
    [int]$ReadyIntervalSec = 1,
    [int]$ClientStartDelaySec = 10
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Serial)) {
    $adbOk = @(adb devices 2>$null | Where-Object { $_ -match '^\S+\s+device\s*$' })
    if ($adbOk.Count -ne 1) {
        throw "-Serial is empty: specify -Serial, or connect exactly one adb device (currently $($adbOk.Count) in 'device' state)."
    }
    $Serial = ($adbOk[0] -split '\s+')[0]
}

$dl2Effective = if ($DlPort2 -le 0) { $DlPort } else { $DlPort2 }

$devices = [System.Collections.Generic.List[hashtable]]::new()
$null = $devices.Add(@{ Serial = $Serial; Ul = $UlPort; Dl = $DlPort })
if ($Dual) {
    if ([string]::IsNullOrWhiteSpace($Serial2)) {
        throw "-Dual requires -Serial2 (second adb device serial)."
    }
    $null = $devices.Add(@{ Serial = $Serial2.Trim(); Ul = $UlPort2; Dl = $dl2Effective })
}

function Build-UECommands {
    param([int]$DlP, [int]$UlP)
    $serverCmd = "$IperfPath -s -p $DlP -i 1"
    $clientCmd = "$IperfPath -c $ServerBindIp -p $UlP -t $TestSeconds -u -b $Bandwidth -i 1"
    if ($PacketLength -gt 0) {
        $clientCmd += " -l $PacketLength"
    }
    $clientCmd += " --repeating-payload"
    return @{ Server = $serverCmd; Client = $clientCmd }
}

# Wrap remote argv in sh -c '...' so toybox sh does not split -t / -u across tokens (short runs ~1s).
function ShQuoteSingle {
    param([string]$Inner)
    return "'" + ($Inner -replace "'", "'\''") + "'"
}

function Invoke-AdbShellCmd {
    param([string]$Serial, [string]$Cmd)
    $remote = "sh -c $(ShQuoteSingle $Cmd)"
    & adb @('-s', $Serial, 'shell', $remote)
}

function Start-AdbShellJob {
    param([string]$Serial, [string]$Cmd)
    $fn = 'iperf_' + [Guid]::NewGuid().ToString('N').Substring(0, 12) + '.sh'
    $remotePath = "/data/local/tmp/$fn"
    $localTemp = [IO.Path]::GetTempFileName()
    try {
        $content = "#!/system/bin/sh`nexec $Cmd`n"
        $enc = New-Object System.Text.UTF8Encoding $false
        [IO.File]::WriteAllText($localTemp, $content, $enc)
        # adb builds differ in where they write progress; keep stdout out of the returned Job.
        & adb @('-s', $Serial, 'push', $localTemp, $remotePath) | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "adb push failed for $Serial -> $remotePath"
        }
        & adb @('-s', $Serial, 'shell', "chmod 700 $remotePath") | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "adb chmod failed on $Serial for $remotePath"
        }
    }
    finally {
        Remove-Item -LiteralPath $localTemp -Force -ErrorAction SilentlyContinue
    }
    Start-Job -ScriptBlock {
        param($serial, $rpath)
        try {
            # Explicit sh: some builds treat "adb shell /path" poorly; shebang alone is unreliable.
            & adb @('-s', $serial, 'shell', "sh $rpath")
        }
        finally {
            & adb @('-s', $serial, 'shell', "rm -f $rpath") 2>$null | Out-Null
        }
    } -ArgumentList $Serial, $remotePath
}

function Test-AdbTcpListen {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Serial,
        [Parameter(Mandatory = $true)]
        [int]$Port
    )
    $portHex = ("{0:X4}" -f $Port).ToUpperInvariant()
    $raw = adb -s $Serial shell "cat /proc/net/tcp /proc/net/tcp6" 2>$null
    if (-not $raw) {
        return $false
    }
    foreach ($line in ($raw -split "`r?`n")) {
        $t = $line.Trim()
        if (-not $t) { continue }
        if ($t -match '^sl\s') { continue }
        $parts = @($t -split '\s+')
        if ($parts.Length -lt 4) { continue }
        if ($parts[3].ToUpperInvariant() -ne "0A") { continue }
        $local = $parts[1]
        if ($local -match ':([0-9A-Fa-f]{4})$') {
            if ($matches[1].ToUpperInvariant() -eq $portHex) {
                return $true
            }
        }
    }
    return $false
}

function Wait-AdbTcpListen {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Serial,
        [Parameter(Mandatory = $true)]
        [int]$Port,
        [Parameter(Mandatory = $true)]
        [System.Management.Automation.Job]$ServerJob,
        [string]$Label = ""
    )
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
    $tag = if ($Label) { $Label } else { "${Serial}:${Port}" }
    Write-Host "Waiting for UE TCP listen: $tag ..."
    while ((Get-Date) -lt $deadline) {
        if ($ServerJob.State -ne "Running") {
            Receive-JobOutput -Job $ServerJob -Prefix $Serial
            throw "UE server job exited before listening on TCP port $Port ($Serial); state: $($ServerJob.State)."
        }
        if (Test-AdbTcpListen -Serial $Serial -Port $Port) {
            Write-Host "Ready (listen): $tag"
            return
        }
        Start-Sleep -Seconds $ReadyIntervalSec
    }
    throw "Timeout waiting for UE to listen on TCP port $Port ($Serial)."
}

function Receive-JobOutput {
    param(
        [Parameter(Mandatory = $true)]
        [System.Management.Automation.Job]$Job,
        [string]$Prefix = ""
    )
    $jobErrors = @()
    $jobOutput = Receive-Job -Job $Job -ErrorAction SilentlyContinue -ErrorVariable +jobErrors

    foreach ($line in $jobOutput) {
        if ($Prefix) {
            Write-Host "[$Prefix] $line"
        } else {
            Write-Host $line
        }
    }

    foreach ($jobError in $jobErrors) {
        $msg = $jobError.ToString()
        if ($msg -notmatch "Remote PTY will not be allocated because stdin is not a terminal\.$") {
            Write-Warning $(if ($Prefix) { "[$Prefix] $msg" } else { $msg })
        }
    }
}

function Assert-ServerJobsAlive {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.Generic.List[object]]$ServerJobs
    )
    foreach ($sj in $ServerJobs) {
        if ($null -eq $sj.Job) { continue }
        if ((Get-Job -Id $sj.Job.Id -ErrorAction SilentlyContinue).State -eq "Running") { continue }
        Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
        throw "UE iperf3 server on $($sj.DevSerial) exited during the test window; DL results are incomplete."
    }
}

$runUl = $Mode -in @("both", "ul")
$runDl = $Mode -in @("both", "dl")
$serverJobs = [System.Collections.Generic.List[object]]::new()
$clientJobs = [System.Collections.Generic.List[object]]::new()

$useLinePrefix = $devices.Count -gt 1
if ($useLinePrefix) {
    Write-Host "Dual-UE mode: serials $($devices[0].Serial), $($devices[1].Serial) (UL $($devices[0].Ul) / $($devices[1].Ul), DL $($devices[0].Dl) / $($devices[1].Dl))."
}

if ($runDl) {
    foreach ($dev in $devices) {
        $cmds = Build-UECommands -DlP $dev.Dl -UlP $dev.Ul
        Write-Host "Starting UE iperf3 server on device $($dev.Serial) (DL, port $($dev.Dl)) ..."
        $sj = Start-AdbShellJob -Serial $dev.Serial -Cmd $cmds.Server
        $null = $serverJobs.Add([pscustomobject]@{ Job = $sj; DevSerial = $dev.Serial; Prefix = $(if ($useLinePrefix) { $dev.Serial } else { "" }) })
    }
}

try {
    if ($runUl) {
        foreach ($dev in $devices) {
            if ($runDl) {
                $sj = $serverJobs | Where-Object { $_.DevSerial -eq $dev.Serial } | Select-Object -First 1
                Wait-AdbTcpListen -Serial $dev.Serial -Port $dev.Dl -ServerJob $sj.Job -Label "$($dev.Serial) DL port $($dev.Dl)"
            }
        }
        if ($ClientStartDelaySec -gt 0) {
            Write-Host "Countdown before UE UL iperf3 client(s):"
            $rem = $ClientStartDelaySec
            while ($rem -gt 0) {
                [Console]::Write("`r  {0} s remaining ... ", $rem)
                Start-Sleep -Seconds 1
                $rem--
            }
            [Console]::Write("`r  starting UL client(s).                    ")
            [Console]::WriteLine()
        }
        foreach ($dev in $devices) {
            $cmds = Build-UECommands -DlP $dev.Dl -UlP $dev.Ul
            Write-Host "Starting UE iperf3 UDP client on $($dev.Serial) (UL -> ${ServerBindIp}:$($dev.Ul), -t $TestSeconds s) ..."
            $cj = Start-AdbShellJob -Serial $dev.Serial -Cmd $cmds.Client
            $null = $clientJobs.Add([pscustomobject]@{ Job = $cj; DevSerial = $dev.Serial; Prefix = $(if ($useLinePrefix) { $dev.Serial } else { "" }) })
        }

        $clientJobList = @($clientJobs | ForEach-Object { $_.Job })
        while (($clientJobList | ForEach-Object { (Get-Job -Id $_.Id).State }) -contains "Running") {
            if ($runDl) {
                Assert-ServerJobsAlive -ServerJobs $serverJobs
            }
            foreach ($sj in $serverJobs) {
                Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
            }
            foreach ($cj in $clientJobs) {
                Receive-JobOutput -Job $cj.Job -Prefix $cj.Prefix
            }
            Start-Sleep -Seconds 1
        }

        foreach ($cj in $clientJobs) {
            Wait-Job -Job $cj.Job | Out-Null
            Receive-JobOutput -Job $cj.Job -Prefix $cj.Prefix
        }

        if ($serverJobs.Count -gt 0 -and $StopGraceSec -gt 0) {
            Write-Host "Client finished. Waiting $StopGraceSec seconds before stopping UE server(s) ..."
            $graceEndAt = (Get-Date).AddSeconds($StopGraceSec)
            while ((Get-Date) -lt $graceEndAt) {
                foreach ($sj in $serverJobs) {
                    Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
                }
                Start-Sleep -Seconds 1
            }
        }
    }
    elseif ($runDl) {
        foreach ($dev in $devices) {
            $sj = $serverJobs | Where-Object { $_.DevSerial -eq $dev.Serial } | Select-Object -First 1
            Wait-AdbTcpListen -Serial $dev.Serial -Port $dev.Dl -ServerJob $sj.Job -Label "$($dev.Serial) DL port $($dev.Dl)"
        }
        Write-Host "DL-only mode: collecting UE server logs for $TestSeconds seconds ..."
        $endAt = (Get-Date).AddSeconds($TestSeconds)
        while ((Get-Date) -lt $endAt) {
            Assert-ServerJobsAlive -ServerJobs $serverJobs
            foreach ($sj in $serverJobs) {
                Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
            }
            Start-Sleep -Seconds 1
        }

        if ($StopGraceSec -gt 0) {
            Write-Host "Waiting additional $StopGraceSec seconds before stopping UE server(s) ..."
            $graceEndAt = (Get-Date).AddSeconds($StopGraceSec)
            while ((Get-Date) -lt $graceEndAt) {
                Assert-ServerJobsAlive -ServerJobs $serverJobs
                foreach ($sj in $serverJobs) {
                    Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
                }
                Start-Sleep -Seconds 1
            }
        }
    }

    foreach ($sj in $serverJobs) {
        Receive-JobOutput -Job $sj.Job -Prefix $sj.Prefix
    }
}
finally {
    if ($runDl) {
        foreach ($sj in $serverJobs) {
            $serverState = $null
            if ($null -ne $sj.Job) {
                $serverState = (Get-Job -Id $sj.Job.Id -ErrorAction SilentlyContinue).State
            }

            if ($serverState -eq "Running") {
                Write-Host "Stopping UE iperf3 server on $($sj.DevSerial) (still running) ..."
                Invoke-AdbShellCmd -Serial $sj.DevSerial -Cmd "pkill -f -- $(ShQuoteSingle $IperfPath)" | Out-Null
            }
            else {
                Write-Host "UE iperf3 server on $($sj.DevSerial) already exited naturally."
            }
        }
    }

    foreach ($cj in $clientJobs) {
        if ($null -ne $cj.Job) {
            Stop-Job -Job $cj.Job -ErrorAction SilentlyContinue
            Remove-Job -Job $cj.Job -Force -ErrorAction SilentlyContinue
        }
    }

    foreach ($sj in $serverJobs) {
        if ($null -ne $sj.Job) {
            Stop-Job -Job $sj.Job -ErrorAction SilentlyContinue
            Remove-Job -Job $sj.Job -Force -ErrorAction SilentlyContinue
        }
    }
}
