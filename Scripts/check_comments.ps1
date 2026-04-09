# check_comments.ps1
# Scans source files under the project root for non-English (CJK) characters.
# Reports file path, line number, and the offending line.
#
# Usage:
#   .\Scripts\check_comments.ps1
#   .\Scripts\check_comments.ps1 -Root "E:/Foundation" -Extensions "cpp","hpp","slang","h"

param(
    [string]$Root       = (Split-Path $PSScriptRoot -Parent),
    [string[]]$Extensions = @("cpp", "hpp", "h", "slang", "py"),
    [string[]]$ExcludeDirs = @(".git", "ThirdParty", "cmake-build-debug", "cmake-build-release", "_deps")
)

# CJK Unified Ideographs and common CJK punctuation ranges
$CJKPattern = '[\u4E00-\u9FFF\u3400-\u4DBF\uF900-\uFAFF\u3000-\u303F\uFF00-\uFFEF]'

$found   = 0
$checked = 0

# Build file list
$allFiles = Get-ChildItem -Path $Root -Recurse -File | Where-Object {
    # Skip excluded directories
    $skip = $false
    foreach ($dir in $ExcludeDirs) {
        if ($_.FullName -match [regex]::Escape($dir)) {
            $skip = $true
            break
        }
    }
    # Only include target extensions
    (-not $skip) -and ($Extensions -contains $_.Extension.TrimStart('.'))
}

foreach ($file in $allFiles) {
    $checked++
    $lines = Get-Content -Path $file.FullName -Encoding UTF8
    $lineNum = 0
    foreach ($line in $lines) {
        $lineNum++
        if ($line -match $CJKPattern) {
            if ($found -eq 0) {
                Write-Host "`nFiles with non-English characters in comments:`n" -ForegroundColor Yellow
            }
            $relPath = $file.FullName.Replace($Root, '').TrimStart('\').TrimStart('/')
            Write-Host ("{0}:{1}" -f $relPath, $lineNum) -ForegroundColor Cyan -NoNewline
            Write-Host ("  $line".TrimEnd())
            $found++
        }
    }
}

Write-Host ""
Write-Host "Checked : $checked file(s)" -ForegroundColor Gray
if ($found -eq 0) {
    Write-Host "Result  : No non-English characters found. All comments are in English." -ForegroundColor Green
} else {
    Write-Host "Result  : $found line(s) with non-English characters found." -ForegroundColor Red
}
