# Copyright (c) 2026 mozc-skkserv contributors. BSD-3-Clause (see LICENSE).
#
# Starts mozc_skkserv.exe (the SKK dictionary server) hidden in the background.
# Point the SKK client at 127.0.0.1:<Port> (UTF-8 by default).
#
# The server creates no folder and no file, and by default the script writes no
# log. Give -LogDir to get one.
#
# The server stops at logoff or reboot. This script does not start it
# automatically.
param(
  [int]$Port = 1178,
  [ValidateSet('utf-8', 'euc-jp')]
  [string]$Encoding = 'utf-8',  # encoding of requests and responses
  [string]$LogDir = '',         # where the output of the server goes (default: no log)
  [string]$Exe = ''
)
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $repo 'dist\mozc_skkserv.exe' }
if (-not (Test-Path $Exe)) { throw "The server was not found: $Exe (run scripts\build.ps1)" }

if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) {
  throw "Port $Port is already in use."
}

$argList = @("--skkserv_port=$Port", "--skkserv_encoding=$Encoding")
$start = @{
  FilePath         = $Exe
  WorkingDirectory = $repo
  WindowStyle      = 'Hidden'
  PassThru         = $true
}
if ($LogDir) {
  # The server is started in another directory, so a relative path is made
  # absolute here.
  $LogDir = (New-Item -ItemType Directory -Force $LogDir).FullName
  $argList += '--skkserv_verbose'
  $start.RedirectStandardOutput = Join-Path $LogDir "skkserv-$Port.log"
  $start.RedirectStandardError = Join-Path $LogDir "skkserv-$Port.err.log"
}
$process = Start-Process @start -ArgumentList $argList

# Loading Mozc takes a while. Wait until the server listens, or until it exits.
$deadline = (Get-Date).AddSeconds(60)
while ($true) {
  if ($process.HasExited) {
    throw "The server exited (exit code $($process.ExitCode)). Run again with -LogDir to record the cause."
  }
  if (Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue) { break }
  if ((Get-Date) -gt $deadline) {
    $process | Stop-Process -Force
    throw "The server did not start listening on port $Port within 60 seconds."
  }
  Start-Sleep -Milliseconds 300
}
$process | Select-Object Id, ProcessName
