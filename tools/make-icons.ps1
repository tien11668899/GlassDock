# Draws GlassDock's icons and writes them as .ico files:
#   app\icons\app.ico   the program: a frosted panel rising out of a glass dock, on black
#   app\icons\tray.ico  the notification-area glyph: a dock with three icons, white on nothing
# Each size is drawn at four times its size and scaled down. -Preview <png> also writes every
# size side by side, enlarged, on grey and on black.
# Usage: powershell -File tools\make-icons.ps1 [-Preview dev\icons.png]
param([string]$Preview = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot
$out = Join-Path $root 'app\icons'
New-Item -ItemType Directory -Force $out | Out-Null

function Color([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function White([int]$a) { New-Object System.Drawing.SolidBrush (Color $a 255 255 255) }

function RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $p
}

$appPaint = {
    param($g, [float]$S)
    $tile = RoundedRect 0 0 $S $S (0.22 * $S)
    $from = New-Object System.Drawing.PointF 0, 0
    $to = New-Object System.Drawing.PointF 0, $S
    $g.FillPath((New-Object System.Drawing.Drawing2D.LinearGradientBrush $from, $to, (Color 255 34 34 40), (Color 255 2 2 4)), $tile)
    # Light behind the glass, which the dock bends.
    $glow = New-Object System.Drawing.Drawing2D.GraphicsPath
    $glow.AddEllipse(0.08 * $S, 0.36 * $S, 0.84 * $S, 0.56 * $S)
    $light = New-Object System.Drawing.Drawing2D.PathGradientBrush $glow
    $light.CenterColor = Color 170 70 130 255
    $light.SurroundColors = [System.Drawing.Color[]]@(Color 0 70 130 255)
    $g.SetClip($tile)
    $g.FillPath($light, $glow)
    $g.ResetClip()
    # The Start menu rising out of the dock: frosted, with a bright rim.
    $panel = RoundedRect (0.25 * $S) (0.14 * $S) (0.50 * $S) (0.33 * $S) (0.07 * $S)
    $g.FillPath((White 46), $panel)
    $g.DrawPath((New-Object System.Drawing.Pen (Color 120 255 255 255), (0.018 * $S)), $panel)
    # The dock and three icons in it.
    $dock = RoundedRect (0.12 * $S) (0.55 * $S) (0.76 * $S) (0.25 * $S) (0.09 * $S)
    $g.FillPath((White 72), $dock)
    $g.DrawPath((New-Object System.Drawing.Pen (Color 200 255 255 255), (0.02 * $S)), $dock)
    foreach ($cx in 0.32, 0.5, 0.68) {
        $g.FillPath((White 245), (RoundedRect (($cx - 0.065) * $S) (0.61 * $S) (0.13 * $S) (0.13 * $S) (0.035 * $S)))
    }
}

$trayPaint = {
    param($g, [float]$S)
    $dock = RoundedRect (0.07 * $S) (0.29 * $S) (0.86 * $S) (0.44 * $S) (0.15 * $S)
    $g.DrawPath((New-Object System.Drawing.Pen (Color 255 255 255 255), (0.08 * $S)), $dock)
    foreach ($cx in 0.30, 0.5, 0.70) {
        $g.FillPath((White 255), (RoundedRect (($cx - 0.085) * $S) (0.425 * $S) (0.17 * $S) (0.17 * $S) (0.045 * $S)))
    }
}

function Draw([int]$size, [scriptblock]$paint) {
    $k = 4
    $S = $size * $k
    $big = New-Object System.Drawing.Bitmap $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($big)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    & $paint $g ([float]$S)
    $g.Dispose()
    $small = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($small)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingMode = 'SourceCopy'
    $wrap = New-Object System.Drawing.Imaging.ImageAttributes
    $wrap.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
    $g.DrawImage($big, (New-Object System.Drawing.Rectangle 0, 0, $size, $size), 0, 0, $S, $S, 'Pixel', $wrap)
    $g.Dispose()
    $big.Dispose()
    $small
}

# An icon image as a DIB: BITMAPINFOHEADER (twice the height: colour, then mask), 32 bpp bottom-up
# colour with straight alpha, an empty AND mask.
function Dib([System.Drawing.Bitmap]$b) {
    $w = $b.Width
    $h = $b.Height
    $lock = $b.LockBits((New-Object System.Drawing.Rectangle 0, 0, $w, $h), 'ReadOnly', 'Format32bppArgb')
    $pixels = New-Object byte[] ($w * $h * 4)
    [Runtime.InteropServices.Marshal]::Copy($lock.Scan0, $pixels, 0, $pixels.Length)
    $b.UnlockBits($lock)
    $maskRow = [int]([Math]::Ceiling($w / 32) * 4)
    $ms = New-Object IO.MemoryStream
    $bw = New-Object IO.BinaryWriter $ms
    $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32](2 * $h)); $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]0); $bw.Write([uint32]($pixels.Length + $maskRow * $h)); $bw.Write([int32]0); $bw.Write([int32]0)
    $bw.Write([uint32]0); $bw.Write([uint32]0)
    for ($y = $h - 1; $y -ge 0; $y--) { $bw.Write($pixels, $y * $w * 4, $w * 4) }
    $bw.Write((New-Object byte[] ($maskRow * $h)))
    $bw.Flush()
    , $ms.ToArray()
}

function Png([System.Drawing.Bitmap]$b) {
    $ms = New-Object IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    , $ms.ToArray()
}

# An .ico of these images: DIBs up to 128 px, PNG for 256.
function Write-Ico([string]$path, [System.Drawing.Bitmap[]]$images) {
    $blobs = @()
    foreach ($b in $images) { $blobs += , $(if ($b.Width -ge 256) { Png $b } else { Dib $b }) }
    $ms = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter $ms
    $w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$images.Count)
    $offset = 6 + 16 * $images.Count
    for ($i = 0; $i -lt $images.Count; $i++) {
        $side = if ($images[$i].Width -ge 256) { 0 } else { $images[$i].Width }
        $w.Write([byte]$side); $w.Write([byte]$side); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([uint16]1); $w.Write([uint16]32); $w.Write([uint32]$blobs[$i].Length); $w.Write([uint32]$offset)
        $offset += $blobs[$i].Length
    }
    foreach ($blob in $blobs) { $w.Write($blob) }
    $w.Flush()
    [IO.File]::WriteAllBytes($path, $ms.ToArray())
}

$appImages = @(16, 20, 24, 32, 40, 48, 64, 256 | ForEach-Object { Draw $_ $appPaint })
$trayImages = @(16, 20, 24, 32, 40, 48, 64 | ForEach-Object { Draw $_ $trayPaint })
Write-Ico (Join-Path $out 'app.ico') $appImages
Write-Ico (Join-Path $out 'tray.ico') $trayImages
"wrote $out\app.ico ($($appImages.Count) sizes) and $out\tray.ico ($($trayImages.Count) sizes)"

if ($Preview) {
    $scale = 3
    $all = $appImages + $trayImages
    $width = ($all | ForEach-Object { $_.Width * $scale + 12 } | Measure-Object -Sum).Sum
    $height = (256 * 1) + 2 * (64 * $scale + 12)
    $sheet = New-Object System.Drawing.Bitmap ([int]$width), ([int]$height)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.InterpolationMode = 'NearestNeighbor'
    $g.PixelOffsetMode = 'Half'
    $g.FillRectangle((New-Object System.Drawing.SolidBrush (Color 255 128 128 128)), 0, 0, $width, $height / 2)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush (Color 255 0 0 0)), 0, $height / 2, $width, $height / 2)
    foreach ($row in 0, 1) {
        $x = 6
        foreach ($b in $all) {
            $side = [Math]::Min($b.Width * $scale, 256)
            $g.DrawImage($b, $x, 6 + $row * $height / 2, $side, $side)
            $x += $side + 12
        }
    }
    $g.Dispose()
    $sheet.Save($Preview)
    $sheet.Dispose()
    "preview: $Preview"
}
