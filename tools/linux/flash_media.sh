#!/usr/bin/env bash
# ============================================================
#  将多个 AVI/JPEG 按参数顺序打包并烧录到 Flash storage 分区
#
#  用法: ./flash_media.sh                         (使用配置中的 FILES/FILE)
#        ./flash_media.sh a.avi b.jpg c.avi       (按给定顺序打包)
#
#  配置: flash_media.conf (同目录)
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG="$SCRIPT_DIR/flash_media.conf"

# ---- 加载配置 ----
PORT=/dev/ttyACM0
BAUD=921600
FILE=""
if [[ -f "$CONFIG" ]]; then
    source "$CONFIG"
fi

# ---- 参数覆盖配置 ----
MEDIA_FILES=()
if [[ $# -gt 0 ]]; then
    MEDIA_FILES=("$@")
elif declare -p FILES >/dev/null 2>&1; then
    MEDIA_FILES=("${FILES[@]}")
elif [[ -n "$FILE" ]]; then
    MEDIA_FILES=("$FILE")
fi

if [[ ${#MEDIA_FILES[@]} -eq 0 ]]; then
    echo "用法: $0 video1.avi image1.jpg video2.avi ..."
    echo "      或在 $CONFIG 中设置 FILES=(...)"
    exit 1
fi
for media_file in "${MEDIA_FILES[@]}"; do
    if [[ ! -f "$media_file" ]]; then
        echo "[ERROR] 文件不存在: $media_file"
        exit 1
    fi
done

# ---- 获取 storage 偏移 (从 partition-table.bin) ----
PART_BIN="$PROJECT_DIR/build/partition_table/partition-table.bin"
STORAGE_OFFSET_HEX="0x110000"  # 默认
MAX_SIZE=$((14 * 1024 * 1024))
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

# ---- 生成带索引的顺序媒体镜像 ----
PACKER="$SCRIPT_DIR/flash_media_pack.py"
if [[ ! -f "$PACKER" ]]; then
    echo "[ERROR] 缺少打包工具: $PACKER"
    exit 1
fi

TEMP_DIR=$(mktemp -d)
trap 'rm -rf -- "$TEMP_DIR"' EXIT
PACKED_BIN="$TEMP_DIR/flash_media.bin"
python3 "$PACKER" --max-size "$MAX_SIZE" \
    "$PACKED_BIN" "${MEDIA_FILES[@]}"
PACKED_SIZE=$(stat -c%s "$PACKED_BIN")

echo "============================================"
echo "  文件数: ${#MEDIA_FILES[@]}"
echo "  镜像大小: $PACKED_SIZE bytes"
echo "  Flash 偏移: $STORAGE_OFFSET_HEX"
echo "============================================"

python3 -m esptool \
    --chip esp32s3 \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default_reset \
    --after hard_reset \
    write_flash \
    "$STORAGE_OFFSET_HEX" "$PACKED_BIN"

echo "[OK] 多媒体烧录完成!"
echo "     Flash视频: VLIST / VPLAY <n>"
echo "     Flash图片: FIMGLIST / FIMG <n>"
