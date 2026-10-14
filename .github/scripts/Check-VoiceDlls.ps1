<#
.SYNOPSIS
    Checks that braidcast.exe delay-loads voice control's speech engine DLLs.

.DESCRIPTION
    whisper.cpp's ggml-cpu is compiled for AVX2. braidcast.exe must start on a CPU
    without AVX2, so none of the speech engine's DLLs may be mapped when it starts:
    they are delay-loaded (frontend/cmake/voice-engine.cmake), and the app loads them
    itself (Voice::EnsureWhisperLoaded) only after its CPU check passes. A plain
    import of any of them would map ggml-cpu.dll, and run its initializers, at
    launch. This script reads the built exe with dumpbin and asserts:

      1. none of whisper.dll, ggml.dll, ggml-base.dll and ggml-cpu.dll is a
         load-time dependency of braidcast.exe (dumpbin /dependents and /imports);
      2. whisper.dll is a delay-load dependency, importing whisper_* functions;
      3. all four DLLs sit beside the exe, which is where the app loads them from;
      4. whisper.dll imports ggml.dll, i.e. they are the shared build, not stale
         files from somewhere else.

    Prints PASS or FAIL per check and exits 0 only when every check passes.

    If voice ever goes back to linking whisper statically, this check no longer
    applies and the audit it replaced (index risk R2) is owed again; see "Going back
    to a static link" in frontend/deps/README.md.

.PARAMETER ExePath
    The braidcast.exe to check. Defaults to the RelWithDebInfo rundir of the
    windows-x64 preset (build_x64/rundir/RelWithDebInfo/bin/64bit/braidcast.exe).

.PARAMETER Dumpbin
    Path to dumpbin.exe. Defaults to dumpbin on PATH (a VS developer shell), then to
    the newest Visual Studio or Build Tools install vswhere can find.

.EXAMPLE
    pwsh .github/scripts/Check-VoiceDlls.ps1

.EXAMPLE
    pwsh .github/scripts/Check-VoiceDlls.ps1 -ExePath C:\Braidcast\bin\64bit\braidcast.exe

.NOTES
    PowerShell 7+. Read-only: it changes nothing. Works on a rundir, an unzipped
    package or an installed copy alike.
#>
[CmdletBinding()]
param(
    [string] $ExePath,
    [string] $Dumpbin
)

$ErrorActionPreference = 'Stop'

# The speech engine's DLLs, in load order. Must match frontend/cmake/voice-engine.cmake
# and kWhisperDlls in frontend/src/voice/WhisperLibrary.hpp.
$VoiceDlls = @('ggml-base.dll', 'ggml-cpu.dll', 'ggml.dll', 'whisper.dll')

$script:Failures = 0

function Write-Result {
    param([bool] $Ok, [string] $Message)
    if ( $Ok ) {
        Write-Host "PASS: $Message" -ForegroundColor Green
    } else {
        Write-Host "FAIL: $Message" -ForegroundColor Red
        $script:Failures++
    }
}

function Find-Dumpbin {
    $onPath = Get-Command dumpbin -ErrorAction SilentlyContinue
    if ( $onPath ) {
        return $onPath.Source
    }
    if ( -not ${env:ProgramFiles(x86)} ) {
        return $null
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if ( Test-Path -LiteralPath $vswhere ) {
        $found = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
        if ( $found ) {
            return $found
        }
    }
    return $null
}

# dumpbin /dependents: the DLL names under "Image has the following dependencies" and
# under "Image has the following delay load dependencies".
function Get-Dependents {
    param([string] $Image)
    $result = @{ Normal = [System.Collections.Generic.List[string]]::new(); Delay = [System.Collections.Generic.List[string]]::new() }
    $mode = $null
    foreach ( $line in (& $script:DumpbinPath /nologo /dependents $Image) ) {
        if ( $line -match '^\s*Image has the following dependencies:' ) {
            $mode = 'Normal'
        } elseif ( $line -match '^\s*Image has the following delay load dependencies:' ) {
            $mode = 'Delay'
        } elseif ( $line -match '^\s*Summary\s*$' ) {
            $mode = $null
        } elseif ( $mode -and $line -match '^\s+(\S+\.dll)\s*$' ) {
            $result[$mode].Add($Matches[1])
        }
    }
    return $result
}

# dumpbin /imports: for each DLL, whether it is in the load-time import section or the
# delay-load one, and the function names imported from it.
function Get-Imports {
    param([string] $Image)
    $result = @{ Normal = @{}; Delay = @{} }
    $mode = $null
    $dll = $null
    foreach ( $line in (& $script:DumpbinPath /nologo /imports $Image) ) {
        if ( $line -match '^\s*Section contains the following imports:' ) {
            $mode = 'Normal'
            $dll = $null
        } elseif ( $line -match '^\s*Section contains the following delay load imports:' ) {
            $mode = 'Delay'
            $dll = $null
        } elseif ( $line -match '^\s*Summary\s*$' ) {
            $mode = $null
        } elseif ( $mode -and $line -match '^\s{4}(\S+\.dll)\s*$' ) {
            $dll = $Matches[1].ToLowerInvariant()
            $result[$mode][$dll] = [System.Collections.Generic.List[string]]::new()
        } elseif ( $mode -and $dll -and $line -match '^\s+[0-9A-Fa-f]+\s.*\s([A-Za-z_][A-Za-z0-9_@?$]*)\s*$' ) {
            # An entry line: hex fields, then the name last.
            $result[$mode][$dll].Add($Matches[1])
        }
    }
    return $result
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ( -not $ExePath ) {
    $ExePath = Join-Path $repoRoot 'build_x64/rundir/RelWithDebInfo/bin/64bit/braidcast.exe'
}
if ( -not ( Test-Path -LiteralPath $ExePath -PathType Leaf ) ) {
    Write-Host "FAIL: braidcast.exe not found at $ExePath (pass -ExePath)" -ForegroundColor Red
    exit 1
}
$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$exeDir = Split-Path -Parent $ExePath

$script:DumpbinPath = if ( $Dumpbin ) { $Dumpbin } else { Find-Dumpbin }
if ( -not $script:DumpbinPath ) {
    Write-Host 'FAIL: dumpbin.exe not found: run from a VS developer shell, or pass -Dumpbin' -ForegroundColor Red
    exit 1
}

Write-Host "Exe     : $ExePath"
Write-Host "dumpbin : $script:DumpbinPath"
Write-Host ''

# 1 and 2, from /dependents.
$deps = Get-Dependents -Image $ExePath
if ( $deps.Normal.Count -eq 0 ) {
    Write-Result $false 'dumpbin /dependents listed no load-time dependencies at all (unexpected output?)'
}
foreach ( $name in $VoiceDlls ) {
    $static = $deps.Normal | Where-Object { $_ -ieq $name }
    Write-Result (-not $static) "$name is not a load-time dependency of braidcast.exe"
}
$delayWhisper = $deps.Delay | Where-Object { $_ -ieq 'whisper.dll' }
Write-Result ([bool] $delayWhisper) 'whisper.dll is a delay-load dependency of braidcast.exe'
$otherDelay = ($deps.Delay | Where-Object { $VoiceDlls -icontains $_ -and $_ -ine 'whisper.dll' }) -join ', '
if ( $otherDelay ) {
    Write-Host "  (also delay-loaded directly: $otherDelay)"
}

# The same from /imports, which also names the functions.
$imports = Get-Imports -Image $ExePath
foreach ( $name in $VoiceDlls ) {
    $key = $name.ToLowerInvariant()
    Write-Result (-not $imports.Normal.ContainsKey($key)) "no load-time import from $name"
}
$whisperFns = if ( $imports.Delay.ContainsKey('whisper.dll') ) {
    @($imports.Delay['whisper.dll'] | Where-Object { $_ -like 'whisper_*' })
} else {
    @()
}
Write-Result ($whisperFns.Count -gt 0) "whisper_* functions are delay-load imports ($($whisperFns.Count) found)"

# 3: beside the exe.
foreach ( $name in $VoiceDlls ) {
    $path = Join-Path $exeDir $name
    $present = Test-Path -LiteralPath $path -PathType Leaf
    Write-Result $present "$name is beside braidcast.exe"
}

# 4: whisper.dll is the shared build that imports ggml.
$whisperPath = Join-Path $exeDir 'whisper.dll'
if ( Test-Path -LiteralPath $whisperPath -PathType Leaf ) {
    $whisperDeps = Get-Dependents -Image $whisperPath
    $importsGgml = [bool] ($whisperDeps.Normal | Where-Object { $_ -ieq 'ggml.dll' })
    Write-Result $importsGgml 'whisper.dll imports ggml.dll (the shared build)'
}

Write-Host ''
if ( $script:Failures -gt 0 ) {
    Write-Host "Voice DLL check FAILED ($script:Failures failure(s))." -ForegroundColor Red
    exit 1
}
Write-Host 'Voice DLL check PASSED: the speech engine is delay-loaded and staged beside the exe.' -ForegroundColor Green
exit 0
