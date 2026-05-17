from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"LPTN"
FORMAT_VERSION = 2
LPTN_ENDIAN_LITTLE = 1

LPTN_ACTION_HOLD = 0
LPTN_ACTION_MOVE = 1
LPTN_ACTION_JITTER = 2
LPTN_ACTION_OFF_HOLD = 3
LPTN_ACTION_OFF_MOVE = 4

LPTN_GLOBAL_FLAG_HAS_CRC = 0x0001
LPTN_GLOBAL_FLAG_STRICT_BOUNDS = 0x0002

MAX_USER_PATTERNS = 20
MAX_POINTS_PER_PATTERN = 64
MAX_PATTERN_FILE_SIZE = 65536
MIN_POINT_DURATION_MS = 80
MAX_POINT_DURATION_MS = 10000
MAX_JITTER_AMPLITUDE = 1000
MAX_PATTERN_ID_LEN = 23

HEADER_STRUCT = struct.Struct("<4sBBHIIIIHHIHHI2I")
INDEX_STRUCT = struct.Struct("<24sIIIHBBHHHH")
POINT_STRUCT = struct.Struct("<HHHBBH")

HEADER_SIZE = HEADER_STRUCT.size
INDEX_SIZE = INDEX_STRUCT.size
POINT_SIZE = POINT_STRUCT.size


@dataclass(frozen=True)
class BinaryPoint:
    x: int
    y: int
    duration_ms: int
    laser: int
    action: int
    arg: int


@dataclass(frozen=True)
class BinaryPattern:
    pattern_id: str
    weight: int
    flags: int
    duration_total_ms: int
    point_offset: int
    pattern_crc32: int
    x_min: int
    x_max: int
    y_min: int
    y_max: int
    points: list[BinaryPoint]


@dataclass(frozen=True)
class BinaryPack:
    patterns: list[BinaryPattern]
    file_size: int
    header_crc32: int
    payload_crc32: int
    flags: int

    @property
    def point_count(self) -> int:
        return sum(len(pattern.points) for pattern in self.patterns)


def read_dat_file(path: str | Path) -> BinaryPack:
    return read_dat_bytes(Path(path).read_bytes())


def read_dat_bytes(data: bytes) -> BinaryPack:
    if len(data) < HEADER_SIZE:
        raise ValueError("patterns.dat trop court")

    (
        magic,
        version,
        endian,
        header_size,
        file_size,
        header_crc32,
        payload_crc32,
        index_offset,
        pattern_count,
        index_entry_size,
        data_offset,
        point_size,
        flags,
        _schema_hash,
        reserved0,
        reserved1,
    ) = HEADER_STRUCT.unpack_from(data, 0)

    if magic != MAGIC:
        raise ValueError("magic patterns.dat invalide")
    if version != FORMAT_VERSION:
        raise ValueError(f"version patterns.dat inconnue: {version}")
    if endian != LPTN_ENDIAN_LITTLE:
        raise ValueError("endianness patterns.dat non supportee")
    if header_size != HEADER_SIZE:
        raise ValueError("taille header patterns.dat invalide")
    if file_size != len(data):
        raise ValueError("taille patterns.dat incoherente")
    if file_size > MAX_PATTERN_FILE_SIZE:
        raise ValueError("patterns.dat trop grand")
    if index_offset != HEADER_SIZE:
        raise ValueError("index_offset invalide")
    if pattern_count > MAX_USER_PATTERNS:
        raise ValueError("trop de patterns dans patterns.dat")
    if index_entry_size != INDEX_SIZE:
        raise ValueError("index_entry_size invalide")
    if point_size != POINT_SIZE:
        raise ValueError("point_size invalide")
    if reserved0 != 0 or reserved1 != 0:
        raise ValueError("reserved header non nul")

    header_bytes = bytearray(data[:HEADER_SIZE])
    struct.pack_into("<I", header_bytes, 12, 0)
    if (zlib.crc32(header_bytes) & 0xFFFFFFFF) != header_crc32:
        raise ValueError("header CRC invalide")
    if (zlib.crc32(data[index_offset:]) & 0xFFFFFFFF) != payload_crc32:
        raise ValueError("payload CRC invalide")

    if data_offset != HEADER_SIZE + pattern_count * INDEX_SIZE:
        raise ValueError("data_offset invalide")

    patterns: list[BinaryPattern] = []
    for i in range(pattern_count):
        rec_offset = HEADER_SIZE + i * INDEX_SIZE
        (
            raw_name,
            point_offset,
            duration_total_ms,
            pattern_crc32,
            point_count,
            weight,
            pflags,
            x_min,
            x_max,
            y_min,
            y_max,
        ) = INDEX_STRUCT.unpack_from(data, rec_offset)

        pattern_id = raw_name.split(b"\0", 1)[0].decode("ascii", errors="ignore") or f"pattern_{i}"
        if point_count == 0 or point_count > MAX_POINTS_PER_PATTERN:
            raise ValueError("point_count invalide")
        if point_offset < data_offset:
            raise ValueError("point_offset invalide")
        points_end = point_offset + point_count * POINT_SIZE
        if points_end > len(data):
            raise ValueError("points tronques")

        points_blob = data[point_offset:points_end]
        if (zlib.crc32(points_blob) & 0xFFFFFFFF) != pattern_crc32:
            raise ValueError("pattern_crc32 invalide")

        points: list[BinaryPoint] = []
        duration_check = 0
        for j in range(point_count):
            p_off = point_offset + j * POINT_SIZE
            x, y, duration_ms, laser, action, arg = POINT_STRUCT.unpack_from(data, p_off)
            _validate_binary_point(x, y, duration_ms, laser, action, arg)
            duration_check += duration_ms
            points.append(BinaryPoint(x, y, duration_ms, laser, action, arg))

        if duration_check != duration_total_ms:
            raise ValueError("duration_total_ms invalide")

        patterns.append(
            BinaryPattern(
                pattern_id=pattern_id,
                weight=weight,
                flags=pflags,
                duration_total_ms=duration_total_ms,
                point_offset=point_offset,
                pattern_crc32=pattern_crc32,
                x_min=x_min,
                x_max=x_max,
                y_min=y_min,
                y_max=y_max,
                points=points,
            )
        )

    return BinaryPack(
        patterns=patterns,
        file_size=file_size,
        header_crc32=header_crc32,
        payload_crc32=payload_crc32,
        flags=flags,
    )


def _validate_binary_point(x: int, y: int, duration_ms: int, laser: int, action: int, arg: int) -> None:
    if x > 1000 or y > 1000:
        raise ValueError("coordonnees point hors plage")
    if duration_ms < MIN_POINT_DURATION_MS or duration_ms > MAX_POINT_DURATION_MS:
        raise ValueError("duree point hors plage")
    if laser not in (0, 1):
        raise ValueError("laser point invalide")
    if action not in {
        LPTN_ACTION_HOLD,
        LPTN_ACTION_MOVE,
        LPTN_ACTION_JITTER,
        LPTN_ACTION_OFF_HOLD,
        LPTN_ACTION_OFF_MOVE,
    }:
        raise ValueError("action point invalide")
    if action == LPTN_ACTION_JITTER:
        if arg > MAX_JITTER_AMPLITUDE:
            raise ValueError("arg jitter hors plage")
    elif arg != 0:
        raise ValueError("arg non-jitter doit valoir 0")
