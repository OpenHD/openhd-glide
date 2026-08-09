param(
    [double]$Latitude = 51.2373245,
    [double]$Longitude = 7.1616353,
    [ValidateSet("circle", "figure8", "line")]
    [string]$Pattern = "figure8",
    [double]$Speed = 12.0,
    [double]$Radius = 180.0,
    [double]$Duration = 0.0
)

$scriptPath = Join-Path $PSScriptRoot "gps-mavlink-emulator.py"
$invariant = [System.Globalization.CultureInfo]::InvariantCulture
python $scriptPath `
    --lat $Latitude.ToString($invariant) `
    --lon $Longitude.ToString($invariant) `
    --pattern $Pattern `
    --speed $Speed.ToString($invariant) `
    --radius $Radius.ToString($invariant) `
    --duration $Duration.ToString($invariant)
