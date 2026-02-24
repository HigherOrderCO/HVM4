#Requires -Version 5.1
# scripts/test.ps1
# =================
# Unified test runner for HVM4 on Windows.
# Runs interpreted and AOT-compiled modes in sequence.
#
# Test format:
#   @main = <expression>
#   //<expected output>
#
# For multi-line expected output, use multiple // lines.
# Tests starting with _ are skipped.
# Per-test CLI flags can be set with one leading directive line:
#   //!--flag1 --flag2

param(
    [switch]$InterpretedOnly,
    [switch]$Help
)

if ($Help) {
    @"
usage: scripts\test.ps1 [-InterpretedOnly] [-Help]

Options:
  -InterpretedOnly   Run interpreted tests only (skip AOT tests)
  -Help              Show this help message
"@ | Write-Host
    exit 0
}

# Config
# ------

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $PSCommandPath
$RootDir = Resolve-Path (Join-Path $ScriptDir "..")
$TestDir = Join-Path $RootDir "test"
$FfiDir = Join-Path $TestDir "ffi"
$CMain = Join-Path $RootDir "clang\main.c"
$CBin = Join-Path $RootDir "clang\main.exe"

$TestTimeoutInterpretedSecs = 2
$TestTimeoutCompiledSecs = 20

if ($env:HVM_TEST_TIMEOUT_SECS) {
    $TestTimeoutInterpretedSecs = [int]$env:HVM_TEST_TIMEOUT_SECS
    $TestTimeoutCompiledSecs = [int]$env:HVM_TEST_TIMEOUT_SECS
}

function Get-FfiClangConfig {
    if ($IsMacOS) {
        return @{ Ext = ".dylib"; LdFlags = @("-dynamiclib", "-fPIC") }
    }
    if ($IsWindows) {
        return @{ Ext = ".dll"; LdFlags = @("-shared") }
    }
    return @{ Ext = ".so"; LdFlags = @("-shared", "-fPIC") }
}

# Build
# -----

# Detect available compiler
function Find-Compiler {
    # Check for clang first
    $clang = Get-Command "clang" -ErrorAction SilentlyContinue
    if ($clang) {
        return @{ Name = "clang"; Cmd = "clang"; Args = @("-O2", "-o", $CBin, $CMain) }
    }
    
    # Check for MSVC cl
    $cl = Get-Command "cl" -ErrorAction SilentlyContinue
    if ($cl) {
        return @{ 
            Name = "cl"; 
            Cmd = "cl"; 
            Args = @("/O2", "/Fe:$CBin", $CMain, "/link", "/SUBSYSTEM:CONSOLE")
        }
    }
    
    throw "No C compiler found. Please install clang or Visual Studio with MSVC."
}

# Compiles the C binary
function Build-Binary {
    if (-not (Test-Path $CMain)) {
        throw "Expected C entrypoint at $CMain"
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
    
    if (-not (Test-Path $CBin)) {
        throw "Build failed - output not found"
    }
}

# Collect
# -------

# Tracks generated artifact files for cleanup
$script:cleanupFiles = [System.Collections.ArrayList]::new()

function Register-Cleanup {
    param([string]$Path)
    [void]$script:cleanupFiles.Add($Path)
}

function Clear-Cleanup {
    foreach ($file in $script:cleanupFiles) {
        if (Test-Path $file) {
            Remove-Item -Path $file -Force -ErrorAction SilentlyContinue
        }
    }
    $script:cleanupFiles.Clear()
}

# Collect test files
function Get-TestFiles {
    $tests = @()
    
    # Main tests
    if (Test-Path $TestDir) {
        Get-ChildItem -Path $TestDir -Filter "*.hvm" | ForEach-Object {
            if (-not $_.Name.StartsWith("_")) {
                $tests += $_.FullName
            }
        }
    }
    
    # FFI tests
    if (Test-Path $FfiDir) {
        Get-ChildItem -Path $FfiDir -Filter "*.hvm" | ForEach-Object {
            if (-not $_.Name.StartsWith("_")) {
                $tests += $_.FullName
            }
        }
    }
    
    return $tests | Sort-Object
}

# Run
# ---

# Runs a command with a timeout
function Invoke-WithTimeout {
    param(
        [string]$Command,
        [array]$Arguments,
        [int]$TimeoutSecs
    )

    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()

    try {
        $proc = Start-Process -FilePath $Command -ArgumentList $Arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile

        if (-not $proc.WaitForExit($TimeoutSecs * 1000)) {
            try { $proc.Kill() } catch {}
            try { $proc.WaitForExit() } catch {}
            return @{ Output = ""; ExitCode = 124 }
        }

        $stdout = ""
        $stderr = ""
        if (Test-Path $stdoutFile) {
            $stdout = [string](Get-Content -Path $stdoutFile -Raw -Encoding UTF8)
        }
        if (Test-Path $stderrFile) {
            $stderr = [string](Get-Content -Path $stderrFile -Raw -Encoding UTF8)
        }
        $stdout = ([string]$stdout) -replace "(\r?\n)+$", ""
        $stderr = ([string]$stderr) -replace "(\r?\n)+$", ""

        $combined = ""
        if (-not [string]::IsNullOrEmpty($stderr) -and -not [string]::IsNullOrEmpty($stdout)) {
            $combined = "$stderr`n$stdout"
        } elseif (-not [string]::IsNullOrEmpty($stderr)) {
            $combined = $stderr
        } else {
            $combined = $stdout
        }

        $combined = [string]$combined
        return @{ Output = $combined.Trim(); ExitCode = [int]$proc.ExitCode }
    } finally {
        Remove-Item -Path $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    }
}

# Strips ANSI escape codes
function Strip-Ansi {
    param([string]$Text)
    $Text -replace "`e\[[0-9;]*m", "" -replace "\x1b\[[0-9;]*m", ""
}

# Builds FFI library for Windows
function Build-FfiLibrary {
    param([string]$TestFile)
    
    $base = [System.IO.Path]::GetFileNameWithoutExtension($TestFile)
    $testDir = Split-Path -Parent $TestFile
    $ffiDir = Join-Path $testDir $base
    $compiler = Find-Compiler
    $ffiClang = Get-FfiClangConfig
    $ffiExt = $ffiClang.Ext
    $ffiLdFlags = $ffiClang.LdFlags
    $ffiExportDefineClang = "-Dhvm_ffi_init=__declspec(dllexport)hvm_ffi_init"
    $ffiExportDefineMsvc = "/Dhvm_ffi_init=__declspec(dllexport)hvm_ffi_init"
    
    if (Test-Path $ffiDir -PathType Container) {
        # Multiple C files in subdirectory
        $cFiles = Get-ChildItem -Path $ffiDir -Filter "*.c"
        if ($cFiles.Count -eq 0) {
            throw "No .c files under $ffiDir"
        }
        
        $dlls = @()
        foreach ($src in $cFiles) {
            $out = Join-Path $src.DirectoryName "$($src.BaseName)$ffiExt"
            Register-Cleanup $out
            
            if ($compiler.Name -eq "clang") {
                & $compiler.Cmd @ffiLdFlags $ffiExportDefineClang "-I" $RootDir "-o" $out $src.FullName 2>&1 | Write-Host
            } else {
                # MSVC
                & $compiler.Cmd "/LD" $ffiExportDefineMsvc "/Fe:$out" "/I" $RootDir $src.FullName 2>&1 | Write-Host
            }
            
            if ($LASTEXITCODE -ne 0) {
                throw "Failed to build $($src.FullName)"
            }
            $dlls += $out
        }
        
        return @{ Flag = "--ffi-dir"; Target = $ffiDir }
    } else {
        # Single C file
        $src = Join-Path $testDir "$base.c"
        $out = Join-Path $testDir "$base$ffiExt"
        Register-Cleanup $out
        
        if (-not (Test-Path $src)) {
            throw "Missing $src"
        }
        
        if ($compiler.Name -eq "clang") {
            & $compiler.Cmd @ffiLdFlags $ffiExportDefineClang "-I" $RootDir "-o" $out $src 2>&1 | Write-Host
        } else {
            # MSVC
            & $compiler.Cmd "/LD" $ffiExportDefineMsvc "/Fe:$out" "/I" $RootDir $src 2>&1 | Write-Host
        }
        
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to build $src"
        }
        
        return @{ Flag = "--ffi"; Target = $out }
    }
}

# Runs all collected tests against the given binary
function Run-Tests {
    param(
        [string]$Bin,
        [string]$Label,
        [int]$TimeoutSecs,
        [array]$ExtraArgs = @()
    )
    
    $tests = Get-TestFiles
    if ($tests.Count -eq 0) {
        throw "No .hvm files found"
    }
    
    $status = 0
    Write-Host "`n=== Testing $Label ==="
    
    foreach ($testFile in $tests) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($testFile)
        
        try {
            # Read leading //!... directive lines as CLI flags
            $extraFlags = @()
            $content = Get-Content -Path $testFile -Raw
            $lines = $content -split "`r?`n"
            
            foreach ($line in $lines) {
                if ($line -match "^//!(.*)$") {
                    $flagLine = $matches[1].Trim()
                    if (-not [string]::IsNullOrEmpty($flagLine)) {
                        $extraFlags += $flagLine -split "\s+"
                    }
                } elseif (-not [string]::IsNullOrWhiteSpace($line)) {
                    break
                }
            }
            
            # Extract trailing // comment lines (consecutive from end)
            $expected = ""
            $nlinesExpected = 0
            $nocollapse = $false
            $expectPrefix = ""
            $expectContains = ""
            
            for ($i = $lines.Count - 1; $i -ge 0; $i--) {
                $line = $lines[$i]
                if ([string]::IsNullOrWhiteSpace($line)) {
                    if ($nlinesExpected -eq 0 -and [string]::IsNullOrEmpty($expectPrefix) -and [string]::IsNullOrEmpty($expectContains)) {
                        continue
                    }
                    break
                }
                if ($line -match "^//EXPECT_PREFIX:(.+)") {
                    $expectPrefix = $matches[1].Trim()
                    continue
                }
                if ($line -match "^//EXPECT_CONTAINS:(.+)") {
                    $expectContains = $matches[1].Trim()
                    continue
                }
                if ($line -match "^//!") {
                    $nocollapse = $true
                    $content = $line -replace "^//!", ""
                } elseif ($line -match "^//(.*)$") {
                    $content = $matches[1]
                } else {
                    break
                }
                
                if ([string]::IsNullOrWhiteSpace($expected)) {
                    $expected = $content
                } else {
                    $expected = "$content`n$expected"
                }
                $nlinesExpected++
            }
            
            # For collapse_* and enum_* tests, infer limit from expected output lines
            $collapseCount = ""
            if ($name -match "^(collapse_|enum_)") {
                $collapseCount = $nlinesExpected
            }
            
            if ($nlinesExpected -eq 0 -and [string]::IsNullOrEmpty($expectPrefix) -and [string]::IsNullOrEmpty($expectContains)) {
                Write-Host "[FAIL] $name (missing expected result comment)" -ForegroundColor Red
                $status = 1
                continue
            }
            
            # Determine flags: all tests use -C by default unless //! is used
            $flags = @()
            if (-not $nocollapse) {
                if ([string]::IsNullOrEmpty($collapseCount)) {
                    $flags += "-C"
                } else {
                    $flags += "-C$collapseCount"
                }
            }
            
            # Build FFI flags if applicable
            $ffiArgs = @()
            if ($testFile -like "*$FfiDir*") {
                try {
                    $ffiInfo = Build-FfiLibrary $testFile
                    $ffiArgs += $ffiInfo.Flag
                    $ffiArgs += $ffiInfo.Target
                } catch {
                    Write-Host "[FAIL] $name (FFI build error: $_)" -ForegroundColor Red
                    $status = 1
                    continue
                }
            }
            
            # Assemble the command
            $cmdArgs = @($testFile) + $flags + $extraFlags + $ffiArgs + $ExtraArgs
            
            # Execute and compare
            $result = Invoke-WithTimeout -Command $Bin -Arguments $cmdArgs -TimeoutSecs $TimeoutSecs
            $actual = $result.Output
            
            if ($result.ExitCode -eq 124) {
                Write-Host "[FAIL] $name (timeout after ${TimeoutSecs}s)" -ForegroundColor Red
                $status = 1
                continue
            }
            
            # Strip ANSI escape codes for comparison
            $actualClean = (Strip-Ansi $actual) -replace "`r`n?", "`n"
            $expectedClean = (Strip-Ansi $expected) -replace "`r`n?", "`n"
            
            # Compare output
            $pass = $false
            if ($expectPrefix) {
                $pass = $actualClean.StartsWith($expectPrefix)
                if (-not $pass) {
                    Write-Host "[FAIL] $name" -ForegroundColor Red
                    Write-Host "  expected prefix: $expectPrefix"
                    Write-Host "  detected: $actualClean"
                }
            } elseif ($expectContains) {
                $pass = $actualClean.Contains($expectContains)
                if (-not $pass) {
                    Write-Host "[FAIL] $name" -ForegroundColor Red
                    Write-Host "  expected to contain: $expectContains"
                    Write-Host "  detected: $actualClean"
                }
            } elseif ($expectedClean -eq "PARSE_ERROR") {
                $pass = $actualClean.StartsWith("PARSE_ERROR")
                if (-not $pass) {
                    Write-Host "[FAIL] $name" -ForegroundColor Red
                    Write-Host "  expected: PARSE_ERROR"
                    Write-Host "  detected: $actualClean"
                }
            } elseif ($actualClean -eq $expectedClean) {
                $pass = $true
            } else {
                Write-Host "[FAIL] $name" -ForegroundColor Red
                Write-Host "  expected: $expected"
                Write-Host "  detected: $actual"
            }
            
            if ($pass) {
                Write-Host "[PASS] $name" -ForegroundColor Green
            } else {
                $status = 1
            }
        } catch {
            Write-Host "[FAIL] $name (error: $_)" -ForegroundColor Red
            $status = 1
        }
    }
    
    Write-Host ""
    return $status
}

# Main
# ----

try {
    Build-Binary
    
    $overallStatus = 0
    
    $sharedFlags = @()
    if ($env:HVM_TEST_FLAGS) {
        $sharedFlags = $env:HVM_TEST_FLAGS -split "\s+"
    }
    
    $result = Run-Tests -Bin $CBin -Label "HVM (interpreted)" -TimeoutSecs $TestTimeoutInterpretedSecs -ExtraArgs $sharedFlags
    if ($result -ne 0) {
        $overallStatus = 1
    }
    
    if (-not $InterpretedOnly) {
        $aotArgs = @("--as-c") + $sharedFlags
        $result = Run-Tests -Bin $CBin -Label "HVM (AOT)" -TimeoutSecs $TestTimeoutCompiledSecs -ExtraArgs $aotArgs
        if ($result -ne 0) {
            $overallStatus = 1
        }
    }
    
    Clear-Cleanup
    
    if ($overallStatus -eq 0) {
        Write-Host "All tests passed!" -ForegroundColor Green
        exit 0
    } else {
        exit 1
    }
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    Clear-Cleanup
    exit 1
}
