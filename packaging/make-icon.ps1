<#
.SYNOPSIS
    Genera el icono .ico de EdgeDock Studio a codigo (sin binarios versionados).

.DESCRIPTION
    Dibuja en memoria la identidad visual del panel: tarjeta negra OLED con esquinas
    redondeadas, barra cian (0,255,255) anclada al borde derecho y barra purpura
    (160,32,240) en la base, y la empaqueta como ICO multi-tamano (16-256 px,
    entradas PNG internas, admitidas desde Windows Vista).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File packaging/make-icon.ps1 -OutFile build/EdgeDock.ico
#>
[CmdletBinding()]
param(
    [string]$OutFile = "build/EdgeDock.ico"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

function New-PngBytes {
    param([int]$Size)

    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $Size, $Size
    try {
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $g.Clear([System.Drawing.Color]::Transparent)

            # Tarjeta negra redondeada
            $radio = [Math]::Min([Math]::Max(2.0, $Size * 0.18), $Size / 2)
            $d = $radio * 2
            $path = New-Object System.Drawing.Drawing2D.GraphicsPath
            [void]$path.AddArc(0, 0, $d, $d, 180, 90)
            [void]$path.AddArc($Size - $d, 0, $d, $d, 270, 90)
            [void]$path.AddArc($Size - $d, $Size - $d, $d, $d, 0, 90)
            [void]$path.AddArc(0, $Size - $d, $d, $d, 90, 90)
            $path.CloseFigure()

            $negro   = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::FromArgb(255, 0, 0, 0))
            $cyan    = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::FromArgb(255, 0, 255, 255))
            $purpura = New-Object System.Drawing.SolidBrush -ArgumentList ([System.Drawing.Color]::FromArgb(255, 160, 32, 240))
            $borde   = New-Object System.Drawing.Pen -ArgumentList ([System.Drawing.Color]::FromArgb(255, 64, 64, 64))
            try {
                $g.FillPath($negro, $path)
                $g.SetClip($path)

                $grosor = [Math]::Max(2.0, $Size * 0.13)
                $g.FillRectangle($cyan,    $Size - $grosor, 0, $grosor, $Size)
                $g.FillRectangle($purpura, 0, $Size - $grosor, $Size - $grosor + 1, $grosor)

                $g.ResetClip()
                $g.DrawPath($borde, $path)
            } finally {
                $negro.Dispose(); $cyan.Dispose(); $purpura.Dispose(); $borde.Dispose(); $path.Dispose()
            }
        } finally {
            $g.Dispose()
        }

        $ms = New-Object System.IO.MemoryStream
        try {
            $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,($ms.ToArray())
        } finally {
            $ms.Dispose()
        }
    } finally {
        $bmp.Dispose()
    }
}

function New-IcoFile {
    param([int[]]$Sizes, [string]$Destination)

    $entradas = @()
    foreach ($s in $Sizes) {
        $png = New-PngBytes -Size $s
        $entradas += ,@{ Size = $s; Png = $png }
    }

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter -ArgumentList $ms
    try {
        $bw.Write([uint16]0)                 # reservado
        $bw.Write([uint16]1)                 # tipo: icono
        $bw.Write([uint16]$entradas.Count)

        $offset = 6 + 16 * $entradas.Count
        foreach ($e in $entradas) {
            $dim = [int]$e.Size
            if ($dim -ge 256) { $dim = 0 }   # 256 se codifica como 0
            $bw.Write([byte]$dim)            # ancho
            $bw.Write([byte]$dim)            # alto
            $bw.Write([byte]0)               # colores en paleta
            $bw.Write([byte]0)               # reservado
            $bw.Write([uint16]1)             # planos
            $bw.Write([uint16]32)            # bits por pixel
            $bw.Write([uint32]$e.Png.Length)
            $bw.Write([uint32]$offset)
            $offset += $e.Png.Length
        }
        foreach ($e in $entradas) {
            $bw.Write($e.Png)
        }
        $bw.Flush()
        [System.IO.File]::WriteAllBytes($Destination, $ms.ToArray())
    } finally {
        $bw.Dispose(); $ms.Dispose()
    }

    return $entradas.Count
}

$destino = $OutFile
if (-not [System.IO.Path]::IsPathRooted($destino)) {
    $destino = Join-Path (Get-Location).Path $destino
}
$dir = Split-Path -Parent $destino
if ($dir) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$conteo = New-IcoFile -Sizes $sizes -Destination $destino

$info = Get-Item -LiteralPath $destino
Write-Output ("Icono generado: {0} ({1:N0} bytes, {2} tamanos)" -f $destino, $info.Length, $conteo)
