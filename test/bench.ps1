#Requires -Version 5.1
# scripts/bench.ps1
# =================
# Benchmark runner for HVM4 on Windows.
# Clones/caches the bench repo, builds the C binary, and runs
# benchmarks across multiple thread counts in a table format.

param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsList
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RootDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$CacheDir = Join-Path $RootDir ".cache\bench"
$BenchDir = Join-Path $CacheDir "bench"
$MainC = Join-Path $RootDir "clang\main.c"
$MainExe = Join-Path $RootDir "clang\main.exe"
$BenchRepo = "https://github.com/HigherOrderCO/bench.git"
$TimeoutSecs = if ($env:TIMEOUT) { [int]$env:TIMEOUT } else { 10 }
$Threads = @(1, 2, 4, 8, 12)
$ShowIps = $false
$Mode = ""

function Show-Help {
    @"
Usage: scripts\bench.ps1 [--interpreted | --compiled] [--ips] [-TN]

  --interpreted  Run benchmarks via the C interpreter
  --compiled     Run benchmarks via AOT compilation (--as-c)
  --ips          Show interactions/s instead of time
  -TN            Run only with N threads (example: -T1)

The bench repo is cloned/cached at .cache\bench\.
"@ | Write-Host
}

function Find-Compiler {
    $clang = Get-Command "clang" -ErrorAction SilentlyContinue
    if ($clang) {
        return @{
            Name = "clang"
            Cmd = "clang"
            Args = @("-O2", "-o", $MainExe, $MainC)
        }
    }

    $cl = Get-Command "cl" -ErrorAction SilentlyContinue
    if ($cl) {
        return @{
            Name = "cl"
            Cmd = "cl"
            Args = @("/O2", "/Fe:$MainExe", $MainC, "/link", "/SUBSYSTEM:CONSOLE")
        }
    }

    throw "No C compiler found. Please install clang or Visual Studio with MSVC."
}

function Invoke-WithTimeout {
    param(
        [string]$Command,
        [string[]]$Arguments,
        [int]$Timeout
    )

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()

    try {
        $proc = Start-Process -FilePath $Command -ArgumentList $Arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

        if (-not $proc.WaitForExit($Timeout * 1000)) {
            try { $proc.Kill() } catch {}
            try { $proc.WaitForExit() } catch {}
            return @{ ExitCode = 124; Output = "" }
        }

        $stdout = if (Test-Path $stdoutFile) { [string](Get-Content -Path $stdoutFile -Raw -Encoding UTF8) } else { "" }
        $stderr = if (Test-Path $stderrFile) { [string](Get-Content -Path $stderrFile -Raw -Encoding UTF8) } else { "" }
        $stdout = ([string]$stdout) -replace "(\r?\n)+$", ""
        $stderr = ([string]$stderr) -replace "(\r?\n)+$", ""

        $combined = if (-not [string]::IsNullOrEmpty($stderr) -and -not [string]::IsNullOrEmpty($stdout)) {
            "$stderr`n$stdout"
        } elseif (-not [string]::IsNullOrEmpty($stderr)) {
            $stderr
        } else {
            $stdout
        }

        return @{ ExitCode = [int]$proc.ExitCode; Output = ([string]$combined).Trim() }
    } finally {
        Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

function Sync-BenchRepo {
    if (Test-Path (Join-Path $CacheDir ".git")) {
        Write-Host "Updating bench repo..."
        Push-Location $CacheDir
        try {
            & git pull --quiet
            if ($LASTEXITCODE -ne 0) {
                throw "git pull failed"
            }
        } finally {
            Pop-Location
        }
    } else {
        Write-Host "Cloning bench repo..."
        $parent = Split-Path -Parent $CacheDir
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        & git clone --quiet $BenchRepo $CacheDir
        if ($LASTEXITCODE -ne 0) {
            throw "git clone failed"
        }
    }
}

function Build-Main {
    if (-not (Test-Path $MainC)) {
        throw "Expected C entrypoint at $MainC"
    }

    $compiler = Find-Compiler
    Write-Host "Building clang\main.exe using $($compiler.Name)..."
    Push-Location (Join-Path $RootDir "clang")
    try {
        & $compiler.Cmd @($compiler.Args) 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed"
        }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $MainExe)) {
        throw "Build failed - output not found"
    }
}

function Get-Metric {
    param([string]$OutputText)

    if ($ShowIps) {
        $line = ($OutputText -split "\r?\n" | Where-Object { $_ -match "^- Perf:" } | Select-Object -First 1)
        if (-not $line) { return "n/a" }
        $parts = ($line -replace "^- Perf:\s*", "") -split "\s+"
        if ($parts.Count -lt 1 -or [string]::IsNullOrEmpty($parts[0])) { return "n/a" }
        if ($parts.Count -ge 2 -and -not [string]::IsNullOrEmpty($parts[1])) { return "$($parts[0])$($parts[1])" }
        return $parts[0]
    }

    $timeLine = ($OutputText -split "\r?\n" | Where-Object { $_ -match "^- Time:" } | Select-Object -First 1)
    if (-not $timeLine) { return "n/a" }
    $parts = ($timeLine -replace "^- Time:\s*", "") -split "\s+"
    if ($parts.Count -lt 1 -or [string]::IsNullOrEmpty($parts[0])) { return "n/a" }
    return "$($parts[0])s"
}

function Run-Benchmarks {
    $benchFiles = @(Get-ChildItem -Path $BenchDir -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "main.hvm" } |
        Where-Object { Test-Path $_ } |
        Sort-Object)

    if ($benchFiles.Count -eq 0) {
        throw "No benchmarks found under $BenchDir\*\main.hvm"
    }

    $nameW = 4
    foreach ($file in $benchFiles) {
        $name = Split-Path -Leaf (Split-Path -Parent $file)
        if ($name.Length -gt $nameW) { $nameW = $name.Length }
    }
    $nameW += 2

    Write-Host -NoNewline ("{0,-$nameW}" -f "bench")
    foreach ($t in $Threads) {
        Write-Host -NoNewline ("{0,8}" -f "T$t")
    }
    Write-Host ""

    $modeFlags = @()
    if ($Mode -eq "compiled") {
        $modeFlags += "--as-c"
    }

    foreach ($file in $benchFiles) {
        $name = Split-Path -Leaf (Split-Path -Parent $file)
        Write-Host -NoNewline ("{0,-$nameW}" -f $name)

        foreach ($t in $Threads) {
            $extraArgs = @()
            if ($name -like "gen_*") {
                $extraArgs += "-C1"
            } elseif ($name -like "collapse_*") {
                $extraArgs += "-C"
            }

            $args = @($file, "-s", "-S", "-T$t") + $modeFlags + $extraArgs
            $result = Invoke-WithTimeout -Command $MainExe -Arguments $args -Timeout $TimeoutSecs

            $val = if ($result.ExitCode -eq 124) {
                "timeout"
            } elseif ($result.ExitCode -ne 0) {
                "error"
            } else {
                Get-Metric -OutputText $result.Output
            }

            Write-Host -NoNewline ("{0,8}" -f $val)
        }
        Write-Host ""
    }
}

foreach ($arg in $ArgsList) {
    switch -Regex ($arg) {
        "^--interpreted$" { $Mode = "interpreted" }
        "^--compiled$" { $Mode = "compiled" }
        "^--ips$" { $ShowIps = $true }
        "^-T[0-9]+$" {
            $n = [int]$arg.Substring(2)
            if ($n -le 0) {
                throw "Invalid thread count on '$arg'"
            }
            $Threads = @($n)
        }
        "^--help$|^-h$" {
            Show-Help
            exit 0
        }
        default {
            throw "Unknown flag '$arg'"
        }
    }
}

if ([string]::IsNullOrEmpty($Mode)) {
    Show-Help
    exit 1
}

try {
    Sync-BenchRepo
    Build-Main
    Run-Benchmarks
} catch {
    Write-Host "error: $_" -ForegroundColor Red
    exit 1
}
