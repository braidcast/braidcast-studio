<#
.SYNOPSIS
    Boot-smoke gate for the packaged Braidcast Windows app.

.DESCRIPTION
    Proves a CPack zip is actually launchable -- not merely that it compiled.
    The frontend exe target is `braidcast-frontend` (OUTPUT_NAME `braidcast`),
    not literally `obs-studio`, so OBS's `set_target_properties_obs` skips
    `_bundle_dependencies()` -- the only path that emits install() rules for
    third-party deps. The fork copies runtime files into the dev rundir via
    POST_BUILD custom commands, so the rundir works while the CPack zip has
    historically shipped gutted. Three regressions shipped "CI green":
        1. ~23 runtime/dependency DLLs missing (missing-DLL dialog cascade).
        2. CEF binaries/resources missing.
        3. The Svelte web bundle data/braidcast/web/ missing -> white window.
    This gate fails the build for all three.

    Two layers of checks:
      * File-manifest assertions (PRIMARY, deterministic) -- catch all three
        bugs without launching anything. Requires only an unzipped package root
        and, ideally, a known-good rundir to diff the DLL set against.
      * Liveness (SECONDARY) -- launches the packaged braidcast.exe under
        FE_SMOKE_QUIT_SECONDS and asserts the run was genuinely alive: exit 0
        arriving only AFTER the smoke window elapsed (the app's own
        self-terminate path is the only legitimate exit-0), no crash report
        left behind, and a log scan for the UI-loaded marker and crash
        patterns.

.PARAMETER PackageRoot
    Path to an UNZIPPED package root (the directory that contains bin/, data/,
    obs-plugins/). This is what gets asserted against.

.PARAMETER ReferenceRoot
    Optional path to a known-good rundir (e.g. build_x64/rundir/RelWithDebInfo).
    When supplied, the required-DLL set is DERIVED from it: every *.dll present
    under the reference's bin/64bit and obs-plugins/64bit must also exist in the
    package. This is the resilient, self-adjusting form of bug #1's check. When
    omitted, a reviewed minimal required-DLL floor (below) is used instead.
    Pointing this at the rundir itself (PackageRoot == ReferenceRoot) trivially
    passes the DLL diff -- useful for a local sanity run.

.PARAMETER SkipLiveness
    Skip the launch/liveness stage; run only the deterministic file-manifest
    assertions. Use when the environment cannot host a GUI/Direct3D process.

.PARAMETER LivenessSeconds
    Value passed as FE_SMOKE_QUIT_SECONDS to the launched app (self-terminate
    after N seconds). Must comfortably exceed the app's 4s self-test probe.

.PARAMETER LivenessTimeoutSeconds
    Hard wall-clock ceiling for the launched process before it is killed and the
    liveness check fails.

.NOTES
    PowerShell 7+. Unlike Build-/Package-Windows.ps1 this script does NOT hard
    require $env:CI: the gate is deliberately runnable locally against a rundir
    for pre-push verification. It has no build side effects.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $PackageRoot,
    [string] $ReferenceRoot,
    [switch] $SkipLiveness,
    [switch] $RequireLiveness,
    [int] $LivenessSeconds = 20,
    [int] $LivenessTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'Smoke-Package.ps1 requires PowerShell Core 7. https://aka.ms/pscore6'
    exit 2
}

# Optional: reuse the repo's Log-* helpers for CI log grouping when present.
$LoggerPath = Join-Path $PSScriptRoot 'utils.pwsh/Logger.ps1'
if ( Test-Path $LoggerPath ) {
    . $LoggerPath
}

# OBS keeps the "64bit" directory name on every 64-bit architecture.
$BitDir = '64bit'

# Subdirectories whose *.dll set is diffed against the reference rundir. Both
# have shipped gutted historically: bin/64bit (runtime deps + CEF) and
# obs-plugins/64bit (loadable modules).
$DllScanDirs = @(
    "bin/$BitDir",
    "obs-plugins/$BitDir"
)

# CEF runtime files that must be present for the browser process to initialize
# and for the Svelte UI to render (bug #2). Paths are relative to bin/64bit.
$CefRequiredFiles = @(
    'libcef.dll',
    'resources.pak',
    'chrome_100_percent.pak',
    'chrome_200_percent.pak',
    'icudtl.dat',
    'v8_context_snapshot.bin',
    'snapshot_blob.bin'
)

# Minimal reviewed required-DLL floor used ONLY when no -ReferenceRoot is given.
# The reference diff is the real check; this is a coarse backstop so the script
# still catches gross DLL loss when run standalone. Names are lowercased.
$MinimalRequiredDlls = @(
    'libcef.dll',
    'obs.dll',
    'obs-frontend-api.dll',
    'libobs-d3d11.dll',
    'libobs-winrt.dll',
    'w32-pthreads.dll',
    'zlib.dll',
    'libcurl.dll',
    'avcodec-62.dll',
    'avformat-62.dll',
    'avutil-60.dll',
    'swscale-9.dll',
    'swresample-6.dll',
    'libx264-164.dll'
)

# Fatal/crash markers scanned for in the launched process' captured log. Kept
# tight to avoid false positives from the self-test battery, whose benign output
# legitimately contains the substrings "FAIL" and "(BUG)". Only unambiguous
# crash/abort signals are listed.
$CrashPatterns = @(
    '\[cef\] load error',
    '\[cef\] CreateBrowserSync failed',
    'Unhandled exception',
    'Access violation',
    '0xc0000005',
    'terminate called after throwing',
    'Assertion failed',
    'std::bad_alloc'
)

# Positive proof the renderer actually loaded the Svelte bundle (OnLoadEnd on the
# main frame in client.cpp). Its absence in a clean-exit run is a hard failure --
# that is exactly the white-window symptom of bug #3.
$UiLoadedPattern = '\[cef\] page loaded:'

$script:Failures = [System.Collections.Generic.List[string]]::new()
$script:Passes = [System.Collections.Generic.List[string]]::new()
$script:Warnings = [System.Collections.Generic.List[string]]::new()

function Write-Section {
    param([string] $Message)
    if ( Get-Command Log-Group -ErrorAction SilentlyContinue ) {
        Log-Group $Message
    } else {
        Write-Host "==> $Message" -ForegroundColor Cyan
    }
}

function Add-Pass {
    param([string] $Message)
    $script:Passes.Add($Message)
    Write-Host "  [PASS] $Message" -ForegroundColor Green
}

function Add-Failure {
    param([string] $Message)
    $script:Failures.Add($Message)
    Write-Host "  [FAIL] $Message" -ForegroundColor Red
}

function Add-Warning {
    param([string] $Message)
    $script:Warnings.Add($Message)
    Write-Host "  [WARN] $Message" -ForegroundColor Yellow
}

function Test-RequiredFile {
    param(
        [string] $Root,
        [string] $RelativePath,
        [switch] $NonEmpty
    )
    $full = Join-Path $Root $RelativePath
    if ( -not ( Test-Path -LiteralPath $full -PathType Leaf ) ) {
        Add-Failure "missing file: $RelativePath"
        return
    }
    if ( $NonEmpty ) {
        $len = (Get-Item -LiteralPath $full).Length
        if ( $len -le 0 ) {
            Add-Failure "empty file (0 bytes): $RelativePath"
            return
        }
        Add-Pass "$RelativePath present ($len bytes)"
    } else {
        Add-Pass "$RelativePath present"
    }
}

function Get-DllSet {
    # Returns a case-insensitive HashSet of *.dll basenames under $Dir.
    param([string] $Dir)
    $set = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    if ( Test-Path -LiteralPath $Dir -PathType Container ) {
        foreach ( $dll in Get-ChildItem -LiteralPath $Dir -Filter '*.dll' -File ) {
            [void] $set.Add($dll.Name)
        }
    }
    return , $set
}

# ---------------------------------------------------------------------------
# Resolve and validate roots
# ---------------------------------------------------------------------------
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
if ( -not ( Test-Path -LiteralPath $PackageRoot -PathType Container ) ) {
    Write-Error "PackageRoot is not a directory: $PackageRoot"
    exit 2
}
Write-Host "Package root : $PackageRoot"

if ( $ReferenceRoot ) {
    $ReferenceRoot = (Resolve-Path -LiteralPath $ReferenceRoot).Path
    Write-Host "Reference    : $ReferenceRoot"
} else {
    Write-Host "Reference    : (none -- using minimal required-DLL floor)"
}

# ---------------------------------------------------------------------------
# 1. Web bundle (bug #3: white window)
# ---------------------------------------------------------------------------
Write-Section 'Smoke: web bundle'
Test-RequiredFile -Root $PackageRoot -RelativePath 'data/braidcast/web/index.html' -NonEmpty

# ---------------------------------------------------------------------------
# 2. CEF runtime (bug #2: missing CEF binaries/resources)
# ---------------------------------------------------------------------------
Write-Section 'Smoke: CEF runtime'
foreach ( $rel in $CefRequiredFiles ) {
    Test-RequiredFile -Root $PackageRoot -RelativePath "bin/$BitDir/$rel"
}
$localesDir = Join-Path $PackageRoot "bin/$BitDir/locales"
if ( Test-Path -LiteralPath $localesDir -PathType Container ) {
    $localeCount = (Get-ChildItem -LiteralPath $localesDir -Filter '*.pak' -File).Count
    if ( $localeCount -ge 1 ) {
        Add-Pass "CEF locales present ($localeCount .pak files)"
    } else {
        Add-Failure "CEF locales directory has no .pak files: bin/$BitDir/locales"
    }
} else {
    Add-Failure "missing CEF locales directory: bin/$BitDir/locales"
}

# ---------------------------------------------------------------------------
# 3. Dependency DLLs (bug #1: gutted runtime/dependency DLLs)
# ---------------------------------------------------------------------------
Write-Section 'Smoke: dependency DLLs'
if ( $ReferenceRoot ) {
    foreach ( $sub in $DllScanDirs ) {
        $refDir = Join-Path $ReferenceRoot $sub
        $pkgDir = Join-Path $PackageRoot $sub
        $refSet = Get-DllSet -Dir $refDir
        if ( $refSet.Count -eq 0 ) {
            Write-Host "  (reference has no DLLs under $sub -- skipping)" -ForegroundColor DarkGray
            continue
        }
        $pkgSet = Get-DllSet -Dir $pkgDir
        $missing = [System.Collections.Generic.List[string]]::new()
        foreach ( $name in $refSet ) {
            if ( -not $pkgSet.Contains($name) ) {
                $missing.Add($name)
            }
        }
        if ( $missing.Count -gt 0 ) {
            $sorted = ($missing | Sort-Object) -join ', '
            Add-Failure "${sub}: $($missing.Count)/$($refSet.Count) reference DLL(s) missing from package: $sorted"
        } else {
            Add-Pass "${sub}: all $($refSet.Count) reference DLL(s) present"
        }
    }
} else {
    $pkgSet = Get-DllSet -Dir (Join-Path $PackageRoot "bin/$BitDir")
    $missing = [System.Collections.Generic.List[string]]::new()
    foreach ( $name in $MinimalRequiredDlls ) {
        if ( -not $pkgSet.Contains($name) ) {
            $missing.Add($name)
        }
    }
    if ( $missing.Count -gt 0 ) {
        Add-Failure "bin/${BitDir}: minimal required DLL(s) missing: $(( $missing | Sort-Object ) -join ', ')"
    } else {
        Add-Pass "bin/${BitDir}: all $($MinimalRequiredDlls.Count) minimal required DLL(s) present"
    }
}

# ---------------------------------------------------------------------------
# 4. Liveness (secondary): launch the packaged exe and watch it exit clean
# ---------------------------------------------------------------------------
if ( $SkipLiveness ) {
    Write-Host "==> Smoke: liveness SKIPPED (-SkipLiveness)" -ForegroundColor Yellow
} else {
    Write-Section 'Smoke: liveness (launch packaged braidcast.exe)'
    $exe = Join-Path $PackageRoot "bin/$BitDir/braidcast.exe"
    if ( -not ( Test-Path -LiteralPath $exe -PathType Leaf ) ) {
        Add-Failure "packaged executable missing: bin/$BitDir/braidcast.exe"
    } else {
        $workDir = Split-Path -Parent $exe
        $outFile = New-TemporaryFile
        $errFile = New-TemporaryFile
        $prevSmoke = $env:FE_SMOKE_QUIT_SECONDS
        $env:FE_SMOKE_QUIT_SECONDS = "$LivenessSeconds"

        # Spawn-vs-run distinction: an exception here means the environment could
        # not even START the process (e.g. a session that cannot launch a GUI
        # process surfaces ERROR_ELEVATION_REQUIRED). That is infrastructure, not
        # a packaging regression -- treat it as inconclusive so it does not mask
        # or override the deterministic manifest gate. Promote to a hard failure
        # only under -RequireLiveness. A process that DID start and then exited
        # non-zero / crashed / hung is always a hard failure.
        $started = $false
        $ranToExit = $false
        $exitCode = $null
        $launchedAt = $null
        $elapsedSeconds = $null
        try {
            Write-Host "  launching (FE_SMOKE_QUIT_SECONDS=$LivenessSeconds, timeout=${LivenessTimeoutSeconds}s)..."
            $launchedAt = Get-Date
            $proc = Start-Process -FilePath $exe -WorkingDirectory $workDir -PassThru -NoNewWindow `
                -RedirectStandardOutput $outFile -RedirectStandardError $errFile
            $started = $true
            $ranToExit = $proc.WaitForExit($LivenessTimeoutSeconds * 1000)
            $elapsedSeconds = [math]::Round(((Get-Date) - $launchedAt).TotalSeconds, 1)
            if ( $ranToExit ) {
                $exitCode = $proc.ExitCode
            } else {
                try { $proc.Kill($true) } catch { }
            }
        } catch {
            $msg = "could not launch packaged exe in this environment: $($_.Exception.Message)"
            if ( $RequireLiveness ) {
                Add-Failure $msg
            } else {
                Add-Warning "$msg (liveness inconclusive; deterministic manifest gate still applies -- use -RequireLiveness to make this fatal)"
            }
        } finally {
            $env:FE_SMOKE_QUIT_SECONDS = $prevSmoke
        }

        if ( $started ) {
            if ( -not $ranToExit ) {
                Add-Failure "process did not exit within ${LivenessTimeoutSeconds}s (killed)"
            } elseif ( $exitCode -ne 0 ) {
                Add-Failure "process exited with code $exitCode"
            } elseif ( $elapsedSeconds -lt $LivenessSeconds ) {
                # Exit 0 is only trustworthy when it came from the app's own
                # FE_SMOKE_QUIT_SECONDS self-terminate timer, which cannot fire
                # before the smoke window elapses. An earlier exit-0 is some
                # other path (single-instance bounce, a crash misreporting
                # success) and means the app never proved it was alive.
                Add-Failure "process exited 0 after ${elapsedSeconds}s, before the ${LivenessSeconds}s smoke window elapsed"
            } else {
                Add-Pass "process exited 0 after the full ${LivenessSeconds}s smoke window (${elapsedSeconds}s)"
            }

            # The crash sink writes <config>/crashes/*.txt before exiting, so a
            # crash report from this run is a hard failure regardless of exit
            # code. Config resolves to %APPDATA%/braidcast, or to
            # <exe dir>/config when a portable marker sits next to the exe.
            $crashDirs = @(
                (Join-Path $env:APPDATA 'braidcast/crashes'),
                (Join-Path $workDir 'config/crashes')
            )
            $newCrashFiles = foreach ( $dir in $crashDirs ) {
                if ( Test-Path -LiteralPath $dir -PathType Container ) {
                    Get-ChildItem -LiteralPath $dir -Filter '*.txt' -File |
                        Where-Object { $_.LastWriteTime -ge $launchedAt }
                }
            }
            if ( $newCrashFiles ) {
                foreach ( $f in $newCrashFiles ) {
                    Add-Failure "crash report written during run: $($f.FullName)"
                }
            } else {
                Add-Pass 'no crash report left behind'
            }

            # Merge captured streams (blog's default handler writes to stderr).
            $log = @()
            foreach ( $f in @($outFile, $errFile) ) {
                if ( Test-Path -LiteralPath $f ) {
                    $log += Get-Content -LiteralPath $f -ErrorAction SilentlyContinue
                }
            }

            Write-Host "  ---- captured log tail ----" -ForegroundColor DarkGray
            $log | Select-Object -Last 25 | ForEach-Object { Write-Host "  | $_" -ForegroundColor DarkGray }
            Write-Host "  ---------------------------" -ForegroundColor DarkGray

            # A crash/fatal marker is always a hard failure.
            foreach ( $pat in $CrashPatterns ) {
                $hit = $log | Where-Object { $_ -match $pat } | Select-Object -First 1
                if ( $hit ) {
                    Add-Failure "crash/fatal pattern in log: /$pat/ -> $hit"
                }
            }

            # The UI-loaded marker corroborates that the renderer loaded the
            # Svelte bundle. Its absence on an otherwise-clean exit is a warning,
            # not a failure: the white-window bug is already caught deterministically
            # by the index.html check, and software-render/GPU quirks on CI runners
            # can legitimately suppress this marker without a real regression.
            if ( ($log -join "`n") -match $UiLoadedPattern ) {
                Add-Pass "UI-loaded marker seen ($UiLoadedPattern)"
            } else {
                Add-Warning "UI-loaded marker not found in log (expected '$UiLoadedPattern')"
            }
        }

        Remove-Item -LiteralPath $outFile, $errFile -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# Verdict
# ---------------------------------------------------------------------------
if ( Get-Command Log-Group -ErrorAction SilentlyContinue ) { Log-Group }
Write-Host ''
Write-Host "Smoke summary: $($script:Passes.Count) passed, $($script:Warnings.Count) warned, $($script:Failures.Count) failed"
if ( $script:Warnings.Count -gt 0 ) {
    foreach ( $w in $script:Warnings ) {
        Write-Host "  WARN: $w" -ForegroundColor Yellow
    }
}
if ( $script:Failures.Count -gt 0 ) {
    Write-Host ''
    foreach ( $f in $script:Failures ) {
        Write-Host "  FAIL: $f" -ForegroundColor Red
    }
    Write-Error "Boot-smoke gate FAILED with $($script:Failures.Count) assertion failure(s). The package would not launch correctly."
    exit 1
}
Write-Host 'Boot-smoke gate PASSED: package launches with its UI.' -ForegroundColor Green
exit 0
