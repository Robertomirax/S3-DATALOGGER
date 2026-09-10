$ErrorActionPreference = 'Stop'

$espIdfPath = 'C:\esp\v6.1\esp-idf'
$exportScript = Join-Path $espIdfPath 'export.ps1'

if (-not (Test-Path $exportScript)) {
  throw "No se encontro ESP-IDF en $espIdfPath"
}

if ($env:IDF_PATH -ne $espIdfPath -or -not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
  . $exportScript
}

Write-Host "ESP-IDF listo: $env:IDF_PATH"
Write-Host "idf.py: $((Get-Command idf.py).Source)"
