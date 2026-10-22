param(
    [ValidateSet('Uncapped', 'Capped60')]
    [string]$Mode = 'Uncapped',
    [int]$ResX = 3840,
    [int]$ResY = 2160,
    [switch]$Trace,
    [switch]$Sequence,
    [ValidateSet('SteadySpread', 'SpreadSweep', 'Burst', 'Oscillation', 'Soak', 'Overhead', 'Lifetime', 'SoakShort')]
    [string]$Scenario,
    # The upload mode the game runs in (-StsMode=): LogicalOnly, Baseline, BaselineSingleUse or Regions. Without it the testbench's UploadMode knob applies.
    # A comma list (-StsMode Baseline,BaselineSingleUse,Regions) runs every mode: -Suite runs each scenario under each mode, -Scenario / -Repeat run the modes one after the other.
    [string[]]$StsMode,
    # Server-response checksums (-STSChecksums=). They cost game-thread time, so -Suite, -Scenario and -Repeat runs default to Off; other runs keep the game's default unless this is given.
    [ValidateSet('On', 'Off')]
    [string]$Checksums,
    # Runs the same configuration N times in a row; each run's recording files are tagged with the run index (_r1, _r2, ...).
    [ValidateRange(1, 1000)]
    [int]$Repeat = 1,
    # Runs SteadySpread, SpreadSweep, Burst and Oscillation, each uncapped and capped at 60 fps, 3 repeats each (or -Repeat N), then writes the aggregate report.
    [switch]$Suite,
    # Adds the 30 minute Soak scenario to -Suite.
    [switch]$IncludeSoak,
    # A run that has not exited after this long is killed (the Soak run gets at least 75).
    [int]$RunTimeoutMinutes = 45,
    # Writes the aggregate report from the tagged *_summary.txt files already in this folder, without launching the game.
    [string]$ReportFrom,
    # Where -Suite and -ReportFrom write the report. Defaults to Saved\SimTexStrm\Suites.
    [string]$SuiteOutDir,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$PassThrough
)

$ErrorActionPreference = 'Stop'

$projectDir = $PSScriptRoot
$uproject = if ($env:LAUNCH_PROJECT) { $env:LAUNCH_PROJECT } else { Join-Path $projectDir 'SimTextureStrm.uproject' }
$editor = if ($env:LAUNCH_EDITOR) { $env:LAUNCH_EDITOR } else { 'C:\Program Files\Epic Games\UE_5.6\Engine\Binaries\Win64\UnrealEditor.exe' }
$recordDir = Join-Path $projectDir 'Saved\SimTexStrm'
if (-not $SuiteOutDir) { $SuiteOutDir = Join-Path $recordDir 'Suites' }

$inv = [System.Globalization.CultureInfo]::InvariantCulture

# -StsMode arrives as one string when the script is started through RunBenchmark.cmd (-File), so split commas here and check against the known modes.
$knownModes = @('LogicalOnly', 'Baseline', 'BaselineSingleUse', 'Regions')
$modeList = @()
foreach ($item in @($StsMode)) {
    foreach ($name in ($item -split '[,;\s]+')) {
        if (-not $name) { continue }
        $known = $knownModes | Where-Object { $_ -ieq $name }
        if (-not $known) { Write-Error "Unknown -StsMode '$name'. Valid modes: $($knownModes -join ', ')."; exit 1 }
        if ($modeList -notcontains $known) { $modeList += $known }
    }
}

$checksumSetting = $Checksums
if (-not $checksumSetting -and ($Suite -or $Scenario -or $Repeat -gt 1)) { $checksumSetting = 'Off' }

# Benchmark render profile: Lumen off, ray tracing off (startup-only, hence the ini overrides), VSM off.
$renderSection = '[/Script/Engine.RendererSettings]'
$profile = @(
    'r.RayTracing=False',
    'r.DynamicGlobalIlluminationMethod=0',
    'r.ReflectionMethod=2',
    'r.Shadow.Virtual.Enable=0'
)

# --- Launching ---

# Launches one game process. Returns its exit code, or 0 straight away when it is not a self-quitting run.
function Start-BenchRun {
    param(
        [string]$RunMode,
        [string]$RunScenario,
        [bool]$RunSequence,
        [string]$Tag,
        [int]$TimeoutMinutes,
        [string]$RunStsMode
    )

    $maxFps = if ($RunMode -eq 'Capped60') { 60 } else { 0 }

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
        New-Item -ItemType Directory -Force -Path $recordDir | Out-Null
        $traceName = if ($Tag) { 'Bench_{0}_{1}.utrace' -f $Tag, (Get-Date -Format 'yyyyMMdd_HHmmss') } else { 'Bench_{0}.utrace' -f (Get-Date -Format 'yyyyMMdd_HHmmss') }
        $traceFile = Join-Path $recordDir $traceName
        $arguments.Add('-trace=cpu,gpu,frame,bookmark,memory')
        $arguments.Add('-statnamedevents')
        $arguments.Add("-tracefile=`"$traceFile`"")
    }

    if ($RunSequence) { $arguments.Add('-STSSequence') }
    if ($RunScenario) { $arguments.Add("-STSScenario=$RunScenario") }
    if ($RunStsMode) { $arguments.Add("-StsMode=$RunStsMode") }
    if ($checksumSetting) { $arguments.Add("-STSChecksums=$(if ($checksumSetting -eq 'On') { 1 } else { 0 })") }
    if ($Tag) { $arguments.Add("-STSRunTag=$Tag") }

    foreach ($extra in $PassThrough) {
        if ($extra -match '\s') { $arguments.Add("`"$extra`"") } else { $arguments.Add($extra) }
    }

    $argumentString = $arguments -join ' '

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $editor
    $startInfo.Arguments = $argumentString
    $startInfo.WorkingDirectory = $projectDir
    $startInfo.UseShellExecute = $false

    Write-Host "Launching benchmark ($RunMode, ${ResX}x${ResY}$(if ($Trace) { ', trace' })$(if ($RunSequence) { ', sequence' })$(if ($RunScenario) { ", scenario $RunScenario" })$(if ($RunStsMode) { ", StsMode $RunStsMode" })$(if ($checksumSetting) { ", checksums $checksumSetting" } else { ', checksums per game default' })$(if ($Tag) { ", tag $Tag" }))"
    Write-Host "  $editor $argumentString"

    $process = [System.Diagnostics.Process]::Start($startInfo)
    if (-not $process) { Write-Error 'Failed to start the process.'; exit 1 }

    if (-not ($RunSequence -or $RunScenario)) {
        return 0
    }

    if (-not $process.WaitForExit($TimeoutMinutes * 60 * 1000)) {
        Write-Warning "Run did not exit within $TimeoutMinutes minutes; killing it."
        $process.Kill()
        $process.WaitForExit()
        return -1
    }
    return $process.ExitCode
}

# --- Reporting ---

function ConvertTo-Number([string]$Text) {
    $value = 0.0
    if ([double]::TryParse($Text, [System.Globalization.NumberStyles]::Float, $inv, [ref]$value)) { return $value }
    return $null
}

function Format-Number($Value, [string]$Format = '0.###') {
    if ($null -eq $Value) { return '' }
    return ([double]$Value).ToString($Format, $inv)
}

# Reads the timing table and the [Key] lines of one *_summary.txt.
function Read-RunSummary([string]$Path) {
    $timing = @{}
    $keys = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\[Key\]\s+(\S+)\s*=\s*(.+?)\s*$') {
            $keys[$Matches[1]] = $Matches[2]
        }
        elseif ($line -match '^(FrameMs|GameThreadMs|RenderThreadMs|RHIThreadMs|GpuMs)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\s*$') {
            $timing[$Matches[1]] = @{
                Samples = ConvertTo-Number $Matches[2]
                Avg     = ConvertTo-Number $Matches[3]
                P50     = ConvertTo-Number $Matches[4]
                P99     = ConvertTo-Number $Matches[5]
                P999    = ConvertTo-Number $Matches[6]
                Max     = ConvertTo-Number $Matches[7]
                Over16  = ConvertTo-Number $Matches[8]
            }
        }
    }
    return @{ Timing = $timing; KeyValues = $keys }
}

function Get-KeyNumber($Summary, [string]$Name) {
    if ($Summary.KeyValues.ContainsKey($Name)) { return ConvertTo-Number $Summary.KeyValues[$Name] }
    return $null
}

function Get-TimingNumber($Summary, [string]$Column, [string]$Stat) {
    if ($Summary.Timing.ContainsKey($Column)) { return $Summary.Timing[$Column][$Stat] }
    return $null
}

# One aggregate row per recording: the key stats of the Phase 2 spec section 8, in priority order.
function New-RunRow($File, [string]$Recording, [string]$Cap, [int]$Run) {
    $s = Read-RunSummary $File.FullName
    $row = [ordered]@{}
    $row['Scenario'] = ($Recording -split '_')[0]
    $row['Recording'] = $Recording
    $row['Cap'] = $Cap
    $row['Run'] = $Run
    $row['UploadMode'] = if ($s.KeyValues.ContainsKey('UploadMode')) { $s.KeyValues['UploadMode'] } else { '' }
    $row['Checksums'] = if ($s.KeyValues.ContainsKey('Checksums')) { $s.KeyValues['Checksums'] } else { '' }
    $row['RunContext'] = if ($s.KeyValues.ContainsKey('RunContext')) { $s.KeyValues['RunContext'] } else { '' }
    $row['RenderProfile'] = if ($s.KeyValues.ContainsKey('RenderProfile')) { $s.KeyValues['RenderProfile'] } else { '' }

    $row['FrameMs_Max'] = Format-Number (Get-TimingNumber $s 'FrameMs' 'Max')
    $row['FrameMs_P999'] = Format-Number (Get-TimingNumber $s 'FrameMs' 'P999')
    $row['FrameMs_P99'] = Format-Number (Get-TimingNumber $s 'FrameMs' 'P99')
    $row['Frames_Over16_67'] = Format-Number (Get-TimingNumber $s 'FrameMs' 'Over16') '0'

    $row['RenderThreadMs_Max'] = Format-Number (Get-TimingNumber $s 'RenderThreadMs' 'Max')
    $row['RenderThreadMs_P999'] = Format-Number (Get-TimingNumber $s 'RenderThreadMs' 'P999')
    $row['RHIThreadMs_Max'] = Format-Number (Get-TimingNumber $s 'RHIThreadMs' 'Max')
    $row['RHIThreadMs_P999'] = Format-Number (Get-TimingNumber $s 'RHIThreadMs' 'P999')
    foreach ($label in '4k', '2k', '1k', '512') {
        $row["RTCreateMs_Avg_$label"] = Format-Number (Get-KeyNumber $s "RTCreateMsAvg_$label")
        $row["RTCreateMs_Max_$label"] = Format-Number (Get-KeyNumber $s "RTCreateMsMax_$label")
    }

    $row['GpuMs_Max'] = Format-Number (Get-TimingNumber $s 'GpuMs' 'Max')
    $row['GpuMs_P999'] = Format-Number (Get-TimingNumber $s 'GpuMs' 'P999')

    $row['RHITextureMB_Peak'] = Format-Number (Get-KeyNumber $s 'RHITextureMBPeak') '0.0'
    $row['RHITextureMB_End'] = Format-Number (Get-KeyNumber $s 'RHITextureMBEnd') '0.0'
    $row['GpuLocalMemMB_Peak'] = Format-Number (Get-KeyNumber $s 'GpuLocalMemMBPeak') '0.0'
    $row['GpuLocalMemMB_End'] = Format-Number (Get-KeyNumber $s 'GpuLocalMemMBEnd') '0.0'
    $row['ProcessPhysMB_Peak'] = Format-Number (Get-KeyNumber $s 'ProcessPhysMBPeak') '0.0'
    $row['ProcessPhysMB_End'] = Format-Number (Get-KeyNumber $s 'ProcessPhysMBEnd') '0.0'

    $row['IssueToVisible_P99Ms'] = Format-Number (Get-KeyNumber $s 'IssueToVisibleP99Ms')
    $row['IssueToVisible_MaxMs'] = Format-Number (Get-KeyNumber $s 'IssueToVisibleMaxMs')
    $row['ApplyGTMs_Max'] = Format-Number (Get-KeyNumber $s 'ApplyGTMsMax')
    $row['UpdateResourceGTMs_Max'] = Format-Number (Get-KeyNumber $s 'UpdateResourceGTMsMax')
    $row['ApplyStepGTMs_Max'] = Format-Number (Get-KeyNumber $s 'ApplyStepGTMsMax')
    $row['UploadGTMs_Max'] = Format-Number (Get-KeyNumber $s 'UploadGTMsMax')
    foreach ($label in '4k', '2k', '1k', '512') {
        $row["RTUploadMs_Avg_$label"] = Format-Number (Get-KeyNumber $s "RTUploadMsAvg_$label") '0.0000'
        $row["RTUploadMs_Max_$label"] = Format-Number (Get-KeyNumber $s "RTUploadMsMax_$label") '0.0000'
    }
    foreach ($label in '4k', '2k', '1k', '512') {
        $row["UploadLatency_P99Ms_$label"] = Format-Number (Get-KeyNumber $s "UploadLatencyP99Ms_$label")
    }
    $row['SlatePresents_Avg'] = Format-Number (Get-KeyNumber $s 'SlatePresentsAvg') '0.00'
    $row['PoolVramMB'] = Format-Number (Get-KeyNumber $s 'PoolVramMB') '0.0'
    $row['RetainedPlatformDataMB'] = Format-Number (Get-KeyNumber $s 'RetainedPlatformDataMB') '0.0'
    $row['BufferPoolMisses'] = Format-Number (Get-KeyNumber $s 'BufferPoolMisses') '0'
    $row['MipFixedCostUs'] = Format-Number (Get-KeyNumber $s 'MipFixedCostUs') '0.00'
    $row['MipThroughputGBps'] = Format-Number (Get-KeyNumber $s 'MipThroughputGBps') '0.00'
    $row['MipFitR2'] = Format-Number (Get-KeyNumber $s 'MipFitR2') '0.000'
    $row['MipCallsPerFrameMax'] = Format-Number (Get-KeyNumber $s 'MipCallsPerFrameMax') '0'
    $row['MipFixedCostPerBurstFrameMs'] = Format-Number (Get-KeyNumber $s 'MipFixedCostPerBurstFrameMs') '0.000'

    $unsafe = Get-KeyNumber $s 'UnsafeReuses'
    $exhausted = Get-KeyNumber $s 'PoolExhausted'
    $checksum = Get-KeyNumber $s 'ChecksumFailures'
    $mismatches = Get-KeyNumber $s 'ConsistencyMismatches'
    $afterInit = Get-KeyNumber $s 'UpdateResourceAfterInit'
    $readback = Get-KeyNumber $s 'ReadbackMipMismatches'
    $checks = Get-KeyNumber $s 'ConsistencyChecks'
    $row['UnsafeReuses'] = Format-Number $unsafe '0'
    $row['PoolExhausted'] = Format-Number $exhausted '0'
    $row['ChecksumFailures'] = Format-Number $checksum '0'
    $row['ConsistencyMismatches'] = Format-Number $mismatches '0'
    $row['UpdateResourceAfterInit'] = Format-Number $afterInit '0'

    $nonZero = ($unsafe -gt 0) -or ($exhausted -gt 0) -or ($checksum -gt 0) -or ($mismatches -gt 0) -or ($afterInit -gt 0) -or ($readback -gt 0)
    $row['MustBeZero'] = if ($nonZero) { 'FAIL' } elseif (-not ($checks -gt 0)) { 'NO_CHECK' } else { 'OK' }

    $row['ConsumerMs_Avg'] = Format-Number (Get-KeyNumber $s 'ConsumerMsAvg') '0.0000'
    $row['ConsumerMs_Max'] = Format-Number (Get-KeyNumber $s 'ConsumerMsMax') '0.0000'
    return [pscustomobject]$row
}

# Run-to-run spread of the frame-time max and p99.9, per recording and cap.
function New-SpreadRows($Rows) {
    $spread = @()
    foreach ($group in ($Rows | Group-Object -Property { '{0}|{1}|{2}' -f $_.Recording, $_.Cap, $_.UploadMode })) {
        $first = $group.Group[0]
        $row = [ordered]@{ Scenario = $first.Scenario; Recording = $first.Recording; Cap = $first.Cap; UploadMode = $first.UploadMode; Runs = $group.Count }
        foreach ($stat in @(@{ Name = 'FrameMs_Max'; Prop = 'FrameMs_Max' }, @{ Name = 'FrameMs_P999'; Prop = 'FrameMs_P999' })) {
            $values = @($group.Group | ForEach-Object { ConvertTo-Number $_.($stat.Prop) } | Where-Object { $null -ne $_ })
            if ($values.Count -eq 0) { continue }
            $measure = $values | Measure-Object -Minimum -Maximum -Average
            $range = $measure.Maximum - $measure.Minimum
            $row["$($stat.Name)_Min"] = Format-Number $measure.Minimum
            $row["$($stat.Name)_Max"] = Format-Number $measure.Maximum
            $row["$($stat.Name)_Mean"] = Format-Number $measure.Average
            $row["$($stat.Name)_Range"] = Format-Number $range
            $row["$($stat.Name)_RangePct"] = if ($measure.Average -gt 0) { Format-Number (100.0 * $range / $measure.Average) '0.0' } else { '' }
        }
        $spread += [pscustomobject]$row
    }
    return $spread
}

function Get-ColumnMean($Rows, [string]$Column) {
    $values = @($Rows | ForEach-Object { ConvertTo-Number $_.$Column } | Where-Object { $null -ne $_ })
    if ($values.Count -eq 0) { return $null }
    return ($values | Measure-Object -Average).Average
}

# One row per recording x cap x mode (means over the runs), with the difference to the Baseline row of the same recording and cap.
function New-ComparisonRows($Rows) {
    $metrics = @('FrameMs_Max', 'FrameMs_P999', 'FrameMs_P99', 'ApplyStepGTMs_Max', 'RenderThreadMs_Max', 'RHIThreadMs_Max', 'GpuMs_P999', 'IssueToVisible_P99Ms', 'RHITextureMB_Peak', 'ProcessPhysMB_Peak')
    $deltaMetrics = @('FrameMs_Max', 'FrameMs_P999', 'ApplyStepGTMs_Max', 'RenderThreadMs_Max')
    $comparison = @()
    foreach ($cell in ($Rows | Group-Object -Property { '{0}|{1}' -f $_.Recording, $_.Cap })) {
        $byMode = @($cell.Group | Group-Object -Property UploadMode)
        $baseline = $byMode | Where-Object { $_.Name -eq 'Baseline' } | Select-Object -First 1
        foreach ($modeGroup in $byMode) {
            $first = $modeGroup.Group[0]
            $contexts = @($modeGroup.Group | ForEach-Object { $_.RunContext } | Select-Object -Unique)
            $row = [ordered]@{
                Recording = $first.Recording; Cap = $first.Cap; UploadMode = $modeGroup.Name; Runs = $modeGroup.Count
                Context = if ($contexts -contains 'PIE') { 'PIE (not comparable)' } else { ($contexts -join '/') }
            }
            foreach ($metric in $metrics) {
                $row[$metric] = Format-Number (Get-ColumnMean $modeGroup.Group $metric)
            }
            foreach ($metric in $deltaMetrics) {
                $delta = $null
                if ($baseline -and $modeGroup.Name -ne 'Baseline') {
                    $mean = Get-ColumnMean $modeGroup.Group $metric
                    $reference = Get-ColumnMean $baseline.Group $metric
                    if ($null -ne $mean -and $null -ne $reference) { $delta = $mean - $reference }
                }
                $row["d_$metric"] = if ($null -ne $delta) { Format-Number $delta '+0.###;-0.###;0' } else { '' }
            }
            $row['UpdateResourceAfterInit'] = Format-Number (($modeGroup.Group | ForEach-Object { ConvertTo-Number $_.UpdateResourceAfterInit } | Where-Object { $null -ne $_ } | Measure-Object -Sum).Sum) '0'
            $row['MustBeZero'] = if (@($modeGroup.Group | Where-Object { $_.MustBeZero -eq 'FAIL' }).Count -gt 0) { 'FAIL' } elseif (@($modeGroup.Group | Where-Object { $_.MustBeZero -eq 'NO_CHECK' }).Count -gt 0) { 'NO_CHECK' } else { 'OK' }
            $comparison += [pscustomobject]$row
        }
    }
    return $comparison
}

function ConvertTo-MarkdownTable($Rows, [string[]]$Columns) {
    $lines = @()
    $lines += '| ' + ($Columns -join ' | ') + ' |'
    $lines += '|' + (($Columns | ForEach-Object { '---' }) -join '|') + '|'
    foreach ($row in $Rows) {
        $lines += '| ' + (($Columns | ForEach-Object { [string]$row.$_ }) -join ' | ') + ' |'
    }
    return $lines
}

# Runs: array of @{ File; Recording; Cap; Run }. Missing: descriptions of runs that produced nothing.
function Write-SuiteReport($Runs, [string[]]$Missing, [string]$Header) {
    $rows = @()
    foreach ($run in ($Runs | Sort-Object { $_.Recording }, { $_.Mode }, { $_.Cap }, { [int]$_.Run })) {
        $rows += New-RunRow $run.File $run.Recording $run.Cap $run.Run
    }
    if ($rows.Count -eq 0) {
        Write-Warning 'No summaries to aggregate.'
        return $false
    }

    $spread = @(New-SpreadRows $rows)
    $comparison = @(New-ComparisonRows $rows)
    $modes = @($rows | ForEach-Object { $_.UploadMode } | Where-Object { $_ } | Select-Object -Unique)
    $mode = if ($modes.Count -gt 0) { $modes -join '+' } else { 'Unknown' }
    $checksumStates = @($rows | ForEach-Object { $_.Checksums } | Where-Object { $_ } | Select-Object -Unique)
    $profileStates = @($rows | ForEach-Object { $_.RenderProfile } | Where-Object { $_ } | Select-Object -Unique)
    $pieRuns = @($rows | Where-Object { $_.RunContext -eq 'PIE' }).Count

    New-Item -ItemType Directory -Force -Path $SuiteOutDir | Out-Null
    $label = 'Suite_{0}_{1}' -f $mode, (Get-Date -Format 'yyyyMMdd_HHmmss')
    $csvPath = Join-Path $SuiteOutDir "$label.csv"
    $spreadPath = Join-Path $SuiteOutDir "${label}_spread.csv"
    $comparisonPath = Join-Path $SuiteOutDir "${label}_comparison.csv"
    $mdPath = Join-Path $SuiteOutDir "$label.md"

    $rows | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
    $spread | Export-Csv -LiteralPath $spreadPath -NoTypeInformation -Encoding UTF8
    $comparison | Export-Csv -LiteralPath $comparisonPath -NoTypeInformation -Encoding UTF8

    $md = @()
    $md += "# Benchmark suite: $mode"
    $md += ''
    $md += $Header
    $md += ''
    $md += "Checksums: $(if ($checksumStates.Count -gt 0) { $checksumStates -join ', ' } else { 'unknown' })$(if ($checksumStates -contains 'On') { ' (On adds harness cost on the game thread: not a performance run)' }). Render profile: $(if ($profileStates.Count -gt 0) { $profileStates -join ', ' } else { 'unknown' })$(if ($profileStates -contains 'NotApplied') { ' (NotApplied: Lumen, ray tracing or VSM were on; compare only within one profile)' })."
    if ($pieRuns -gt 0) {
        $md += ''
        $md += "**$pieRuns run(s) were in PIE (not comparable): the editor adds its own viewports and presents, so their thread times and frame times do not match a standalone game.**"
    }
    $md += ''
    $md += '## Mode comparison (mean over the runs of each recording x cap; d_* is the difference to Baseline)'
    $md += ''
    $md += ConvertTo-MarkdownTable $comparison @(
        'Recording', 'Cap', 'UploadMode', 'Runs', 'Context',
        'FrameMs_Max', 'd_FrameMs_Max', 'FrameMs_P999', 'd_FrameMs_P999', 'FrameMs_P99',
        'ApplyStepGTMs_Max', 'd_ApplyStepGTMs_Max', 'RenderThreadMs_Max', 'd_RenderThreadMs_Max', 'RHIThreadMs_Max',
        'GpuMs_P999', 'IssueToVisible_P99Ms', 'RHITextureMB_Peak', 'ProcessPhysMB_Peak',
        'UpdateResourceAfterInit', 'MustBeZero')
    $md += ''
    $md += '## Per run (one row per recording x mode x cap x run)'
    $md += ''
    $md += 'Frame times are ms. MustBeZero is OK when UnsafeReuses, PoolExhausted, ChecksumFailures, ConsistencyMismatches, UpdateResourceAfterInit and any readback mismatches are all 0 and a consistency check ran. A memory value of -1 means not available. ApplyStepGTMs is the game-thread time of the whole apply step of the active mode; RTUploadMs and RTCreateMs are render-thread times, about one frame late.'
    $md += ''
    $md += ConvertTo-MarkdownTable $rows @(
        'Recording', 'UploadMode', 'Cap', 'Run', 'Checksums', 'RunContext', 'RenderProfile',
        'FrameMs_Max', 'FrameMs_P999', 'FrameMs_P99', 'Frames_Over16_67',
        'RenderThreadMs_Max', 'RenderThreadMs_P999', 'RHIThreadMs_Max', 'RHIThreadMs_P999',
        'RTCreateMs_Avg_4k', 'RTCreateMs_Max_4k', 'RTCreateMs_Avg_2k', 'RTCreateMs_Max_2k', 'RTCreateMs_Avg_1k', 'RTCreateMs_Max_1k', 'RTCreateMs_Avg_512', 'RTCreateMs_Max_512',
        'GpuMs_Max', 'GpuMs_P999',
        'RHITextureMB_Peak', 'RHITextureMB_End', 'GpuLocalMemMB_Peak', 'GpuLocalMemMB_End', 'ProcessPhysMB_Peak', 'ProcessPhysMB_End',
        'IssueToVisible_P99Ms', 'IssueToVisible_MaxMs', 'ApplyGTMs_Max', 'UpdateResourceGTMs_Max', 'ApplyStepGTMs_Max', 'UploadGTMs_Max',
        'RTUploadMs_Avg_4k', 'RTUploadMs_Max_4k', 'RTUploadMs_Avg_2k', 'RTUploadMs_Max_2k', 'RTUploadMs_Avg_1k', 'RTUploadMs_Max_1k', 'RTUploadMs_Avg_512', 'RTUploadMs_Max_512',
        'UploadLatency_P99Ms_4k', 'UploadLatency_P99Ms_2k', 'UploadLatency_P99Ms_1k', 'UploadLatency_P99Ms_512',
        'PoolVramMB', 'RetainedPlatformDataMB', 'BufferPoolMisses', 'SlatePresents_Avg',
        'MipFixedCostUs', 'MipThroughputGBps', 'MipFitR2', 'MipCallsPerFrameMax', 'MipFixedCostPerBurstFrameMs',
        'UnsafeReuses', 'PoolExhausted', 'ChecksumFailures', 'ConsistencyMismatches', 'UpdateResourceAfterInit', 'MustBeZero',
        'ConsumerMs_Avg', 'ConsumerMs_Max')
    $md += ''
    $md += '## Run-to-run spread of frame time (the noise band)'
    $md += ''
    $md += ConvertTo-MarkdownTable $spread @(
        'Recording', 'UploadMode', 'Cap', 'Runs',
        'FrameMs_Max_Min', 'FrameMs_Max_Max', 'FrameMs_Max_Mean', 'FrameMs_Max_Range', 'FrameMs_Max_RangePct',
        'FrameMs_P999_Min', 'FrameMs_P999_Max', 'FrameMs_P999_Mean', 'FrameMs_P999_Range', 'FrameMs_P999_RangePct')
    if ($Missing -and $Missing.Count -gt 0) {
        $md += ''
        $md += '## Runs that produced no summary'
        $md += ''
        foreach ($item in $Missing) { $md += "- $item" }
    }
    Set-Content -LiteralPath $mdPath -Value $md -Encoding UTF8

    Write-Host "Suite report written:"
    Write-Host "  $csvPath"
    Write-Host "  $spreadPath"
    Write-Host "  $comparisonPath"
    Write-Host "  $mdPath"
    return $true
}

# Maps a summary file name back to its recording, mode, cap and run: <Recording>_[<Mode>_]<Cap>_r<N>_<yyyyMMdd>_<HHmmss>_summary.txt (the mode is absent when the run had no -StsMode)
function ConvertFrom-SummaryName([System.IO.FileInfo]$File) {
    if ($File.Name -match '^(?<rec>.+?)(?:_(?<mode>LogicalOnly|Baseline|BaselineSingleUse|Regions))?_(?<cap>Uncapped|Capped60)_r(?<run>\d+)_\d{8}_\d{6}_summary\.txt$') {
        return @{ File = $File; Recording = $Matches['rec']; Mode = $Matches['mode']; Cap = $Matches['cap']; Run = [int]$Matches['run'] }
    }
    return $null
}

# --- Entry points ---

if ($ReportFrom) {
    if (-not (Test-Path -LiteralPath $ReportFrom)) { Write-Error "Folder not found: $ReportFrom"; exit 1 }
    $runs = @()
    foreach ($file in Get-ChildItem -LiteralPath $ReportFrom -Filter '*_summary.txt') {
        $parsed = ConvertFrom-SummaryName $file
        if ($parsed) { $runs += $parsed }
    }
    $ok = Write-SuiteReport $runs @() "Aggregated from $($runs.Count) tagged summaries in ``$ReportFrom``."
    exit $(if ($ok) { 0 } else { 1 })
}

if (-not (Test-Path -LiteralPath $editor)) { Write-Error "Editor executable not found: $editor"; exit 1 }
if (-not (Test-Path -LiteralPath $uproject)) { Write-Error "Project file not found: $uproject"; exit 1 }

if ($Sequence -and $Scenario) { Write-Error 'Use either -Sequence or -Scenario, not both.'; exit 1 }

if ($Suite) {
    if ($Sequence -or $Scenario) { Write-Error '-Suite chooses the scenarios itself; do not combine it with -Sequence or -Scenario.'; exit 1 }

    $runsPerConfig = if ($PSBoundParameters.ContainsKey('Repeat')) { $Repeat } else { 3 }
    $scenarios = @('SteadySpread', 'SpreadSweep', 'Burst', 'Oscillation')
    if ($IncludeSoak) { $scenarios += 'Soak' }
    $caps = @('Uncapped', 'Capped60')

    # Without -StsMode the game's UploadMode knob applies and the tags carry no mode. With several modes, the modes of one run index are launched back to back so drift hits them equally.
    $suiteModes = if ($modeList.Count -gt 0) { $modeList } else { @('') }

    $suiteStart = Get-Date
    $total = $scenarios.Count * $caps.Count * $runsPerConfig * $suiteModes.Count
    Write-Host "Suite: $($scenarios.Count) scenarios x $($caps.Count) caps x $runsPerConfig runs x $($suiteModes.Count) mode(s) = $total launches$(if ($modeList.Count -gt 0) { ", StsMode $($modeList -join ',')" }), checksums $checksumSetting"

    $collected = @()
    $missing = @()
    $index = 0
    foreach ($suiteScenario in $scenarios) {
        foreach ($cap in $caps) {
            for ($run = 1; $run -le $runsPerConfig; ++$run) {
                foreach ($suiteMode in $suiteModes) {
                    ++$index
                    $tag = if ($suiteMode) { "${suiteMode}_${cap}_r$run" } else { "${cap}_r$run" }
                    $timeout = if ($suiteScenario -eq 'Soak') { [Math]::Max($RunTimeoutMinutes, 75) } else { $RunTimeoutMinutes }
                    $description = "$suiteScenario, $(if ($suiteMode) { "$suiteMode, " })$cap, run $run"
                    Write-Host "[$index/$total] $description"

                    $runStart = (Get-Date).AddSeconds(-2)
                    $exitCode = Start-BenchRun -RunMode $cap -RunScenario $suiteScenario -RunSequence $false -Tag $tag -TimeoutMinutes $timeout -RunStsMode $suiteMode
                    if ($exitCode -ne 0) { Write-Warning "Run exited with code $exitCode." }

                    $found = @(Get-ChildItem -LiteralPath $recordDir -Filter "*_${tag}_*_summary.txt" -ErrorAction SilentlyContinue |
                        Where-Object { $_.LastWriteTime -ge $runStart })
                    if ($found.Count -eq 0) {
                        $missing += "$description (exit code $exitCode)"
                        continue
                    }
                    foreach ($file in $found) {
                        $parsed = ConvertFrom-SummaryName $file
                        if ($parsed) { $collected += $parsed }
                    }
                }
            }
        }
    }

    $elapsed = (Get-Date) - $suiteStart
    $header = "Suite of $total launches ($runsPerConfig runs per scenario, cap and mode) finished in $([int]$elapsed.TotalMinutes) minutes; started $($suiteStart.ToString('yyyy-MM-dd HH:mm:ss'))."
    $ok = Write-SuiteReport $collected $missing $header
    exit $(if ($ok -and $missing.Count -eq 0) { 0 } else { 1 })
}

if ($Repeat -gt 1 -and -not ($Sequence -or $Scenario)) {
    Write-Error '-Repeat needs -Sequence or -Scenario, so each run quits by itself.'
    exit 1
}

$singleModes = if ($modeList.Count -gt 0) { $modeList } else { @('') }
if ($singleModes.Count -gt 1 -and -not ($Sequence -or $Scenario)) {
    Write-Error 'Several -StsMode values need -Sequence or -Scenario, so each run quits by itself.'
    exit 1
}

$lastExitCode = 0
for ($run = 1; $run -le $Repeat; ++$run) {
    foreach ($singleMode in $singleModes) {
        $tagParts = @()
        if ($singleModes.Count -gt 1) { $tagParts += $singleMode }
        if ($Repeat -gt 1) { $tagParts += "r$run" }
        $tag = $tagParts -join '_'
        if ($Repeat -gt 1) { Write-Host "Run $run of $Repeat$(if ($singleMode) { ", $singleMode" })" }
        $lastExitCode = Start-BenchRun -RunMode $Mode -RunScenario $Scenario -RunSequence ([bool]$Sequence) -Tag $tag -TimeoutMinutes $RunTimeoutMinutes -RunStsMode $singleMode
        if ($lastExitCode -ne 0) { Write-Warning "Run $run$(if ($singleMode) { " ($singleMode)" }) exited with code $lastExitCode." }
    }
}
exit $lastExitCode
