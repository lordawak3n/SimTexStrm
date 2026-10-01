param(
    [ValidateSet('Uncapped', 'Capped60')]
    [string]$Mode = 'Uncapped',
    [int]$ResX = 3840,
    [int]$ResY = 2160,
    [switch]$Trace,
    [switch]$Sequence,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$PassThrough
)

$ErrorActionPreference = 'Stop'

$projectDir = $PSScriptRoot
$uproject = if ($env:LAUNCH_PROJECT) { $env:LAUNCH_PROJECT } else { Join-Path $projectDir 'SimTextureStrm.uproject' }
$editor = if ($env:LAUNCH_EDITOR) { $env:LAUNCH_EDITOR } else { 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe' }

if (-not (Test-Path -LiteralPath $editor)) { Write-Error "Editor executable not found: $editor"; exit 1 }
if (-not (Test-Path -LiteralPath $uproject)) { Write-Error "Project file not found: $uproject"; exit 1 }

$maxFps = if ($Mode -eq 'Capped60') { 60 } else { 0 }

# Benchmark render profile: Lumen off, ray tracing off (startup-only, hence the ini overrides), VSM off.
$renderSection = '[/Script/Engine.RendererSettings]'
$profile = @(
    'r.RayTracing=False',
    'r.DynamicGlobalIlluminationMethod=0',
    'r.ReflectionMethod=2',
    'r.Shadow.Virtual.Enable=0'
)

$arguments = New-Object System.Collections.Generic.List[string]
$arguments.Add("`"$uproject`"")
$arguments.Add('/Game/StartupMap')
$arguments.Add('-game')
$arguments.Add('-windowed')
$arguments.Add('-ForceRes')
$arguments.Add("-ResX=$ResX")
$arguments.Add("-ResY=$ResY")
$arguments.Add('-NoSplash')
$arguments.Add("-ExecCmds=`"r.VSync 0,t.MaxFPS $maxFps`"")
foreach ($setting in $profile) {
    $arguments.Add("-ini:Engine:$renderSection`:$setting")
}

if ($Trace) {
    $traceDir = Join-Path $projectDir 'Saved\SimTexStrm'
    New-Item -ItemType Directory -Force -Path $traceDir | Out-Null
    $traceFile = Join-Path $traceDir ("Bench_{0}.utrace" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
    $arguments.Add('-trace=cpu,gpu,frame,bookmark,memory')
    $arguments.Add('-statnamedevents')
    $arguments.Add("-tracefile=`"$traceFile`"")
}

if ($Sequence) { $arguments.Add('-STSSequence') }

foreach ($extra in $PassThrough) {
    if ($extra -match '\s') { $arguments.Add("`"$extra`"") } else { $arguments.Add($extra) }
}

$argumentString = $arguments -join ' '

$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $editor
$startInfo.Arguments = $argumentString
$startInfo.WorkingDirectory = $projectDir
$startInfo.UseShellExecute = $false

Write-Host "Launching benchmark ($Mode, ${ResX}x${ResY}$(if ($Trace) { ', trace' })$(if ($Sequence) { ', sequence' }))"
Write-Host "  $editor $argumentString"

$process = [System.Diagnostics.Process]::Start($startInfo)
if (-not $process) { Write-Error 'Failed to start the process.'; exit 1 }

if ($Sequence) {
    $process.WaitForExit()
    exit $process.ExitCode
}
exit 0
