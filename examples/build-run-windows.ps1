param(
    [ValidateSet("h264", "h265", "mjpeg")]
    [string]$Codec = "h264",
    [int]$Port = 5600,
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot

$VcpkgRoot = if ($env:VCPKG_ROOT) {
    $env:VCPKG_ROOT
} else {
    Join-Path $env:LOCALAPPDATA "openhd-glide\vcpkg"
}
if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot "vcpkg.exe"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $VcpkgRoot) | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot ".git"))) {
        git clone --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
        if ($LASTEXITCODE -ne 0) { throw "Could not download vcpkg." }
    }
    & (Join-Path $VcpkgRoot "bootstrap-vcpkg.bat") -disableMetrics
    if ($LASTEXITCODE -ne 0) { throw "Could not bootstrap vcpkg." }
}

$GStreamerCandidates = @(
    $env:GSTREAMER_1_0_ROOT_MSVC_X86_64,
    (Join-Path $env:LOCALAPPDATA "Programs\gstreamer\1.0\msvc_x86_64"),
    "C:\gstreamer\1.0\msvc_x86_64",
    "C:\Program Files\gstreamer\1.0\msvc_x86_64"
)
$GStreamerRoot = $GStreamerCandidates |
    Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ "bin\pkg-config.exe")) } |
    Select-Object -First 1
if (-not $GStreamerRoot -and (Get-Command winget -ErrorAction SilentlyContinue)) {
    winget install --id gstreamerproject.gstreamer --exact --silent --scope user --accept-package-agreements --accept-source-agreements --disable-interactivity
    $GStreamerRoot = $GStreamerCandidates |
        Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ "bin\pkg-config.exe")) } |
        Select-Object -First 1
}
if (-not $GStreamerRoot) {
    throw "Install the 64-bit GStreamer MSVC package, or set GSTREAMER_1_0_ROOT_MSVC_X86_64."
}

$PkgConfig = Join-Path $GStreamerRoot "bin\pkg-config.exe"
$env:PATH = "$(Join-Path $GStreamerRoot 'bin');$env:PATH"
$env:PKG_CONFIG = $PkgConfig
$env:PKG_CONFIG_PATH = Join-Path $GStreamerRoot "lib\pkgconfig"
$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

$CMake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $CMake) {
    $CMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path -LiteralPath $CMake)) { throw "CMake was not found." }

& $CMake --preset windows-desktop "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
& $CMake --build --preset windows-desktop
if ($LASTEXITCODE -ne 0) { throw "Windows build failed." }

# Make Release directly runnable from Explorer/Visual Studio as well. The
# controller and its workers are separate executables, so relying on the
# launching terminal's PATH produces one missing-DLL dialog per worker.
$RuntimeDir = Join-Path $ProjectRoot "build-windows\Release"
$RuntimeSources = @(
    (Join-Path $ProjectRoot "build-windows\vcpkg_installed\x64-windows\bin"),
    (Join-Path $GStreamerRoot "bin")
)
foreach ($RuntimeSource in $RuntimeSources) {
    if (-not (Test-Path -LiteralPath $RuntimeSource)) { continue }
    Get-ChildItem -LiteralPath $RuntimeSource -Filter "*.dll" | ForEach-Object {
        $Destination = Join-Path $RuntimeDir $_.Name
        if (-not (Test-Path -LiteralPath $Destination)) {
            Copy-Item -LiteralPath $_.FullName -Destination $Destination
        }
    }
}
$PluginRuntimeDir = Join-Path $RuntimeDir "gstreamer-1.0"
New-Item -ItemType Directory -Force -Path $PluginRuntimeDir | Out-Null
$RequiredPlugins = @(
    "gstapp.dll",
    "gstcoreelements.dll",
    "gstlibav.dll",
    "gstrtp.dll",
    "gstrtpmanager.dll",
    "gsttypefindfunctions.dll",
    "gstudp.dll",
    "gstvideoconvertscale.dll",
    "gstvideoparsersbad.dll"
)
foreach ($Plugin in $RequiredPlugins) {
    $Source = Join-Path $GStreamerRoot "lib\gstreamer-1.0\$Plugin"
    if (-not (Test-Path -LiteralPath $Source)) { throw "Missing required GStreamer plugin: $Plugin" }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $PluginRuntimeDir $Plugin) -Force
}
$PluginScanner = Join-Path $GStreamerRoot "libexec\gstreamer-1.0\gst-plugin-scanner.exe"
if (Test-Path -LiteralPath $PluginScanner) {
    Copy-Item -LiteralPath $PluginScanner -Destination (Join-Path $RuntimeDir "gst-plugin-scanner.exe") -Force
}

if (-not $BuildOnly) {
    $Controller = Join-Path $ProjectRoot "build-windows\Release\openhd-glide.exe"
    $RunArguments = @("--preview-stack", "--view-udp-codec", $Codec, "--view-udp-port", $Port)
    & $Controller @RunArguments
}
