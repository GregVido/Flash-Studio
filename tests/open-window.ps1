param(
    [Parameter(Mandatory)]
    [string] $Executable,
    [Parameter(Mandatory)]
    [string] $Movie,
    [int] $TimeoutMs = 8000,
    [int] $ObserveMs = 10000,
    [int] $Tag = -1
)

$ErrorActionPreference = 'Stop'
$program = (Resolve-Path -LiteralPath $Executable).ProviderPath
$sample = (Resolve-Path -LiteralPath $Movie).ProviderPath
$filename = Split-Path -Leaf $sample
$launch = [System.Diagnostics.ProcessStartInfo]::new($program)
$launch.CreateNoWindow = $true
$launch.WorkingDirectory = Split-Path -Parent $program
$launch.ArgumentList.Add($sample)
if ($Tag -ge 0) {
    $launch.ArgumentList.Add('--tag')
    $launch.ArgumentList.Add($Tag.ToString())
}
$elapsed = [System.Diagnostics.Stopwatch]::StartNew()
$instance = [System.Diagnostics.Process]::Start($launch)
try {
    do {
        Start-Sleep -Milliseconds 100
        $instance.Refresh()
        if ($instance.HasExited) {
            throw "Flash Studio s’est arrêté avec le code $($instance.ExitCode)."
        }
        if ($instance.MainWindowTitle.EndsWith($filename) -and $instance.Responding) {
            $openingMs = $elapsed.ElapsedMilliseconds
            if ($openingMs -gt $TimeoutMs) {
                break
            }
            $stability = [System.Diagnostics.Stopwatch]::StartNew()
            do {
                Start-Sleep -Milliseconds 250
                $instance.Refresh()
                if ($instance.HasExited) {
                    throw "Flash Studio s’est arrêté avec le code $($instance.ExitCode) après $($stability.ElapsedMilliseconds) ms."
                }
                if (!$instance.Responding) {
                    throw "La fenêtre s’est bloquée après $($stability.ElapsedMilliseconds) ms."
                }
            } while ($stability.ElapsedMilliseconds -lt $ObserveMs)
            $result = "{0} — ouverte en {1} ms, réactive pendant {2} ms" -f
                $instance.MainWindowTitle, $openingMs, $stability.ElapsedMilliseconds
            if (!$instance.CloseMainWindow() -or !$instance.WaitForExit(3000)) {
                throw "Flash Studio ne s’est pas fermé normalement."
            }
            if ($instance.ExitCode -ne 0) {
                throw "Flash Studio s’est fermé avec le code $($instance.ExitCode)."
            }
            Write-Output "$result, fermeture normale"
            return
        }
    } while ($elapsed.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Le fichier n’a pas produit de fenêtre réactive en $TimeoutMs ms."
} finally {
    if (!$instance.HasExited) {
        if ($instance.CloseMainWindow()) {
            [void] $instance.WaitForExit(1000)
        }
        if (!$instance.HasExited) {
            $instance.Kill()
        }
    }
    $instance.Dispose()
}
