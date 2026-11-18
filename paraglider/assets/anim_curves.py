# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
"""Rewrite a skeletal animation's float curves (flag 0x4000) into the remake's quantised Hermite form (0x4805).

Works in place: each curve's frame and key areas only shrink, so no pointer moves. Checked against all 76 curves
of vanilla Animal_Octopus (largest relative difference 3e-7).

  uv run --no-project python anim_curves.py IN.bfres OUT.bfres
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

FLOAT_FLAG, GAME_FLAG = 0x4000, 0x4805
RECORD = 0x30


class CurveError(ValueError):
    pass


def curve_records(data: bytes) -> list[int]:
    """Offsets of every curve record of the one skeletal animation in the file."""
    if data.count(b"FSKA") != 1:
        raise CurveError("expected exactly one skeletal animation")
    anim = data.index(b"FSKA")
    bones = struct.unpack_from("<Q", data, anim + 0x28)[0]
    bone_count = struct.unpack_from("<H", data, anim + 0x4C)[0]
    records = []
    for bone in range(bone_count):
        entry = bones + bone * 0x38
        first = struct.unpack_from("<Q", data, entry + 8)[0]
        records += [first + index * RECORD for index in range(data[entry + 0x2E])]
    if len(records) != struct.unpack_from("<i", data, anim + 0x44)[0]:
        raise CurveError("curve records do not add up to the animation's curve count")
    return records


def read_curve(data: bytes, record: int) -> tuple[int, list[float], list[tuple[float, float]]]:
    """(target, frames, (value, slope) keys) of a float or quantised curve."""
    frames_at, keys_at, flag, count, target = struct.unpack_from("<QQHHI", data, record)
    frame_scale, scale, offset = struct.unpack_from("<fff", data, record + 0x20)
    if flag & 0x0400 or flag & 0x00F0:
        raise CurveError(f"curve flag {flag:#06x} is outside the handled class")
    frame_type, key_type = flag & 3, flag >> 2 & 3
    if frame_type == 0:
        frames = list(struct.unpack_from(f"<{count}f", data, frames_at))
    elif frame_type == 1:
        frames = [raw / 32 for raw in struct.unpack_from(f"<{count}H", data, frames_at)]
    else:
        frames = [float(raw) for raw in struct.unpack_from(f"<{count}B", data, frames_at)]
    if key_type == 0:
        raw = struct.unpack_from(f"<{2 * count}f", data, keys_at)
        keys = [(raw[2 * i], raw[2 * i + 1]) for i in range(count)]
    else:
        slope_scale, slope_offset = struct.unpack_from("<ff", data, keys_at)
        raw = struct.unpack_from(f"<{2 * count}{'H' if key_type == 1 else 'B'}", data, keys_at + 8)
        keys = [(raw[2 * i] * scale + offset, raw[2 * i + 1] * slope_scale + slope_offset) for i in range(count)]
    return target, frames, keys


def _quantise(values: list[float]) -> tuple[float, float, list[int]]:
    low, high = min(values), max(values)
    scale = struct.unpack("<f", struct.pack("<f", (high - low) / 65535 or 1e-9))[0]
    offset = struct.unpack("<f", struct.pack("<f", low))[0]
    return scale, offset, [min(65535, max(0, round((value - offset) / scale))) for value in values]


def requantise(data: bytes) -> bytes:
    """Float curves -> the game's 16-bit form, in place."""
    out = bytearray(data)
    for record in curve_records(data):
        frames_at, keys_at, flag, count, _ = struct.unpack_from("<QQHHI", data, record)
        if flag != FLOAT_FLAG:
            raise CurveError(f"curve at {record:#x} has flag {flag:#06x}, expected the library's {FLOAT_FLAG:#06x}")
        _, frames, keys = read_curve(data, record)
        if count < 2 or any(frame != int(frame) or not 0 <= frame < 2048 for frame in frames):
            raise CurveError(f"curve at {record:#x} cannot be stored with 16-bit frame numbers")
        scale, offset, values = _quantise([value for value, _ in keys])
        slope_scale, slope_offset, slopes = _quantise([slope for _, slope in keys])
        out[frames_at:frames_at + 4 * count] = bytes(4 * count)
        out[keys_at:keys_at + 8 * count] = bytes(8 * count)
        struct.pack_into(f"<{count}H", out, frames_at, *(int(frame) * 32 for frame in frames))
        struct.pack_into("<ff", out, keys_at, slope_scale, slope_offset)
        for index in range(count):
            struct.pack_into("<HH", out, keys_at + 8 + 4 * index, values[index], slopes[index])
        struct.pack_into("<H", out, record + 0x10, GAME_FLAG)
        struct.pack_into("<fff", out, record + 0x20, 0.0, scale, offset)

        _, frames_back, keys_back = read_curve(out, record)
        value_span = max(abs(value) for value, _ in keys) + 1e-6
        slope_span = max(abs(slope) for _, slope in keys) + 1e-6
        if frames_back != frames or any(
            abs(a[0] - b[0]) > 1e-4 * value_span or abs(a[1] - b[1]) > 1e-4 * slope_span for a, b in zip(keys, keys_back)
        ):
            raise CurveError(f"curve at {record:#x} does not read back as written")
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    Path(sys.argv[2]).write_bytes(requantise(data))
    print(f"rewrote {len(curve_records(data))} curve(s) in the game's 16-bit form")
    return 0


if __name__ == "__main__":
    sys.exit(main())
