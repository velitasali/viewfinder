Add-Type -AssemblyName System.Drawing

# ---------------------------------------------------------------------------
# Draw one viewfinder icon bitmap at the given size.
# Design: dark rounded background, white L-corner brackets, blue crosshairs.
# ---------------------------------------------------------------------------
function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size,
               [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode    = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode  = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    $g.Clear([System.Drawing.Color]::Transparent)

    $s = [float]$size

    # ---- Rounded background ----------------------------------------
    $r = [int]($s * 0.16)
    $d = $r * 2
    $bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $bgPath.AddArc(  0,        0,       $d, $d, 180, 90)
    $bgPath.AddArc($size-$d,   0,       $d, $d, 270, 90)
    $bgPath.AddArc($size-$d, $size-$d,  $d, $d,   0, 90)
    $bgPath.AddArc(  0,      $size-$d,  $d, $d,  90, 90)
    $bgPath.CloseFigure()
    $bgBrush = New-Object System.Drawing.SolidBrush(
                   [System.Drawing.Color]::FromArgb(255, 14, 14, 18))
    $g.FillPath($bgBrush, $bgPath)
    $bgBrush.Dispose(); $bgPath.Dispose()

    # ---- Corner L-brackets -----------------------------------------
    $margin = $s * 0.13
    $bl     = $s * 0.22
    $thick  = [Math]::Max(1.5, $s * 0.07)
    $pen = New-Object System.Drawing.Pen(
               [System.Drawing.Color]::FromArgb(255, 225, 225, 225), $thick)
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Square
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Square

    $m = $margin; $e = $s - $margin; $b = $bl

    $g.DrawLine($pen, $m,     $m+$b,  $m,   $m  )   # TL vertical
    $g.DrawLine($pen, $m,     $m,     $m+$b,$m  )   # TL horizontal
    $g.DrawLine($pen, $e,     $m+$b,  $e,   $m  )   # TR vertical
    $g.DrawLine($pen, $e,     $m,     $e-$b,$m  )   # TR horizontal
    $g.DrawLine($pen, $m,     $e-$b,  $m,   $e  )   # BL vertical
    $g.DrawLine($pen, $m,     $e,     $m+$b,$e  )   # BL horizontal
    $g.DrawLine($pen, $e,     $e-$b,  $e,   $e  )   # BR vertical
    $g.DrawLine($pen, $e,     $e,     $e-$b,$e  )   # BR horizontal
    $pen.Dispose()

    # ---- Crosshairs + centre dot (32px and above) ------------------
    if ($size -ge 32) {
        $cx = $s / 2.0;  $cy = $s / 2.0
        $cLen  = $s * 0.13
        $cGap  = $s * 0.055
        $cThk  = [Math]::Max(1.0, $s * 0.032)
        $blue  = [System.Drawing.Color]::FromArgb(210, 79, 195, 247)
        $cp = New-Object System.Drawing.Pen($blue, $cThk)
        $cp.StartCap = [System.Drawing.Drawing2D.LineCap]::Square
        $cp.EndCap   = [System.Drawing.Drawing2D.LineCap]::Square

        # horizontal arms
        $g.DrawLine($cp, $cx-$cLen-$cGap, $cy,  $cx-$cGap, $cy)
        $g.DrawLine($cp, $cx+$cGap,       $cy,  $cx+$cLen+$cGap, $cy)
        # vertical arms
        $g.DrawLine($cp, $cx, $cy-$cLen-$cGap,  $cx, $cy-$cGap)
        $g.DrawLine($cp, $cx, $cy+$cGap,        $cx, $cy+$cLen+$cGap)
        $cp.Dispose()

        # centre dot
        $dr = [Math]::Max(2.0, $s * 0.035)
        $db = New-Object System.Drawing.SolidBrush($blue)
        $g.FillEllipse($db, $cx-$dr, $cy-$dr, $dr*2, $dr*2)
        $db.Dispose()
    }

    $g.Dispose()
    return $bmp
}

# ---------------------------------------------------------------------------
# Write multiple bitmaps as a single .ico file (PNG-in-ICO, Vista+).
# ---------------------------------------------------------------------------
function Write-Ico([string]$outPath, [System.Drawing.Bitmap[]]$bitmaps) {
    # Encode each bitmap as PNG bytes
    $pngChunks = foreach ($bmp in $bitmaps) {
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        ,$ms.ToArray()   # , keeps it as array element
        $ms.Dispose()
    }

    $count      = $bitmaps.Count
    $dataOffset = 6 + 16 * $count   # ICONDIR + ICONDIRENTRYs

    $fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create,
                                  [System.IO.FileAccess]::Write)
    $bw = New-Object System.IO.BinaryWriter($fs)

    # ICONDIR (6 bytes)
    $bw.Write([uint16]0)      # reserved
    $bw.Write([uint16]1)      # type = icon
    $bw.Write([uint16]$count)

    # ICONDIRENTRYs (16 bytes each)
    $off = [uint32]$dataOffset
    for ($i = 0; $i -lt $count; $i++) {
        $w = $bitmaps[$i].Width
        $bw.Write([byte]$(if ($w -eq 256) { 0 } else { $w }))  # width
        $bw.Write([byte]$(if ($w -eq 256) { 0 } else { $w }))  # height
        $bw.Write([byte]0)     # colour count (0 = truecolour)
        $bw.Write([byte]0)     # reserved
        $bw.Write([uint16]1)   # planes
        $bw.Write([uint16]32)  # bit depth
        $bw.Write([uint32]$pngChunks[$i].Length)
        $bw.Write([uint32]$off)
        $off += [uint32]$pngChunks[$i].Length
    }

    # Image data
    foreach ($chunk in $pngChunks) { $bw.Write($chunk) }

    $bw.Close(); $fs.Close()
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
$outFile = Join-Path $PSScriptRoot "viewfinder.ico"
$sizes   = @(16, 32, 48, 256)
$bitmaps = $sizes | ForEach-Object { New-IconBitmap $_ }

Write-Ico $outFile $bitmaps

foreach ($b in $bitmaps) { $b.Dispose() }
Write-Host "Icon written: $outFile"
