# Download and unpack a Zig release into runtime/zig/. Used both during
# Glide development (so `glide build` finds a bundled Zig) and during
# release packaging.
#
#   $env:ZIG_VERSION  override the Zig version (default below)
#   $env:RUNTIME_DIR  override the install dir (default: runtime/zig)

$ErrorActionPreference = "Stop"

$ZigVersion = if ($env:ZIG_VERSION) { $env:ZIG_VERSION } else { "0.14.1" }
$RuntimeDir = if ($env:RUNTIME_DIR) { $env:RUNTIME_DIR } else { "runtime/zig" }

if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64") {
    $Arch = "aarch64"
} else {
    $Arch = "x86_64"
}

$Name = "zig-$Arch-windows-$ZigVersion"
$Url  = "https://ziglang.org/download/$ZigVersion/$Name.zip"

Write-Host ">> Downloading $Url"
$DlDir = New-Item -ItemType Directory -Path ([System.IO.Path]::GetTempPath() + [System.Guid]::NewGuid())
try {
    $ZipPath = Join-Path $DlDir "zig.zip"
    Invoke-WebRequest -Uri $Url -OutFile $ZipPath

    Write-Host ">> Extracting"
    Expand-Archive -Path $ZipPath -DestinationPath $DlDir -Force

    if (Test-Path $RuntimeDir) {
        Remove-Item -Recurse -Force $RuntimeDir
    }
    $ParentDir = Split-Path $RuntimeDir -Parent
    if ($ParentDir -and -not (Test-Path $ParentDir)) {
        New-Item -ItemType Directory -Force -Path $ParentDir | Out-Null
    }
    Move-Item -Path (Join-Path $DlDir $Name) -Destination $RuntimeDir
}
finally {
    Remove-Item -Recurse -Force $DlDir
}

Write-Host ">> Installed at $RuntimeDir"
& (Join-Path $RuntimeDir "zig.exe") version
