# SPDX-License-Identifier: GPL-3.0-or-later
# Flash and verify the tag over a TI XDS110 (LaunchPad debugger) using UniFlash's DSLite.
#
#   tools\xflash.ps1 [image]     default binaries\Tag_FW_CC2630_TG-GR6000N.bin
#
# A .bin is loaded at 0x0 (full image incl. CCFG); a .elf carries its own addresses.
# Wiring, config and troubleshooting: docs/FLASHING_XDS110.md
#
# UniFlash is located via $env:UNIFLASH_DIR, else the newest C:\ti\uniflash_*.
#
# After a successful flash the tag must be fully power cycled — the XDS110's reset
# pulse does not start the new image.
param(
    [string]$Image = "binaries\Tag_FW_CC2630_TG-GR6000N.bin"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Image)) { Write-Error "no such image: $Image" }
$Image = (Resolve-Path $Image).Path

$Ccxml = Join-Path $PSScriptRoot "cc2630_xds110.ccxml"
if (-not (Test-Path $Ccxml)) { Write-Error "missing target config: $Ccxml" }

$UniflashDir = $env:UNIFLASH_DIR
if (-not $UniflashDir) {
    $UniflashDir = Get-ChildItem "C:\ti\uniflash_*" -Directory -ErrorAction SilentlyContinue |
        Sort-Object { [version]($_.Name -replace '^uniflash_', '') } |
        Select-Object -Last 1 -ExpandProperty FullName
}
$Dslite = Join-Path $UniflashDir "dslite.bat"
if (-not (Test-Path $Dslite)) { Write-Error "UniFlash not found (set UNIFLASH_DIR to the uniflash_x.y.z directory)" }

# .bin needs an explicit load address; .elf/.out/.hex do not
$Target = if ($Image -like "*.bin") { "$Image,0x0" } else { $Image }

Write-Host "=== TG-GR6000N XDS110 Flash & Verify ==="
Write-Host "Image:    $Image"
Write-Host "UniFlash: $UniflashDir"
Write-Host ""

$Log = & $Dslite --config=$Ccxml -e -f -v $Target 2>&1 | ForEach-Object { "$_" }
$Log | Where-Object { $_ -match "Erasing|Loading Program|Verifying Program|verification|error|fatal|Failed|^Success" } | ForEach-Object { Write-Host $_ }

if (($Log -match "Program verification successful") -and ($Log -match "^Success")) {
    Write-Host ""
    Write-Host "=== PASS === flashed and verified $Image"
    Write-Host "Now power cycle the tag to boot the new image."
} else {
    $LogFile = Join-Path $env:TEMP "xflash_dslite.log"
    $Log | Set-Content $LogFile
    Write-Host ""
    Write-Host "=== FAIL === see docs/FLASHING_XDS110.md, Troubleshooting. Full log: $LogFile"
    exit 1
}
