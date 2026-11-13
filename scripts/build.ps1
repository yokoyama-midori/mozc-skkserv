# Copyright (c) 2026 mozc-skkserv contributors. BSD-3-Clause (see LICENSE).
#
# Builds dist\mozc_skkserv.exe from the unmodified Mozc submodule.
#
# 1. Copies overlay\mozc_inproc into mozc\src\mozc_inproc (new files only).
# 2. Runs Bazel with the settings that Mozc's own bazel wrapper (bazel.bat)
#    would set. The wrapper is skipped because it does not quote paths.
#    The environment variables and the current directory that this script
#    changes are restored when it ends.
# 3. Copies the executables of -Targets to -OutDir (default: <repo>\dist).
#
# Requirements:
#  * The repository path contains only ASCII characters and no spaces
#    (Bazel's Windows client cannot change into a workspace directory with
#    non-ASCII characters: https://github.com/bazelbuild/bazel/issues/23859).
#  * Windows Developer Mode, or an elevated shell (Mozc needs symlinks).
#  * Visual Studio with the C++ tools, the Windows SDK and ATL.
#  * Bazelisk (on PATH, or give -BazeliskDir).
#  * `git submodule update --init` and, in mozc\src,
#    `python build_tools/update_deps.py --noqt --nowix`.
param(
  [string]$BazeliskDir = '',
  [string]$VcDir = '',
  [string]$OutDir = '',
  # mozc_skkserv is the server. euc_jp_check is the program that
  # tests\test_euc_jp.py runs.
  [ValidateSet('mozc_skkserv', 'euc_jp_check')]
  [string[]]$Targets = @('mozc_skkserv')
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if ($repo -match '[^\x00-\x7F]' -or $repo -match '\s') {
  throw "The repository path must contain only ASCII characters and no spaces: $repo"
}
$src = Join-Path $repo 'mozc\src'
if (-not (Test-Path (Join-Path $src 'MODULE.bazel'))) {
  throw 'The Mozc submodule is empty. Run: git submodule update --init'
}
if (-not (Test-Path (Join-Path $src 'third_party\llvm\bin\clang-cl.exe'))) {
  throw "LLVM is missing. In $src run: python build_tools/update_deps.py --noqt --nowix"
}
if (-not $OutDir) { $OutDir = Join-Path $repo 'dist' }

# The folder in the Mozc tree is made from scratch so that no file that was
# removed from the overlay remains.
$pkg = Join-Path $src 'mozc_inproc'
if (Test-Path -LiteralPath $pkg) {
  Remove-Item -LiteralPath $pkg -Recurse -Force
}
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item -Path (Join-Path $repo 'overlay\mozc_inproc\*') -Destination $pkg -Force

$savedEnv = @{}
foreach ($name in 'Path', 'BAZELISK_SKIP_WRAPPER', 'BAZEL_LLVM', 'BAZEL_SH', 'BAZEL_VC', 'TEMP', 'TMP') {
  $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location
try {
  if ($BazeliskDir) { $env:Path = "$BazeliskDir;$env:Path" }
  if (-not (Get-Command bazelisk -ErrorAction SilentlyContinue)) {
    throw 'bazelisk was not found. Put it on PATH or pass -BazeliskDir.'
  }
  if (-not $VcDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    # The C++ tools for the architecture of this machine (the build is native).
    $tools = if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') {
      'Microsoft.VisualStudio.Component.VC.Tools.ARM64'
    } else {
      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    }
    # Visual Studio 2022 first, then 2026: the same order as Mozc's
    # build_tools\vs_util.py.
    $inst = $null
    foreach ($versions in '[17,18)', '[18,19)') {
      $inst = & $vswhere -latest -products * -requires $tools -version $versions -property installationPath
      if ($inst) { break }
    }
    if (-not $inst) { throw 'Visual Studio with the C++ tools was not found. Pass -VcDir.' }
    $VcDir = Join-Path $inst 'VC'
  }
  $tmp = Join-Path $repo 'tmp'
  New-Item -ItemType Directory -Force $tmp | Out-Null
  $env:BAZELISK_SKIP_WRAPPER = '1'
  $env:BAZEL_LLVM = Join-Path $src 'third_party\llvm'
  $env:BAZEL_SH   = Join-Path $src 'third_party\msys64\usr\bin\bash.exe'
  $env:BAZEL_VC   = $VcDir
  $env:TEMP = $tmp
  $env:TMP  = $tmp

  Set-Location $src
  # --check_visibility=false: the new package uses Mozc targets that are not
  # exported to it.
  # --features=static_link_msvcrt: links the C/C++ runtime into the executable
  # (/MT), so the Visual C++ Redistributable is not needed.
  foreach ($t in $Targets) {
    bazelisk "--output_user_root=$(Join-Path $repo 'out')" build "//mozc_inproc:$t" --config release_build --check_visibility=false --features=static_link_msvcrt
    if ($LASTEXITCODE -ne 0) { throw "bazel build of $t failed (exit code $LASTEXITCODE)" }
  }
} finally {
  Pop-Location
  foreach ($name in $savedEnv.Keys) {
    [Environment]::SetEnvironmentVariable($name, $savedEnv[$name], 'Process')
  }
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($t in $Targets) {
  $from = Join-Path $src "bazel-bin\mozc_inproc\$t.exe"
  $to = Join-Path $OutDir "$t.exe"
  # A running program cannot be overwritten. Skip the copy if nothing changed.
  if ((Test-Path $to) -and ((Get-FileHash $from).Hash -eq (Get-FileHash $to).Hash)) {
    Write-Host "unchanged: $to"
  } else {
    Copy-Item -LiteralPath $from -Destination $to -Force
  }
  Get-Item $to | Select-Object FullName, Length
}

if ($Targets -contains 'mozc_skkserv') {
  # The licenses go with the server (it contains Mozc and its dictionary data).
  $licDir = Join-Path $OutDir 'licenses'
  New-Item -ItemType Directory -Force $licDir | Out-Null
  Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $licDir -Force
  Copy-Item -LiteralPath (Join-Path $repo 'LICENSE.mozc') -Destination $licDir -Force
  Copy-Item -LiteralPath (Join-Path $repo 'licenses\THIRD_PARTY_NOTICES.txt') -Destination $licDir -Force
}
