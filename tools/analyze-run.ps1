# Summarizes a DualAudioCli --csv file.   analyze-run.ps1 -Csv run.csv [-SkipSeconds 10]
param([Parameter(Mandatory)][string]$Csv, [double]$SkipSeconds = 10)
$rows = Import-Csv $Csv | Where-Object { [double]$_.t -gt $SkipSeconds }
if (-not $rows) { throw 'no rows' }
$n = $rows.Count
$synced = ($rows | Where-Object { $_.status -eq 'Synchronized' }).Count
$diffs = $rows | ForEach-Object { [math]::Abs([double]$_.diff_ms) }
$sorted = $diffs | Sort-Object
function Pct($p) { $sorted[[math]::Min($n - 1, [int][math]::Floor($p * ($n - 1)))] }
$last = $rows[-1]
$aCorr = $rows | ForEach-Object { [double]$_.a_corr_ppm } | Measure-Object -Minimum -Maximum
$bCorr = $rows | ForEach-Object { [double]$_.b_corr_ppm } | Measure-Object -Minimum -Maximum
$aErr = $rows | ForEach-Object { [math]::Abs([double]$_.a_err) } | Measure-Object -Maximum -Average
$bErr = $rows | ForEach-Object { [math]::Abs([double]$_.b_err) } | Measure-Object -Maximum -Average
[pscustomobject]@{
    'Duration (s)'                 = [double]$last.t
    'Samples'                      = $n
    'Synchronized %'               = [math]::Round(100.0 * $synced / $n, 2)
    'Inter-output |diff| mean (ms)' = [math]::Round(($diffs | Measure-Object -Average).Average, 3)
    'Inter-output |diff| p50 (ms)' = [math]::Round((Pct 0.50), 3)
    'Inter-output |diff| p99 (ms)' = [math]::Round((Pct 0.99), 3)
    'Inter-output |diff| max (ms)' = [math]::Round($sorted[-1], 3)
    'A |err| mean/max (ms)'        = '{0:N3} / {1:N2}' -f $aErr.Average, $aErr.Maximum
    'B |err| mean/max (ms)'        = '{0:N3} / {1:N2}' -f $bErr.Average, $bErr.Maximum
    'A correction range (ppm)'     = '{0:N0} .. {1:N0}' -f $aCorr.Minimum, $aCorr.Maximum
    'B correction range (ppm)'     = '{0:N0} .. {1:N0}' -f $bCorr.Minimum, $bCorr.Maximum
    'A / B final drift (ppm)'      = '{0} / {1}' -f $last.a_drift_ppm, $last.b_drift_ppm
    'A / B underrun events'        = '{0} / {1}' -f $last.a_underruns, $last.b_underruns
    'Capture gaps filled'          = $last.capture_gaps
} | Format-List
