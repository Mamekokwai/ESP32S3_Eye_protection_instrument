#!/usr/bin/env python3
"""
视频转换工具: 将视频文件转为 RGB565 原始像素格式 (.raw)

用法:
    python convert_video.py input.mp4 output.raw --size 240x160 --fps 15

输出的 .raw 文件格式:
    Header (14 bytes):
        magic:     4B  "RAWV"
        width:     2B  uint16 LE
        height:    2B  uint16 LE
        fps:       2B  uint16 LE
        frames:    4B  uint32 LE
    Frame data:
        每帧 N 帧, 每帧 width * height * 2 bytes (RGB565 LE)

依赖: ffmpeg, numpy (pip install numpy)
"""

import argparse
import struct
import subprocess
import sys
import numpy as np


def parse_size(s: str) -> tuple[int, int]:
    w, h = s.split("x")
    return int(w), int(h)


def rgb888_to_rgb565(r, g, b):
    """RGB888 → RGB565 (little-endian uint16)"""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    val = (r5 << 11) | (g6 << 5) | b5
    return struct.pack("<H", val)


def convert(input_path: str, output_path: str, size: tuple[int, int], fps: int):
    width, height = size

    # Step 1: 用 ffmpeg 提取帧到 stdout (RGB24 原始像素流)
    cmd = [
        "ffmpeg", "-i", input_path,
        "-vf", f"fps={fps},scale={width}:{height}",
        "-f", "rawvideo", "-pix_fmt", "rgb24",
        "-vcodec", "rawvideo", "-an",
        "pipe:1"
    ]
    print(f"Running: {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    frame_size_rgb24 = width * height * 3
    frame_size_rgb565 = width * height * 2

    frames = []
    total_frames = 0

    while True:
        raw = proc.stdout.read(frame_size_rgb24)
        if len(raw) < frame_size_rgb24:
            break

        # RGB24 → RGB565
        arr = np.frombuffer(raw, dtype=np.uint8).reshape((height, width, 3))
        r, g, b = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
        r5 = (r >> 3).astype(np.uint16)
        g6 = (g >> 2).astype(np.uint16)
        b5 = (b >> 3).astype(np.uint16)
        rgb565 = (r5 << 11) | (g6 << 5) | b5

        # 预交换字节: ST7789 SPI 需要大端序, 避免 ESP32 端再做 swap
        rgb565 = ((rgb565 & 0x00FF) << 8) | ((rgb565 & 0xFF00) >> 8)
        frames.append(rgb565.tobytes())
        total_frames += 1

        if total_frames % 100 == 0:
            print(f"  processed {total_frames} frames...")

    proc.terminate()
    print(f"Total frames: {total_frames}")

    if total_frames == 0:
        print("Error: no frames extracted. Check ffmpeg and input file.")
        sys.exit(1)

    # Step 2: 写入 .raw 文件
    with open(output_path, "wb") as f:
        # 写入 header
        f.write(struct.pack("<4sHHHI", b"RAWV", width, height, fps, total_frames))
        # 写入帧数据
        for i, frame in enumerate(frames):
            f.write(frame)
            if (i + 1) % 100 == 0:
                print(f"  writing frame {i + 1}/{total_frames}...")

    file_size_mb = (12 + total_frames * frame_size_rgb565) / (1024 * 1024)
    print(f"Done: {output_path} ({file_size_mb:.1f} MB, {width}x{height}, {fps}fps, {total_frames} frames)")


def main():
    parser = argparse.ArgumentParser(description="Convert video to RGB565 raw format")
    parser.add_argument("input", help="Input video file")
    parser.add_argument("output", help="Output .raw file")
    parser.add_argument("--size", default="240x160", help="Output resolution WxH (default: 240x160)")
    parser.add_argument("--fps", type=int, default=15, help="Output frame rate (default: 15)")
    args = parser.parse_args()

    size = parse_size(args.size)
    convert(args.input, args.output, size, args.fps)


if __name__ == "__main__":
    main()
