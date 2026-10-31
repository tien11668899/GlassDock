# Builds GlassDock from source and packs it: the app and taskbar_hooks.dll (Rust), taskbar-dock.dll
# and start-glass (C++/WinRT, with the Visual Studio 2022 C++ tools and the Windows SDK) go into
# dist\GlassDock, which is zipped into dist\GlassDock-<version>.zip.
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
$root = $PSScriptRoot

# The C++ builds use the newest Visual Studio with the C++ tools (build.cmd falls back to the
# 2022 Build Tools' default place without it).
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $env:VCVARS = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
}

function Step([string]$name, [scriptblock]$run) {
    Write-Host "== $name"
    & $run
    if ($LASTEXITCODE) { throw "$name failed (exit code $LASTEXITCODE)" }
}

Step 'GlassDock.exe' { cargo build --release --quiet --manifest-path "$root\app\Cargo.toml" }
Step 'taskbar_hooks.dll' { cargo build --release --quiet --manifest-path "$root\taskbar-hooks\Cargo.toml" }
Step 'taskbar-dock.dll' { cmd /c "`"$root\taskbar-dock\build.cmd`"" }
Step 'start-glass' { cmd /c "`"$root\start-glass\build.cmd`"" }

$version = (Select-String -Path "$root\app\Cargo.toml" -Pattern '^version = "(.+)"').Matches[0].Groups[1].Value
$dist = Join-Path $root 'dist'
$folder = Join-Path $dist 'GlassDock'
if (Test-Path $folder) { Remove-Item $folder -Recurse -Force }
New-Item -ItemType Directory -Force $folder | Out-Null
Copy-Item "$root\app\target\release\GlassDock.exe", "$root\taskbar-hooks\target\release\taskbar_hooks.dll",
          "$root\taskbar-dock\out\taskbar-dock.dll", "$root\start-glass\out\start-glass.dll",
          "$root\start-glass\out\start-glass.exe", "$root\README.md", "$root\LICENSE" $folder
$zip = Join-Path $dist "GlassDock-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $folder -DestinationPath $zip
"built $zip"
Get-ChildItem $folder | ForEach-Object { '  {0,-20} {1,6} KB' -f $_.Name, [math]::Round($_.Length / 1KB) }
