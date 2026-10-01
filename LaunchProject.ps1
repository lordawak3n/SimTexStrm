# Launch Unreal Editor so it loads a specific .uproject (not the Recent Projects hub).
#
# Fixes:
# - PowerShell Start-Process -ArgumentList often splits paths on spaces → UE gets no valid project argv.
# - Use System.Diagnostics.ProcessStartInfo with an explicit Arguments string (Win32 rules).
# - Prefer -project="..." — same style Unreal tooling uses (RunUAT / generated shortcuts).

$ErrorActionPreference = 'Stop'

$EditorExe = $env:LAUNCH_EDITOR
$ProjectFile = $env:LAUNCH_PROJECT

# Defaults when run directly (LaunchProject.cmd sets LAUNCH_* via env).
if ([string]::IsNullOrWhiteSpace($EditorExe)) {
    $EditorExe = 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe'
}
if ([string]::IsNullOrWhiteSpace($ProjectFile)) {
    $ProjectFile = Join-Path $PSScriptRoot 'SimTextureStrm.uproject'
}

if (-not (Test-Path -LiteralPath $EditorExe)) {
    Write-Error "UnrealEditor.exe not found: $EditorExe"
    exit 1
}
if (-not (Test-Path -LiteralPath $ProjectFile)) {
    Write-Error ".uproject not found: $ProjectFile"
    exit 1
}

$projFull = (Resolve-Path -LiteralPath $ProjectFile).Path
$projDir = [System.IO.Path]::GetDirectoryName($projFull)

Write-Host "Editor:  $EditorExe"
Write-Host "Project: $projFull"

function Start-UeEditorProcess {
    param([string]$ArgumentString)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $EditorExe
    $psi.Arguments = $ArgumentString
    $psi.WorkingDirectory = $projDir
    # Direct CreateProcess — argv string is preserved (UseShellExecute true can alter behavior).
    $psi.UseShellExecute = $false
    [System.Diagnostics.Process]::Start($psi)
}

$san = $projFull.Replace('"', '')

# Primary: -project="<absolute path>" (quotes protect spaces inside the flag value)
$argPrimary = '-project="' + $san + '"'

try {
    Write-Host "Argv: $argPrimary"
    $p = Start-UeEditorProcess -ArgumentString $argPrimary
    Write-Host "Started Unreal Editor (PID $($p.Id))."
    exit 0
}
catch {
    Write-Host "Primary launch failed: $($_.Exception.Message)"
}

# Fallback: single positional argv — "<absolute path>.uproject"
try {
    $argFallback = '"' + $san + '"'
    Write-Host "Argv: $argFallback"
    $p = Start-UeEditorProcess -ArgumentString $argFallback
    Write-Host "Started Unreal Editor (PID $($p.Id))."
    exit 0
}
catch {
    Write-Host "Fallback launch failed: $($_.Exception.Message)"
    exit 1
}
