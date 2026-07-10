# Glide installer for Windows.
#
# Usage:
#   # local archive
#   .\tools\install.ps1 -Archive .\dist\glide-windows-x86_64-0.1.1.zip
#
#   # remote (after release is published)
#   irm https://github.com/.../releases/download/.../install.ps1 | iex
#
# Installs to %LOCALAPPDATA%\Programs\Glide and adds the bin dir to the
# user PATH (no admin needed).

param(
    [string]$Version = "",
    [string]$Archive = "",
    [switch]$Bundle,
    [string]$InstallDir = "$env:LOCALAPPDATA\Programs\Glide",
    [string]$DownloadUrlBase = ""
)

$ErrorActionPreference = "Stop"

# Resolve the latest release when no version was given (and not installing a
# local archive) via the GitHub API.
if (-not $Version -and -not $Archive) {
    $Version = (Invoke-RestMethod "https://api.github.com/repos/glide-lang/Glide/releases/latest").tag_name -replace '^v', ''
    Write-Host ">> Latest release: v$Version"
}

if (-not $DownloadUrlBase) {
    $DownloadUrlBase = "https://github.com/glide-lang/Glide/releases/download/v$Version"
}

$Flavor = if ($Bundle) { "glide-bundle" } else { "glide" }
if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { $Arch = "aarch64" } else { $Arch = "x86_64" }
$Name = "$Flavor-windows-$Arch-$Version"

# 1. Acquire the archive.
$Tmp = New-Item -ItemType Directory -Path ([System.IO.Path]::GetTempPath() + [System.Guid]::NewGuid())
try {
    if ($Archive -and (Test-Path $Archive)) {
        Write-Host ">> Using local archive: $Archive"
        Copy-Item $Archive (Join-Path $Tmp "glide.zip")
    } else {
        $Url = "$DownloadUrlBase/$Name.zip"
        Write-Host ">> Downloading $Url"
        Invoke-WebRequest -Uri $Url -OutFile (Join-Path $Tmp "glide.zip")
    }

    Write-Host ">> Extracting"
    Expand-Archive -Path (Join-Path $Tmp "glide.zip") -DestinationPath $Tmp -Force

    if (Test-Path $InstallDir) { Remove-Item -Recurse -Force $InstallDir }
    $Parent = Split-Path $InstallDir -Parent
    if ($Parent -and -not (Test-Path $Parent)) { New-Item -ItemType Directory -Force -Path $Parent | Out-Null }
    Move-Item -Path (Join-Path $Tmp $Name) -Destination $InstallDir
    Write-Host ">> Installed to $InstallDir"
}
finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}

# 2. Add to user PATH if missing.
$UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $UserPath) { $UserPath = "" }
$PathParts = $UserPath -split ';' | Where-Object { $_ -ne "" }
if (-not ($PathParts -contains $InstallDir)) {
    $NewPath = if ($UserPath) { "$UserPath;$InstallDir" } else { $InstallDir }
    [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    Write-Host ">> Added $InstallDir to user PATH (open a new terminal to pick it up)"
} else {
    Write-Host ">> $InstallDir already on PATH"
}

Write-Host ""
Write-Host "Done. Try: glide --help" -ForegroundColor Green
