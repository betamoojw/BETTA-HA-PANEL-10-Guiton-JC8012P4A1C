<#
.SYNOPSIS
    Buduje firmware dla wariantu panel10jc (Guition JC8012P4A1C 10.1").
    Naprawia też problem z kodowaniem Windows (UnicodeEncodeError na znaku '≥').

.DESCRIPTION
    Ten skrypt robi w jednym miejscu wszystko, czego wymaga pełny build:
      1. Uruchamia export.ps1 z ESP-IDF (w tym samym procesie).
      2. Wymusza UTF-8 dla Pythona (PYTHONUTF8/PYTHONIOENCODING) — bez tego
         idf.py na Windows z polską stroną kodową (cp1250) wywala się na
         znaku '≥' w wyjściu CMake.
      3. Ustawia target esp32p4 (sdkconfig.defaults NIE zawiera CONFIG_IDF_TARGET).
      4. Buduje wariant panel10jc do build-panel10jc/.

.PARAMETER Port
    Opcjonalnie: port COM do wgrania firmware po buildzie (np. COM3).

.PARAMETER Flash
    Przełącznik: po udanym buildzie wgraj firmware przez port -Port.

.PARAMETER Clean
    Przełącznik: usuń build-panel10jc przed buildem (pełny, czysty build).

.EXAMPLE
    pwsh tools/build_panel10jc.ps1

.EXAMPLE
    pwsh tools/build_panel10jc.ps1 -Flash -Port COM3
#>
[CmdletBinding()]
param(
    [string]$Port = "",
    [switch]$Flash,
    [switch]$Clean
)

# NIE używamy "Stop": export.ps1 wypisuje "Activating ESP-IDF" na stderr,
# a przy ErrorActionPreference="Stop" PowerShell 5.1 zamienia to w błąd krytyczny.
# Zamiast tego po każdym kroku sprawdzamy $LASTEXITCODE.
$ErrorActionPreference = "Continue"

function Resolve-RepoRoot {
    if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        return (Split-Path -Parent $PSScriptRoot)
    }
    return (Get-Location).Path
}

function Resolve-IdfExportScript {
    # 1) IDF_PATH już ustawione?
    if ($env:IDF_PATH) {
        $candidate = Join-Path $env:IDF_PATH 'export.ps1'
        if (Test-Path $candidate) { return $candidate }
    }

    # 2) Typowe lokalizacje ESP-IDF na Windows.
    $candidates = @(
        'C:\Espressif\frameworks\esp-idf\export.ps1',
        'C:\Espressif\frameworks\esp-idf-v6.0.2\export.ps1',
        "$env:USERPROFILE\esp\esp-idf\export.ps1",
        "$env:USERPROFILE\esp\v5.5.5\esp-idf\export.ps1"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }

    throw "Nie znaleziono export.ps1 z ESP-IDF. Ustaw zmienną IDF_PATH lub zainstaluj ESP-IDF w jednej z lokalizacji: $($candidates -join ', ')."
}

$RepoRoot = Resolve-RepoRoot
$BuildDir  = Join-Path $RepoRoot 'build-panel10jc'
$Defaults  = 'sdkconfig.defaults;sdkconfig.defaults.panel10jc'

Write-Host "Repo root : $RepoRoot"
Write-Host "Build dir : $BuildDir"

# ESP-IDF export musi działać w TYM procesie (zmienne PATH itp. nie przechodzą między procesami).
$ExportScript = Resolve-IdfExportScript
Write-Host "ESP-IDF    : $ExportScript"

Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force | Out-Null
. $ExportScript *> $null

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "Po uruchomieniu $ExportScript nie znaleziono polecenia 'idf.py'. Sprawdź instalację ESP-IDF."
}

# Wymuszenie UTF-8: bez tego idf.py na cp1250 pada na znaku '≥'.
$env:PYTHONUTF8       = '1'
$env:PYTHONIOENCODING = 'utf-8'

Push-Location $RepoRoot
try {
    if ($Clean -and (Test-Path $BuildDir)) {
        Write-Host "Cleaning build dir..."
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }

    Write-Host "Setting target esp32p4..."
    idf.py -B $BuildDir "-DSDKCONFIG_DEFAULTS=$Defaults" set-target esp32p4
    if ($LASTEXITCODE -ne 0) { throw "set-target failed (exit $LASTEXITCODE)" }

    Write-Host "Building panel10jc..."
    idf.py -B $BuildDir build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }

    $bin = Join-Path $BuildDir 'betta-ha-panel-10jc.bin'
    if (Test-Path $bin) {
        $size = (Get-Item $bin).Length
        Write-Host "OK: $bin ($size bytes)"
    }
    else {
        Write-Warning "Nie znaleziono pliku bin: $bin"
    }

    if ($Flash) {
        if ([string]::IsNullOrWhiteSpace($Port)) {
            throw "Aby wgrać firmware, podaj port: -Flash -Port COM3"
        }
        Write-Host "Flashing to $Port..."
        idf.py -B $BuildDir -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw "flash failed (exit $LASTEXITCODE)" }
    }
}
finally {
    Pop-Location
}
