<#
Descarga por USB las trazas de las preparaciones guardadas en el display (47_trazas.ino).

Uso (PowerShell, con el display conectado por USB y el monitor serie del IDE CERRADO):
    powershell -ExecutionPolicy Bypass -File tools\descargar_trazas.ps1 -Port COM14
    powershell -ExecutionPolicy Bypass -File tools\descargar_trazas.ps1 -Port COM14 -Carpeta trazas -Solo 3

Crea un archivo CSV por traza (traza_0003.csv...) con todas las lecturas de la Dot a ~10 Hz:
    t_ms, bruto_g, neto_g, caudal_gps, estable
y al final los eventos (líneas "#E,t_ms,evento,arg,valor"): pasos, taras, vertidos, toques, yield...
Sin dependencias: usa System.IO.Ports de Windows.
#>
param(
  [Parameter(Mandatory = $true)][string]$Port,
  [string]$Carpeta = "trazas",
  [int]$Solo = 0
)

$ErrorActionPreference = "Stop"
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.DtrEnable = $true
$sp.ReadTimeout = 500
$sp.NewLine = "`n"
$sp.Open()

# Lee líneas hasta una que empiece por $fin (o hasta $segundos sin nada nuevo)
function Read-Until([string]$fin, [int]$segundos = 5) {
  $lineas = New-Object System.Collections.Generic.List[string]
  $limite = [DateTime]::Now.AddSeconds($segundos)
  while ([DateTime]::Now -lt $limite) {
    try {
      $l = $sp.ReadLine().TrimEnd("`r")
      $limite = [DateTime]::Now.AddSeconds($segundos)
      $lineas.Add($l)
      if ($l.StartsWith($fin)) { return $lineas }
    } catch [System.TimeoutException] { }
  }
  throw "Sin respuesta del display (esperaba '$fin'). ¿Puerto correcto y monitor serie cerrado?"
}

try {
  Start-Sleep -Milliseconds 300
  [void]$sp.ReadExisting()
  $sp.Write("l")
  $lista = Read-Until "[TRZ] FIN"
  $trazas = @()
  foreach ($l in $lista) {
    if ($l -match '^\[TRZ\] (\d+) ') { $trazas += [int]$Matches[1]; Write-Host "  $l" }
  }
  if ($Solo -gt 0) { $trazas = @($Solo) }
  if ($trazas.Count -eq 0) { Write-Host "No hay trazas guardadas."; return }

  New-Item -ItemType Directory -Force $Carpeta | Out-Null
  foreach ($n in $trazas) {
    [void]$sp.ReadExisting()
    $sp.Write("f$n`n")
    $out = Read-Until "[CSV] END" 10
    $csv = @()
    $dentro = $false
    foreach ($l in $out) {
      if ($l.StartsWith("[CSV] BEGIN")) { $dentro = $true; continue }
      if ($l.StartsWith("[CSV] END")) { break }
      if ($l.StartsWith("[CSV] ERROR")) { throw $l }
      # Las líneas de estado del firmware ([STATUS]...) pueden colarse entre las de la traza
      if ($dentro -and -not $l.StartsWith("[")) { $csv += $l }
    }
    $archivo = Join-Path $Carpeta ("traza_{0:D4}.csv" -f $n)
    $ruta = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($archivo)
    [IO.File]::WriteAllLines($ruta, $csv)
    Write-Host ("Traza {0}: {1} lineas -> {2}" -f $n, $csv.Count, $archivo)
  }
} finally {
  $sp.Close()
}
