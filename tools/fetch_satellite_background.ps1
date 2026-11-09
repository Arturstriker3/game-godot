# Baixa o mosaico de satélite do Esri World Imagery em projeção equirretangular
# e grava map/map_data/satellite.jpg (2:1, lon -180..180, lat -90..90).
#
# A API de export do ArcGIS devolve a imagem já reprojetada em EPSG:4326, então
# o resultado casa pixel a pixel com as coordenadas usadas pelo simulador.
# Acima de +-85.05 (limite do Web Mercator) as faixas polares recebem gelo.
#
# Uso: powershell -ExecutionPolicy Bypass -File tools/fetch_satellite_background.ps1

param(
	[int]$Width = 8192,
	[int]$Height = 4096,
	[string]$OutFile = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($OutFile)) {
	$OutFile = Join-Path $root "map\map_data\satellite.jpg"
}

$endpoint = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/export"
$mercatorLimit = 85.05112878
$columns = 4
$rows = 2
$tempDir = Join-Path $env:TEMP "opengs_satellite"
New-Item -ItemType Directory -Force -Path $tempDir | Out-Null

## Formata um número com ponto decimal: a URL não aceita vírgula de locale.
function Format-Coordinate {
	param([double]$Value)

	return $Value.ToString("0.######", [System.Globalization.CultureInfo]::InvariantCulture)
}

## Retângulo em pixels do recorte equirretangular (lat0 é o norte).
function Get-EquirectRect {
	param([double]$Lon0, [double]$Lat0, [double]$Lon1, [double]$Lat1)

	return @{
		X = [int][math]::Round(($Lon0 + 180.0) / 360.0 * $Width)
		Y = [int][math]::Round((90.0 - $Lat0) / 180.0 * $Height)
		W = [int][math]::Round(($Lon1 - $Lon0) / 360.0 * $Width)
		H = [int][math]::Round(($Lat0 - $Lat1) / 180.0 * $Height)
	}
}

## Baixa um recorte e o desenha no canvas na posição equirretangular correta.
function Add-ImageryChunk {
	param(
		[System.Drawing.Graphics]$Graphics,
		[double]$Lon0,
		[double]$Lat0,
		[double]$Lon1,
		[double]$Lat1,
		[string]$Name
	)

	$rect = Get-EquirectRect -Lon0 $Lon0 -Lat0 $Lat0 -Lon1 $Lon1 -Lat1 $Lat1
	$bbox = "{0},{1},{2},{3}" -f (Format-Coordinate $Lon0), (Format-Coordinate $Lat1), `
		(Format-Coordinate $Lon1), (Format-Coordinate $Lat0)
	$uri = "{0}?bbox={1}&bboxSR=4326&imageSR=4326&size={2},{3}&format=jpg&f=image" -f `
		$endpoint, $bbox, $rect.W, $rect.H
	$path = Join-Path $tempDir "$Name.jpg"

	Write-Host ("  {0}: lon {1}..{2} lat {3}..{4} -> {5}x{6}" -f `
		$Name, $Lon0, $Lon1, $Lat1, $Lat0, $rect.W, $rect.H)

	for ($attempt = 1; $attempt -le 3; $attempt++) {
		try {
			Invoke-WebRequest -Uri $uri -OutFile $path -UseBasicParsing -TimeoutSec 600
			break
		}
		catch {
			if ($attempt -eq 3) { throw }
			Start-Sleep -Seconds (5 * $attempt)
		}
	}

	$magic = [System.IO.File]::ReadAllBytes($path)
	if ($magic.Length -lt 2 -or $magic[0] -ne 0xFF -or $magic[1] -ne 0xD8) {
		throw ("Resposta não é JPEG em {0}: {1}" -f $Name, [System.Text.Encoding]::UTF8.GetString($magic))
	}

	$chunk = [System.Drawing.Image]::FromFile($path)
	try {
		$target = New-Object System.Drawing.Rectangle($rect.X, $rect.Y, $rect.W, $rect.H)
		$Graphics.DrawImage($chunk, $target)
	}
	finally {
		$chunk.Dispose()
	}
}

Write-Host "Gerando mosaico de satélite ${Width}x${Height}..."

$canvas = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$graphics = [System.Drawing.Graphics]::FromImage($canvas)

try {
	$graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
	$graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
	$graphics.Clear([System.Drawing.Color]::FromArgb(232, 238, 240))

	$stepLon = 360.0 / $columns
	for ($column = 0; $column -lt $columns; $column++) {
		$lon0 = -180.0 + $column * $stepLon
		$lon1 = $lon0 + $stepLon
		for ($row = 0; $row -lt $rows; $row++) {
			$lat0 = if ($row -eq 0) { $mercatorLimit } else { 0.0 }
			$lat1 = if ($row -eq $rows - 1) { -$mercatorLimit } else { 0.0 }
			Add-ImageryChunk -Graphics $graphics -Lon0 $lon0 -Lat0 $lat0 `
				-Lon1 $lon1 -Lat1 $lat1 -Name ("c{0}r{1}" -f $column, $row)
		}
	}

	$encoder = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() |
		Where-Object { $_.MimeType -eq "image/jpeg" }
	$encoderParams = New-Object System.Drawing.Imaging.EncoderParameters(1)
	$encoderParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter(
		[System.Drawing.Imaging.Encoder]::Quality, [long]92)

	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
	$canvas.Save($OutFile, $encoder, $encoderParams)
}
finally {
	$graphics.Dispose()
	$canvas.Dispose()
}

Write-Host ("Pronto: {0} ({1:N1} MB)" -f $OutFile, ((Get-Item $OutFile).Length / 1MB))
