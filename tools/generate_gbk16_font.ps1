# ============================================================
# 生成 GBK16.FON 中文字库 (16x16 点阵, 标准 94x190 布局)
#
# 布局: 区号 qh (0x81-0xFE, 94 区) x 位号 ql (0x40-0xFE 跳过 0x7F, 190 位)
#   偏移 = ((qh - 0x81) * 190 + ql_index) * 32, 每字 32 字节 (16x16)
# 与 sample/13_spi_sdcard text.c text_get_hz_mat() 偏移算法一致
#
# 用法: pwsh -File tools/generate_gbk16_font.ps1 -Output gbk16_font.bin
# ============================================================
param(
    [string]$Output = "tools/gbk16_font.bin",
    [string]$FontName = "SimSun"
)

Add-Type -AssemblyName System.Drawing

$ROWS = 94      # 区号 0x81..0xFE
$COLS = 190     # 位号 0x40..0xFE 跳过 0x7F
$GLYPH_BYTES = 32  # 16x16 = 256 bit = 32 bytes

$font = New-Object System.Drawing.Font($FontName, 16,
        [System.Drawing.FontStyle]::Regular,
        [System.Drawing.GraphicsUnit]::Pixel)

$bitmap = New-Object System.Drawing.Bitmap(16, 16,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$graphics.Clear([System.Drawing.Color]::White)

$stream = [System.IO.File]::Create($Output)
$total = 0

for ($qh = 0x81; $qh -le 0xFE; $qh++) {
    for ($ql = 0x40; $ql -le 0xFE; $ql++) {
        if ($ql -eq 0x7F) { continue }   # GBK 无 0x7F 位号

        # 构造 GBK 双字节编码并转成 Unicode 字符
        $gbkBytes = [byte[]]@($qh, $ql)
        $gbkEncoding = [System.Text.Encoding]::GetEncoding(936)  # GBK
        $char = $gbkEncoding.GetString($gbkBytes)

        # 渲染到 16x16 位图
        $graphics.Clear([System.Drawing.Color]::White)
        $graphics.DrawString($char, $font,
                [System.Drawing.Brushes]::Black, 0, 0)

        # 逐像素取点阵 (每字节高位在前, 行优先)
        for ($row = 0; $row -lt 16; $row++) {
            $byteHigh = 0
            $byteLow = 0
            for ($col = 0; $col -lt 8; $col++) {
                $pixel = $bitmap.GetPixel($col, $row)
                if ($pixel.R -lt 128) { $byteHigh = $byteHigh -bor (0x80 -shr $col) }
            }
            for ($col = 0; $col -lt 8; $col++) {
                $pixel = $bitmap.GetPixel($col + 8, $row)
                if ($pixel.R -lt 128) { $byteLow = $byteLow -bor (0x80 -shr $col) }
            }
            $stream.WriteByte([byte]$byteHigh)
            $stream.WriteByte([byte]$byteLow)
            $total++
        }
    }
}

$stream.Close()
$graphics.Dispose()
$bitmap.Dispose()
$font.Dispose()

Write-Output "GBK16 字库已生成: $Output"
Write-Output "区数: $ROWS, 每区字数: $COLS, 总字数: $($ROWS * $COLS)"
Write-Output "文件大小: $((Get-Item $Output).Length) 字节"
