#!/usr/bin/env bash
# ============================================================
#  ESP32-S3 RAW 视频转换脚本
#  将任意视频转为 RGB565 .raw 格式 (兼容 raw_player)
#
#  依赖: ffmpeg, python3, numpy (pip install numpy)
#  脚本: convert_video.py (同目录)
#  配置: convert_raw.conf (同目录)
#
#  用法:
#    ./convert_raw.sh video.mp4   → 输出 video.raw
#    ./convert_raw.sh             → 批量转换 convert_raw.conf 中的文件列表
# ============================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$SCRIPT_DIR/convert_raw.conf"
PY_SCRIPT="$SCRIPT_DIR/convert_video.py"

# ---- 加载配置 ----
if [[ -f "$CONFIG" ]]; then
    source "$CONFIG"
else
    echo "[WARN] 未找到 $CONFIG, 使用默认参数"
    SIZE=240x160
    FPS=15
fi

# ---- 检查依赖 ----
check_deps() {
    if ! command -v ffmpeg &>/dev/null; then
        echo "[ERROR] 未找到 ffmpeg: sudo apt install ffmpeg"
        exit 1
    fi
    if ! command -v python3 &>/dev/null; then
        echo "[ERROR] 未找到 python3"
        exit 1
    fi
    if ! python3 -c "import numpy" 2>/dev/null; then
        echo "[ERROR] 未找到 numpy: pip install numpy"
        exit 1
    fi
    if [[ ! -f "$PY_SCRIPT" ]]; then
        echo "[ERROR] 未找到 $PY_SCRIPT"
        exit 1
    fi
    echo "[OK] 依赖检查通过 (ffmpeg + python3 + numpy)"
}

# ---- 转换单个文件 ----
convert_one() {
    local input="$1"
    local output="${input%.*}.raw"

    if [[ ! -f "$input" ]]; then
        echo "[ERROR] 文件不存在: $input"
        return 1
    fi

    echo "============================================"
    echo "  输入: $input"
    echo "  输出: $output"
    echo "  参数: --size $SIZE --fps $FPS"

    python3 "$PY_SCRIPT" "$input" "$output" --size "$SIZE" --fps "$FPS"

    if [[ -f "$output" ]]; then
        local size=$(du -h "$output" | cut -f1)
        echo "[OK] $output ($size)"
    else
        echo "[ERROR] 转换失败"
        return 1
    fi
}

# ---- 主入口 ----
case "${1:-}" in
    "")
        check_deps
        if [[ ${#FILES[@]} -eq 0 ]]; then
            echo "[INFO] FILES 列表为空, 请编辑 $CONFIG 添加文件路径"
            echo "       或直接运行: $0 video.mp4"
            exit 0
        fi
        for f in "${FILES[@]}"; do
            [[ -z "$f" || "$f" == \#* ]] && continue
            convert_one "$f"
        done
        echo ""
        echo "全部完成! 将 .raw 文件复制到 SD 卡即可"
        ;;
    *)
        check_deps
        convert_one "$1"
        echo ""
        echo "将 ${1%.*}.raw 复制到 SD 卡即可"
        ;;
esac
