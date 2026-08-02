<#
.SYNOPSIS
    OrganicLife Easy Build Script for Windows (WSL)

.DESCRIPTION
    Builds OrganicLife using vcpkg for dependency management.
    Note: Native Windows build is not supported. This script uses WSL.

.PARAMETER NoGui
    Build without Qt GUI (daemon only)

.PARAMETER NoWallet
    Build without wallet support

.PARAMETER WithZmq
    Enable ZeroMQ notifications

.PARAMETER WithUpnp
    Enable UPnP port mapping

.PARAMETER WithNatpmp
    Enable NAT-PMP port mapping

.PARAMETER Debug
    Build with debug symbols

.PARAMETER Release
    Build optimized release (default)

.PARAMETER Clean
    Clean build directory before building

.PARAMETER Jobs
    Number of parallel build jobs (default: auto)

.EXAMPLE
    .\build.ps1
    Build with default settings (release, with GUI)

.EXAMPLE
    .\build.ps1 -NoGui -Debug
    Build daemon only with debug symbols

.EXAMPLE
    .\build.ps1 -WithZmq -WithUpnp
    Build with ZMQ and UPnP support
#>

param(
    [switch]$NoGui,
    [switch]$NoWallet,
    [switch]$WithZmq,
    [switch]$WithUpnp,
    [switch]$WithNatpmp,
    [switch]$Debug,
    [switch]$Release,
    [switch]$Clean,
    [int]$Jobs = 0,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

# Colors
function Write-Info { Write-Host "[INFO] $args" -ForegroundColor Blue }
function Write-Success { Write-Host "[OK] $args" -ForegroundColor Green }
function Write-Warning { Write-Host "[WARN] $args" -ForegroundColor Yellow }
function Write-Error { Write-Host "[ERROR] $args" -ForegroundColor Red }

# Show help
if ($Help) {
    Get-Help $MyInvocation.MyCommand.Path -Detailed
    exit 0
}

Write-Host ""
Write-Info "=== OrganicLife Build Script for Windows ==="
Write-Host ""

# Check for WSL
Write-Info "Checking for WSL..."
$wslInstalled = $false
try {
    $wslOutput = wsl --list --quiet 2>$null
    if ($LASTEXITCODE -eq 0 -and $wslOutput) {
        $wslInstalled = $true
    }
} catch {
    $wslInstalled = $false
}

if (-not $wslInstalled) {
    Write-Error "WSL (Windows Subsystem for Linux) is required but not installed."
    Write-Host ""
    Write-Host "To install WSL, run the following in an Administrator PowerShell:"
    Write-Host "  wsl --install"
    Write-Host ""
    Write-Host "After installation, restart your computer and run this script again."
    exit 1
}

Write-Success "WSL is available"

# Get the Windows path and convert to WSL path
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$wslPath = wsl wslpath -u "'$scriptDir'"

Write-Info "Project directory (WSL): $wslPath"

# Build the arguments for build.sh
$buildArgs = @()

if ($NoGui) { $buildArgs += "--no-gui" }
if ($NoWallet) { $buildArgs += "--no-wallet" }
if ($WithZmq) { $buildArgs += "--with-zmq" }
if ($WithUpnp) { $buildArgs += "--with-upnp" }
if ($WithNatpmp) { $buildArgs += "--with-natpmp" }
if ($Debug) { $buildArgs += "--debug" }
if ($Release) { $buildArgs += "--release" }
if ($Clean) { $buildArgs += "--clean" }
if ($Jobs -gt 0) { $buildArgs += "--jobs"; $buildArgs += $Jobs.ToString() }

$argsString = $buildArgs -join " "

Write-Info "Running build in WSL..."
Write-Host ""

# Install required packages in WSL if needed
$installCmd = @"
if ! command -v cmake &> /dev/null; then
    echo "[INFO] Installing build dependencies in WSL..."
    sudo apt-get update
    sudo apt-get install -y build-essential git cmake pkg-config curl autoconf automake libtool
fi
if ! command -v cargo &> /dev/null; then
    echo "[INFO] Installing Rust in WSL..."
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
    source ~/.cargo/env
fi
"@

# Run the build
$buildCmd = "cd '$wslPath' && $installCmd && chmod +x build.sh && ./build.sh $argsString"

wsl bash -c $buildCmd

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Success "Build completed successfully!"
    Write-Host ""
    Write-Host "Binaries are located in: $scriptDir\build"
    Write-Host ""
    Write-Host "To run OrganicLife from Windows, use WSL:"
    Write-Host "  wsl ./build/organiclifed"
    Write-Host "  wsl ./build/organiclife-qt  (requires WSLg or X server)"
} else {
    Write-Error "Build failed. Check the output above for errors."
    exit 1
}
