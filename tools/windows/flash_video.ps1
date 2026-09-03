<#
  Windows Flash 媒体烧录工具 (Flash storage 分区)
  等价于 tools/linux/flash_video.sh
  步骤: 收集媒体文件 -> 解析 storage 偏移 -> 打包 -> esptool 烧录
  用法:
    .\flash_video.ps1 video1.avi image1.jpg ...
    .\flash_video.ps1 -Font tools\gbk16_font.bin a.avi b.jpg
    .\flash_video.ps1 -Port COM3 -Baud 921600 a.avi
    不带参数时读 tools\windows\flash_video.conf 的 FILE/FILES
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files,
    [string]$Port = "",
    [int]$Baud = 0,
    [string]$Font = ""
)

$ErrorActionPreference = "Stop"

# --- 路径 ---
$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolsDir   = Split-Path -Parent $scriptDir
$projectDir = Split-Path -Parent $toolsDir
$confPath   = Join-Path $scriptDir "flash_video.conf"

# --- Python 优先 ESP-IDF 环境 ---
$idfPython = Join-Path $env:USERPROFILE ".espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe"
if (Test-Path $idfPython) { $python = $idfPython } else { $python = "python" }

# --- 默认烧录参数 ---
$defaultPort = "COM3"
$defaultBaud = 921600
if (Test-Path $confPath) {
    foreach ($line in Get-Content $confPath -Encoding UTF8) {
        if ($line -match '^PORT\s*=\s*(.+)$') { $defaultPort = $matches[1].Trim() }
        if ($line -match '^BAUD\s*=\s*(\d+)$') { $defaultBaud = [int]$matches[1] }
    }
}
if ([string]::IsNullOrEmpty($Port)) { $Port = $defaultPort }
if ($Baud -eq 0) { $Baud = $defaultBaud }

# --- 收集媒体文件 ---
$mediaFiles = New-Object System.Collections.ArrayList
if ($Files.Count -gt 0) {
    foreach ($f in $Files) { [void]$mediaFiles.Add($f.Trim('"')) }
} elseif (Test-Path $confPath) {
    $inFiles = $false
    $legacyFile = $null
    foreach ($raw in Get-Content $confPath) {
        $line = $raw.Trim()
        if ($line -match '^FILE\s*=\s*"?([^"].*?)"?\s*$') { $legacyFile = $matches[1].Trim() }
        if ($line -match '^FILES\s*=\s*\(') { $inFiles = $true; continue }
        if ($inFiles) {
            if ($line -match '^\)') { $inFiles = $false; continue }
            if ($line -notmatch '^\s*#') {
                $m = [regex]::Match($line, '"?([^"]+\.\w+)"?')
                if ($m.Success) { [void]$mediaFiles.Add($m.Groups[1].Value.Trim()) }
            }
        }
    }
    # Keep FILE only as a fallback when FILES is empty.
    if ($mediaFiles.Count -eq 0 -and $legacyFile) {
        [void]$mediaFiles.Add($legacyFile)
    }
}

if ($mediaFiles.Count -eq 0) {
    Write-Host "用法: .\flash_video.ps1 video1.avi image1.jpg ..." -ForegroundColor Yellow
    Write-Host "      或编辑 tools\windows\flash_video.conf 里的 FILE/FILES" -ForegroundColor Yellow
    exit 1
}

# Convert relative paths to absolute paths.
for ($i = 0; $i -lt $mediaFiles.Count; $i++) {
    if (-not [IO.Path]::IsPathRooted($mediaFiles[$i])) {
        $mediaFiles[$i] = Join-Path $projectDir $mediaFiles[$i]
    }
    if (-not (Test-Path $mediaFiles[$i])) {
        Write-Host "[ERROR] 文件不存在: $($mediaFiles[$i])" -ForegroundColor Red
        exit 1
    }
}

# --- 解析 storage 偏移 ---
$partBin = Join-Path $projectDir "build\partition_table\partition-table.bin"
$idfRoot = $env:IDF_PATH
if ([string]::IsNullOrWhiteSpace($idfRoot)) {
    $settingsPath = Join-Path $projectDir ".vscode\settings.json"
    if (Test-Path $settingsPath) {
        try {
            $settings = Get-Content $settingsPath -Raw | ConvertFrom-Json
            $idfRoot = $settings.'idf.currentSetup'
        } catch {
            $idfRoot = $null
        }
    }
}
$partTool = if ($idfRoot) {
    Join-Path $idfRoot "components\partition_table\gen_esp32part.py"
} else {
    $null
}
$storageOffset = "0x110000"
if ((Test-Path $partBin) -and (Test-Path $partTool)) {
    # gen_esp32part.py 将正常诊断写到 stderr；Stop 模式下需临时放宽，
    # 否则 PowerShell 会把“Parsing binary partition input...”当成异常。
    $savedErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        # 合并 stderr 并滤掉 gen_esp32part.py 的诊断提示 ("Parsing binary partition input...")
        $full = & $python $partTool $partBin 2>&1
        $tbl = @($full | Where-Object { $_ -is [string] -and $_ -notmatch '^Parsing' })
    } finally {
        $ErrorActionPreference = $savedErrorAction
    }
    $stRow = $tbl | Where-Object { $_ -match '^storage,' } | Select-Object -First 1
    if ($stRow) {
        $fields = $stRow -split ','
        if ($fields.Count -ge 4) {
            $off = [Convert]::ToUInt32($fields[3], 16)
            $storageOffset = "0x{0:X}" -f $off
        }
    }
}
Write-Host "[INFO] storage 偏移: $storageOffset"

# --- 打包镜像 ---
$packer = Join-Path $toolsDir "linux\flash_media_pack.py"
if (-not (Test-Path $packer)) {
    Write-Host "[ERROR] 缺少打包工具: $packer" -ForegroundColor Red
    exit 1
}
$packedBin = Join-Path $env:TEMP ("flash_media_" + [guid]::NewGuid().ToString("N") + ".bin")
$maxSize = 14 * 1024 * 1024

$pArgs = New-Object System.Collections.ArrayList
[void]$pArgs.Add($python)
[void]$pArgs.Add($packer)
[void]$pArgs.Add("--max-size"); [void]$pArgs.Add("$maxSize")
if ($Font) { [void]$pArgs.Add("--font"); [void]$pArgs.Add($Font) }
[void]$pArgs.Add($packedBin)
foreach ($f in $mediaFiles) { [void]$pArgs.Add($f) }

& $pArgs[0] $pArgs[1..($pArgs.Count - 1)]
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] 打包失败" -ForegroundColor Red
    Remove-Item $packedBin -ErrorAction SilentlyContinue
    exit 1
}
$size = (Get-Item $packedBin).Length

Write-Host "============================================"
Write-Host ("  文件数:    {0}" -f $mediaFiles.Count)
Write-Host ("  镜像大小:  {0} bytes" -f $size)
Write-Host ("  烧录:      {0} @ {1} baud" -f $Port, $Baud)
Write-Host ("  Flash偏移: {0}" -f $storageOffset)
Write-Host "============================================"

# --- 烧录 ---
& $python -m esptool --chip esp32s3 --port $Port --baud $Baud --before default_reset --after hard_reset write_flash $storageOffset $packedBin
$exit = $LASTEXITCODE
Remove-Item $packedBin -ErrorAction SilentlyContinue

if ($exit -eq 0) {
    Write-Host "[OK] 多媒体烧录完成!" -ForegroundColor Green
    Write-Host "     Flash视频: VLIST / VPLAY 序号"
    Write-Host "     Flash图片: FIMGLIST / FIMG 序号"
} else {
    Write-Host "[ERROR] 烧录失败 (exit=$exit)" -ForegroundColor Red
}
exit $exit
