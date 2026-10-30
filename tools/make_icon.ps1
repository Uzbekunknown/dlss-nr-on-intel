param([string]$Out = '')
# NR-Setup's icon, drawn here rather than kept in the repository, which takes no binary file
# (publish_check): NR in white Segoe UI Semibold on a pentagon in Intel's classic blue, #0068B5.
# Drawn once at 256 px and scaled down, so every size is the picture the owner chose (2026-10-31).
# Sizes 16 to 128 are 32-bit bitmaps with alpha and 256 is a PNG, as Windows' own icons are.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not $Out) { $Out = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\work\nr-setup.ico')) }

function Draw-Icon {
    $bmp = [Drawing.Bitmap]::new(256, 256, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([Drawing.Color]::Transparent)
    # A regular pentagon, point up, its centre lowered so it sits in the square; filled, and
    # stroked in its own colour with round joins, which rounds its corners.
    $round = 14
    $r = (256 - 2 * $round) * 0.53
    $cy = 128 + 0.0955 * $r
    $points = [Drawing.PointF[]](0..4 | ForEach-Object {
        $a = (-90 + 72 * $_) * [Math]::PI / 180
        [Drawing.PointF]::new([float](128 + $r * [Math]::Cos($a)), [float]($cy + $r * [Math]::Sin($a)))
    })
    $blue = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(255, 0x00, 0x68, 0xB5))
    $g.FillPolygon($blue, $points)
    $edge = [Drawing.Pen]::new($blue, 2 * $round)
    $edge.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPolygon($edge, $points)
    $font = [Drawing.Font]::new('Segoe UI Semibold', 104, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
    $format = [Drawing.StringFormat]::new()
    $format.Alignment = [Drawing.StringAlignment]::Center
    $format.LineAlignment = [Drawing.StringAlignment]::Center
    $g.DrawString('NR', $font, [Drawing.Brushes]::White, [Drawing.RectangleF]::new(0, 14, 256, 256), $format)
    $g.Dispose()
    $bmp
}

function Scaled([Drawing.Bitmap]$Source, [int]$Size) {
    $bmp = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([Drawing.Color]::Transparent)
    $g.DrawImage($Source, 0, 0, $Size, $Size)
    $g.Dispose()
    $bmp
}

# An icon's bitmap: its header counts the colour rows and the AND mask's, the rows run bottom
# up in BGRA with straight alpha, and the mask is left empty because the alpha says it all.
function Dib([Drawing.Bitmap]$Bitmap) {
    $s = $Bitmap.Width
    $data = $Bitmap.LockBits([Drawing.Rectangle]::new(0, 0, $s, $s), [Drawing.Imaging.ImageLockMode]::ReadOnly,
                             [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $pixels = [byte[]]::new($data.Stride * $s)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $pixels, 0, $pixels.Length)
    $Bitmap.UnlockBits($data)
    $mask = [int]([Math]::Ceiling($s / 32.0)) * 4
    $stream = [IO.MemoryStream]::new()
    $w = [IO.BinaryWriter]::new($stream)
    foreach ($value in 40, $s, (2 * $s)) { $w.Write([int]$value) }
    $w.Write([int16]1); $w.Write([int16]32)
    foreach ($value in 0, ($s * $s * 4 + $mask * $s), 0, 0, 0, 0) { $w.Write([int]$value) }
    for ($y = $s - 1; $y -ge 0; $y--) { $w.Write($pixels, $y * $data.Stride, $s * 4) }
    $w.Write([byte[]]::new($mask * $s))
    $w.Flush()
    , $stream.ToArray()
}

$big = Draw-Icon
$images = foreach ($size in 16, 20, 24, 32, 40, 48, 64, 128) {
    $small = Scaled $big $size
    , @($size, (Dib $small))
    $small.Dispose()
}
$png = [IO.MemoryStream]::new()
$big.Save($png, [Drawing.Imaging.ImageFormat]::Png)
$images += , @(256, $png.ToArray())
$big.Dispose()

$stream = [IO.MemoryStream]::new()
$w = [IO.BinaryWriter]::new($stream)
$w.Write([int16]0); $w.Write([int16]1); $w.Write([int16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($image in $images) {
    $side = $(if ($image[0] -ge 256) { 0 } else { $image[0] })
    $w.Write([byte]$side); $w.Write([byte]$side); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([int16]1); $w.Write([int16]32)
    $w.Write([int]$image[1].Length); $w.Write([int]$offset)
    $offset += $image[1].Length
}
foreach ($image in $images) { $w.Write([byte[]]$image[1]) }
$w.Flush()
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Out)) | Out-Null
$temporary = $Out + '.new'
[IO.File]::WriteAllBytes($temporary, $stream.ToArray())
Move-Item -LiteralPath $temporary -Destination $Out -Force
Write-Output ("Drew the setup icon: $Out, " + $images.Count + ' sizes, ' + $stream.Length + ' bytes')
