param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$cmakeTool = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeTool) { $cmakePath = $cmakeTool.Source }
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++ and CMake.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vsRoot) { throw 'Visual Studio C++ toolchain not found.' }
    $cmakePath = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$buildDir = Join-Path $PSScriptRoot 'build'
& $cmakePath -S $PSScriptRoot -B $buildDir -A Win32
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmakePath --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Compilation failed.' }
$ctestPath = Join-Path (Split-Path $cmakePath) 'ctest.exe'
& $ctestPath --test-dir $buildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
Write-Host "Build and tests completed: $buildDir/$Configuration"
