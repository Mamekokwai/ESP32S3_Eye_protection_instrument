#!/usr/bin/env bash
# ============================================================
#  将 AVI 视频烧录到 Flash storage 分区 (内存映射播放)
#
#  用法: ./flash_video.sh              (使用 flash_video.conf 中的配置)
#        ./flash_video.sh video.avi    (指定文件)
#
#  配置: flash_video.conf (同目录)
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$SCRIPT_DIR/flash_video.conf"

# ---- 加载配置 ----
PORT=/dev/ttyACM0
BAUD=921600
FILE=""
if [[ -f "$CONFIG" ]]; then
    source "$CONFIG"
fi

# ---- 参数覆盖配置 ----
AVI_FILE="${1:-$FILE}"
if [[ -z "$AVI_FILE" ]]; then
    echo "用法: $0 video.avi"
    echo "      或在 $CONFIG 中设置 FILE="
    exit 1
fi
if [[ ! -f "$AVI_FILE" ]]; then
    echo "[ERROR] 文件不存在: $AVI_FILE"
    exit 1
fi

# ---- 获取 storage 偏移 (从 partition-table.bin) ----
PART_BIN="$PROJECT_DIR/build/partition_table/partition-table.bin"
STORAGE_OFFSET_HEX="0x110000"  # 默认
if [[ -f "$PART_BIN" ]]; then
    # gen_esp32part.py 输出各分区信息
    PART_TOOL="$IDF_PATH/components/partition_table/gen_esp32part.py"
    if [[ -f "$PART_TOOL" ]]; then
        while IFS=, read -r name _ _ offset _; do
            if [[ "$name" == "storage" ]]; then
                STORAGE_OFFSET_HEX="$(printf '0x%X' "$offset")"
                break
            fi
        done < <(python3 "$PART_TOOL" "$PART_BIN" 2>/dev/null)
    fi
fi
echo "[INFO] storage 偏移: $STORAGE_OFFSET_HEX"

# ---- 校验 ----
FILE_SIZE=$(stat -c%s "$AVI_FILE")
MAX_SIZE=$((14 * 1024 * 1024))
if [[ $FILE_SIZE -gt $MAX_SIZE ]]; then
    echo "[ERROR] 文件太大: $FILE_SIZE > 14MB"
    exit 1
fi

echo "============================================"
echo "  视频: $AVI_FILE ($FILE_SIZE bytes)"
echo "  Flash 偏移: $STORAGE_OFFSET_HEX"
echo "============================================"

python3 -m esptool \
    --chip esp32s3 \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    "$STORAGE_OFFSET_HEX" "$AVI_FILE"

echo "[OK] 烧录完成! 重启 ESP32 即可播放"
