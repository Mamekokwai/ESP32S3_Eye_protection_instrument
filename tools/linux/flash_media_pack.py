#!/usr/bin/env python3
"""Build the raw Flash media image consumed by main/flash_media.c."""

from __future__ import annotations

import argparse
import binascii
import shutil
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

INDEX_SIZE = 4096
MAX_ENTRIES = 63
NAME_SIZE = 48
MAGIC = b"FMD1"
VERSION = 1

HEADER = struct.Struct("<4sHHHHI")
ENTRY = struct.Struct("<BBHIII48s")

TYPE_VIDEO = 1
TYPE_IMAGE = 2


@dataclass
class Media:
    path: Path
    name: str
    media_type: int
    offset: int
    size: int
    crc32: int


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def file_crc32(path: Path) -> int:
    checksum = 0
    with path.open("rb") as source:
        while block := source.read(1024 * 1024):
            checksum = binascii.crc32(block, checksum)
    return checksum & 0xFFFFFFFF


def classify(path: Path) -> int:
    suffix = path.suffix.lower()
    with path.open("rb") as source:
        signature = source.read(12)
    if suffix == ".avi":
        if len(signature) < 12 or signature[:4] != b"RIFF" or signature[8:12] != b"AVI ":
            raise ValueError(f"{path}: 不是有效的RIFF AVI文件")
        return TYPE_VIDEO
    if suffix in {".jpg", ".jpeg"}:
        if len(signature) < 2 or signature[:2] != b"\xFF\xD8":
            raise ValueError(f"{path}: 不是有效的JPEG文件")
        return TYPE_IMAGE
    raise ValueError(f"{path}: 仅支持.avi/.jpg/.jpeg")


def encode_name(name: str) -> bytes:
    encoded = name.encode("utf-8")
    if not encoded or len(encoded) >= NAME_SIZE:
        raise ValueError(f"文件名UTF-8长度必须小于{NAME_SIZE}字节: {name}")
    return encoded + b"\0" * (NAME_SIZE - len(encoded))


def fill(output, count: int, value: int = 0xFF) -> None:
    block = bytes([value]) * min(count, 64 * 1024)
    while count:
        size = min(count, len(block))
        output.write(block[:size])
        count -= size


def build_image(output_path: Path, input_paths: list[Path], max_size: int) -> None:
    if not input_paths:
        raise ValueError("至少需要一个AVI或JPEG文件")
    if len(input_paths) > MAX_ENTRIES:
        raise ValueError(f"文件数超过上限{MAX_ENTRIES}")

    media: list[Media] = []
    names: set[str] = set()
    next_offset = INDEX_SIZE
    for raw_path in input_paths:
        path = raw_path.expanduser().resolve()
        if not path.is_file():
            raise ValueError(f"文件不存在: {path}")
        if path == output_path.resolve():
            raise ValueError("输出文件不能同时作为输入")

        name = path.name
        encode_name(name)
        folded_name = name.casefold()
        if folded_name in names:
            raise ValueError(f"Flash中不允许同名文件: {name}")
        names.add(folded_name)

        media_type = classify(path)
        size = path.stat().st_size
        if size <= 0:
            raise ValueError(f"空文件: {path}")
        if media_type == TYPE_IMAGE and size > 1024 * 1024:
            raise ValueError(f"JPEG超过1MiB限制: {path}")

        offset = align_up(next_offset, INDEX_SIZE)
        end = offset + size
        if end > max_size:
            raise ValueError(
                f"媒体总大小超出storage分区: {end} > {max_size} bytes"
            )
        media.append(
            Media(path, name, media_type, offset, size, file_crc32(path))
        )
        next_offset = end

    header = HEADER.pack(
        MAGIC, VERSION, len(media), ENTRY.size, 0, INDEX_SIZE
    )
    entries = b"".join(
        ENTRY.pack(
            item.media_type,
            0,
            0,
            item.offset,
            item.size,
            item.crc32,
            encode_name(item.name),
        )
        for item in media
    )
    if len(header) + len(entries) > INDEX_SIZE:
        raise ValueError("索引区溢出")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as output:
        output.write(header)
        output.write(entries)
        fill(output, INDEX_SIZE - output.tell())
        for item in media:
            if output.tell() < item.offset:
                fill(output, item.offset - output.tell())
            with item.path.open("rb") as source:
                shutil.copyfileobj(source, output, 1024 * 1024)

    print("Flash媒体顺序:")
    video_index = 0
    image_index = 0
    for item in media:
        if item.media_type == TYPE_VIDEO:
            video_index += 1
            selection = f"VPLAY {video_index}"
            kind = "AVI "
        else:
            image_index += 1
            selection = f"FIMG {image_index}"
            kind = "JPEG"
        print(
            f"  {kind} 0x{item.offset:06X}  {item.size:8d} bytes  "
            f"{selection:<10} {item.name}"
        )
    print(f"打包完成: {output_path} ({output_path.stat().st_size} bytes)")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="打包多个AVI/JPEG到Flash storage媒体镜像"
    )
    parser.add_argument("output", type=Path, help="输出bin文件")
    parser.add_argument("inputs", nargs="+", type=Path, help="按顺序排列的媒体文件")
    parser.add_argument(
        "--max-size",
        type=int,
        default=14 * 1024 * 1024,
        help="storage分区最大字节数",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        build_image(args.output, args.inputs, args.max_size)
    except (OSError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

