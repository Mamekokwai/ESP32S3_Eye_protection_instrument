#!/usr/bin/env bash
# ============================================================
#  ESP32-S3 AVI 视频转换脚本
#  将任意视频转为 MJPEG AVI (兼容本项目的 video_player)
#
#  依赖: ffmpeg
#  配置: 同目录下 convert.conf
#
#  用法:
#    ./convert.sh video.mp4     → 输出 video.avi
#    ./convert.sh               → 批量转换 convert.conf 中的文件列表
#    ./convert.sh -c            → 仅检查 ffmpeg 是否可用
# ============================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG="$SCRIPT_DIR/convert.conf"

# ---- 加载配置 ----
if [[ -f "$CONFIG" ]]; then
    source "$CONFIG"
else
    echo "[WARN] 未找到 $CONFIG, 使用默认参数"
    MAX_WIDTH=320
    MAX_HEIGHT=240
    FPS=0
    JPEG_QUALITY=3
    ENABLE_AUDIO=0
    AUDIO_RATE=16000
    AUDIO_CHANNELS=1
fi

# ---- 检查 ffmpeg ----
check_ffmpeg() {
    if ! command -v ffmpeg &>/dev/null; then
        echo "[ERROR] 未找到 ffmpeg, 请先安装: sudo apt install ffmpeg"
        exit 1
    fi
    echo "[OK] ffmpeg $(ffmpeg -version 2>&1 | head -1)"
}

# ---- 转换单个文件 ----
convert_one() {
    local input="$1"
    local output="${input%.*}.avi"

    if [[ ! -f "$input" ]]; then
        echo "[ERROR] 文件不存在: $input"
        return 1
    fi

    echo "============================================"
    echo "  输入: $input"
    echo "  输出: $output"

    # 获取输入信息
    local in_w in_h in_fps
    read -r in_w in_h in_fps < <(ffprobe -v quiet -select_streams v:0 \
        -show_entries stream=width,height,r_frame_rate \
        -of csv=p=0 "$input" 2>/dev/null | head -1 | tr ',' ' ' || true)

    # 处理帧率分数格式 (如 15/1)
    if [[ "$in_fps" == *"/"* ]]; then
        in_fps=$(echo "scale=1; ${in_fps%/*} / ${in_fps#*/}" | bc 2>/dev/null || echo "$in_fps")
    fi

    echo "  原始: ${in_w}x${in_h}, ${in_fps} fps"

    # 构建滤镜
    local vf="scale=${MAX_WIDTH}:${MAX_HEIGHT}:force_original_aspect_ratio=decrease,pad=${MAX_WIDTH}:${MAX_HEIGHT}:(ow-iw)/2:(oh-ih)/2:black"
    echo "  滤镜: 缩放至 ${MAX_WIDTH}x${MAX_HEIGHT} (保持比例+黑边)"

    # 构建 ffmpeg 参数
    local args=(-hide_banner -y -i "$input")

    # 视频编码
    args+=(-vcodec mjpeg -q:v "$JPEG_QUALITY")
    echo "  编码: MJPEG, 质量=$JPEG_QUALITY"

    # 帧率
    if [[ "${FPS:-0}" != "0" ]]; then
        args+=(-r "$FPS")
        echo "  帧率: ${FPS} fps (覆盖原始)"
    else
        echo "  帧率: 保持原始"
    fi

    # 滤镜
    args+=(-vf "$vf")

    # 音频
    if [[ "${ENABLE_AUDIO:-0}" == "1" ]]; then
        args+=(-acodec pcm_s16le -ar "$AUDIO_RATE" -ac "$AUDIO_CHANNELS")
        echo "  音频: PCM ${AUDIO_RATE}Hz ${AUDIO_CHANNELS}ch"
    else
        args+=(-an)
        echo "  音频: 无"
    fi

    # 输出
    args+=("$output")

    # 执行转换
    echo "--------------------------------------------"
    ffmpeg "${args[@]}" 2>&1 | grep -E "frame=|Output|error|Error" || true

    if [[ -f "$output" ]]; then
        local size=$(du -h "$output" | cut -f1)
        local frames=$(ffprobe -v quiet -select_streams v:0 \
            -show_entries stream=nb_frames -of csv=p=0 "$output" 2>/dev/null || echo "?")
        echo "--------------------------------------------"
        echo "[OK] $output ($size, ${frames} 帧)"
    else
        echo "[ERROR] 转换失败"
        return 1
    fi
}

# ---- 主入口 ----
case "${1:-}" in
    -c|--check)
        check_ffmpeg
        ;;
    "")
        check_ffmpeg
        # 批量转换
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
        echo "全部完成! 将 .avi 文件复制到 SD 卡即可"
        ;;
    *)
        check_ffmpeg
        convert_one "$1"
        echo ""
        echo "将 ${1%.*}.avi 复制到 SD 卡即可"
        ;;
esac
