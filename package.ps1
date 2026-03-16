# DSVP Portable Packaging Script (Windows)
# Creates a clean DSVP-portable/ folder with exe + all DLLs.
#
# Run from git-bash or PowerShell in the DSVP repo root.
#
# Usage:
#   .\package.ps1
#   .\package.ps1 -SkipBuild    # skip compilation, just package

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$version = "0.1.4-beta"
$outDir  = "DSVP-portable"

Write-Host "=== DSVP Packager v$version ===" -ForegroundColor Cyan

# ── Build ──────────────────────────────────────────────────────────

if (-not $SkipBuild) {
    Write-Host "`n[1/5] Building..." -ForegroundColor Yellow
    mingw32-make clean 2>$null
    mingw32-make
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "      Build OK" -ForegroundColor Green
} else {
    Write-Host "`n[1/5] Skipping build" -ForegroundColor DarkGray
}

# ── Verify exe exists ──────────────────────────────────────────────

if (-not (Test-Path "build\dsvp.exe")) {
    Write-Host "ERROR: build\dsvp.exe not found. Run without -SkipBuild." -ForegroundColor Red
    exit 1
}

# ── Create output directory ────────────────────────────────────────

Write-Host "[2/5] Creating $outDir\" -ForegroundColor Yellow
if (Test-Path $outDir) {
    Remove-Item -Recurse -Force $outDir
}
New-Item -ItemType Directory -Path $outDir | Out-Null

# ── Copy exe and build/ DLLs ──────────────────────────────────────

Write-Host "[3/5] Copying exe and build DLLs..." -ForegroundColor Yellow
Copy-Item "build\dsvp.exe" "$outDir\"

# Makefile already copies SDL3.dll, SDL3_ttf.dll, SDL3_shadercross.dll,
# dxcompiler.dll, dxil.dll to build/
$buildDlls = Get-ChildItem "build\*.dll" -ErrorAction SilentlyContinue
foreach ($f in $buildDlls) {
    Copy-Item $f.FullName "$outDir\"
}
Write-Host "      Copied $($buildDlls.Count) DLLs from build/" -ForegroundColor Green

# ── Resolve transitive DLL dependencies ────────────────────────────

Write-Host "[4/5] Resolving DLL dependencies..." -ForegroundColor Yellow

# Find MSYS2 bin directory (where MinGW DLLs live)
$msysBin = "C:\msys64\mingw64\bin"
if (-not (Test-Path $msysBin)) {
    # Try pkg-config to locate it
    $prefix = & pkg-config --variable=prefix sdl3 2>$null
    if ($prefix) {
        $msysBin = Join-Path $prefix "bin"
    }
}

if (-not (Test-Path $msysBin)) {
    Write-Host "WARNING: MSYS2 mingw64 bin not found. Skipping dependency resolution." -ForegroundColor Yellow
} else {
    # Iteratively resolve: check each DLL/exe in outDir, find missing deps in msysBin
    $systemDirs = @("C:\Windows\system32", "C:\Windows\SysWOW64", "C:\Windows")
    $resolved = @{}
    $changed = $true

    while ($changed) {
        $changed = $false
        $files = Get-ChildItem "$outDir\*.dll", "$outDir\*.exe" -ErrorAction SilentlyContinue

        foreach ($f in $files) {
            if ($resolved[$f.Name]) { continue }
            $resolved[$f.Name] = $true

            # Use objdump to find DLL imports
            $deps = & objdump -p $f.FullName 2>$null | Select-String "DLL Name:" |
                ForEach-Object { ($_ -replace '.*DLL Name:\s*', '').Trim() }

            foreach ($dep in $deps) {
                $destPath = Join-Path $outDir $dep
                if (Test-Path $destPath) { continue }

                # Skip system DLLs
                $isSystem = $false
                foreach ($sysDir in $systemDirs) {
                    if (Test-Path (Join-Path $sysDir $dep)) {
                        $isSystem = $true
                        break
                    }
                }
                if ($isSystem) { continue }

                # Try to find in MSYS2
                $srcPath = Join-Path $msysBin $dep
                if (Test-Path $srcPath) {
                    Copy-Item $srcPath "$outDir\"
                    Write-Host "      + $dep" -ForegroundColor DarkGray
                    $changed = $true
                }
            }
        }
    }
}

$totalDlls = (Get-ChildItem "$outDir\*.dll" -ErrorAction SilentlyContinue).Count
Write-Host "      Total DLLs: $totalDlls" -ForegroundColor Green

# ── Summary ────────────────────────────────────────────────────────

Write-Host "`n[5/5] Package complete!" -ForegroundColor Green

$files = Get-ChildItem $outDir
$totalSize = ($files | Measure-Object -Property Length -Sum).Sum / 1MB

Write-Host "`n  Location:  $outDir\" -ForegroundColor White
Write-Host "  Files:     $($files.Count)" -ForegroundColor White
Write-Host "  Size:      $([math]::Round($totalSize, 1)) MB" -ForegroundColor White
Write-Host "`n  Run with:  .\$outDir\dsvp.exe" -ForegroundColor Cyan
Write-Host ""
