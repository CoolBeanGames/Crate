# Prints the RGB of specific pixels in a PNG, for reading back shader
# diagnostic output encoded as color. Not part of the normal build.
param(
    [string]$Path,
    [Parameter(Mandatory=$true)][int[]]$X,
    [Parameter(Mandatory=$true)][int[]]$Y
)
Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($Path)
for ($i = 0; $i -lt $X.Count; $i++) {
    $px = $bmp.GetPixel($X[$i], $Y[$i])
    Write-Output ("({0},{1}) R={2} G={3} B={4} A={5}" -f $X[$i], $Y[$i], $px.R, $px.G, $px.B, $px.A)
}
$bmp.Dispose()
