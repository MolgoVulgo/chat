from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

from .model import POSITION_STEP_TYPES, Pattern, PatternPack, PatternStep, load_pattern_pack
from .validator import ValidationIssue, ValidationResult, validate_pack_file


MAGIC = b"LPTN"
FORMAT_VERSION = 1
MAX_USER_PATTERNS = 20
MAX_POINTS_PER_PATTERN = 64
MAX_PATTERN_FILE_SIZE = 65536
MIN_POINT_DURATION_MS = 80
MAX_POINT_DURATION_MS = 10000
MAX_PATTERN_DURATION_MS = 180000
MAX_PATTERN_ID_LEN = 23
MAX_WEIGHT = 255

EASING_LINEAR = 0
EASING_SMOOTHSTEP = 1
EASING_EASE_IN = 2
EASING_EASE_OUT = 3
EASING_EASE_IN_OUT = 4
EASING_HOLD = 5

HEADER_STRUCT = struct.Struct("<4sHHIHHI")
PATTERN_STRUCT = struct.Struct("<24sBBHIHH")
POINT_STRUCT = struct.Struct("<HHHBBH")
HEADER_SIZE = HEADER_STRUCT.size


@dataclass(frozen=True)
class BinaryPoint:
    x: int
    y: int
    duration_ms: int
    laser: int
    easing: int
    flags: int = 0


@dataclass(frozen=True)
class BinaryPattern:
    pattern_id: str
    weight: int
    mode: int
    duration_total_ms: int
    flags: int
    points: list[BinaryPoint]


@dataclass(frozen=True)
class BinaryPack:
    patterns: list[BinaryPattern]
    checksum_crc32: int
    file_size: int

    @property
    def point_count(self) -> int:
        return sum(len(pattern.points) for pattern in self.patterns)


def export_dat_file(json_path: str | Path, output_path: str | Path) -> BinaryPack:
    result = validate_binary_export_file(json_path)
    if not result.ok:
        messages = "\n".join(issue.format() for issue in result.errors)
        raise ValueError(messages)

    pack = load_pattern_pack(json_path)
    binary_pack = binary_pack_from_pattern_pack(pack)
    data = pack_binary(binary_pack)
    Path(output_path).write_bytes(data)
    return read_dat_bytes(data)


def validate_binary_export_file(path: str | Path) -> ValidationResult:
    result = validate_pack_file(path)
    if not result.ok:
        return result

    pack = load_pattern_pack(path)
    _validate_binary_pack_source(pack, result)
    return result


def _validate_binary_pack_source(pack: PatternPack, result: ValidationResult) -> None:
    if len(pack.patterns) > MAX_USER_PATTERNS:
        result.errors.append(
            ValidationIssue("patterns", f"maximum binaire {MAX_USER_PATTERNS} patterns")
        )

    for pattern_index, pattern in enumerate(pack.patterns):
        ppath = f"patterns[{pattern_index}]"
        encoded_id = pattern.id.encode("ascii", errors="ignore")
        if len(encoded_id) != len(pattern.id.encode("utf-8")):
            result.errors.append(ValidationIssue(f"{ppath}.id", "id ASCII requis pour export binaire"))
        if len(encoded_id) > MAX_PATTERN_ID_LEN:
            result.errors.append(
                ValidationIssue(f"{ppath}.id", f"id trop long pour patterns.dat ({MAX_PATTERN_ID_LEN} max)")
            )
        if pattern.weight < 0 or pattern.weight > MAX_WEIGHT:
            result.errors.append(ValidationIssue(f"{ppath}.weight", "poids binaire attendu dans 0..255"))
        if len(pattern.steps) > MAX_POINTS_PER_PATTERN:
            result.errors.append(
                ValidationIssue(f"{ppath}.steps", f"maximum binaire {MAX_POINTS_PER_PATTERN} points")
            )

        duration_total = sum(int(step.duration_ms) for step in pattern.steps)
        if duration_total > MAX_PATTERN_DURATION_MS:
            result.errors.append(
                ValidationIssue(ppath, f"duree binaire trop longue ({duration_total} ms)")
            )
        for step_index, step in enumerate(pattern.steps):
            spath = f"{ppath}.steps[{step_index}]"
            if step.duration_ms < MIN_POINT_DURATION_MS or step.duration_ms > MAX_POINT_DURATION_MS:
                result.errors.append(
                    ValidationIssue(
                        f"{spath}.duration_ms",
                        f"duree binaire hors plage {MIN_POINT_DURATION_MS}..{MAX_POINT_DURATION_MS}",
                    )
                )


def binary_pack_from_pattern_pack(pack: PatternPack) -> BinaryPack:
    patterns = [_binary_pattern_from_pattern(pattern) for pattern in pack.patterns]
    return BinaryPack(patterns=patterns, checksum_crc32=0, file_size=0)


def _binary_pattern_from_pattern(pattern: Pattern) -> BinaryPattern:
    points: list[BinaryPoint] = []
    last_x = 500
    last_y = 500

    for step in pattern.steps:
        if step.type in POSITION_STEP_TYPES:
            last_x = normalized_to_v2_coord(step.x)
            last_y = normalized_to_v2_coord(step.y)
        laser = 0 if step.type in {"off_hold", "off_move"} else 1
        points.append(
            BinaryPoint(
                x=last_x,
                y=last_y,
                duration_ms=int(step.duration_ms),
                laser=laser,
                easing=easing_for_step(step),
            )
        )

    return BinaryPattern(
        pattern_id=pattern.id,
        weight=int(pattern.weight),
        mode=0,
        duration_total_ms=sum(point.duration_ms for point in points),
        flags=0,
        points=points,
    )


def normalized_to_v2_coord(value: float | int | None) -> int:
    if value is None:
        raise ValueError("coordonne manquante")
    coord = int(round((float(value) + 1.0) * 500.0))
    if coord < 0:
        return 0
    if coord > 1000:
        return 1000
    return coord


def easing_for_step(step: PatternStep) -> int:
    raw_easing = str(step.raw.get("easing", "")).lower()
    if raw_easing == "linear":
        return EASING_LINEAR
    if raw_easing == "ease_in":
        return EASING_EASE_IN
    if raw_easing == "ease_out":
        return EASING_EASE_OUT
    if raw_easing == "ease_in_out":
        return EASING_EASE_IN_OUT
    if raw_easing == "hold" or step.type in {"hold", "off_hold"}:
        return EASING_HOLD
    return EASING_SMOOTHSTEP


def pack_binary(binary_pack: BinaryPack) -> bytes:
    payload = bytearray()
    payload.extend(
        HEADER_STRUCT.pack(
            MAGIC,
            FORMAT_VERSION,
            HEADER_SIZE,
            0,
            len(binary_pack.patterns),
            0,
            0,
        )
    )

    for pattern in binary_pack.patterns:
        pattern_id = pattern.pattern_id.encode("ascii")
        payload.extend(
            PATTERN_STRUCT.pack(
                pattern_id[:MAX_PATTERN_ID_LEN].ljust(24, b"\0"),
                pattern.weight,
                pattern.mode,
                len(pattern.points),
                pattern.duration_total_ms,
                pattern.flags,
                0,
            )
        )
        for point in pattern.points:
            payload.extend(
                POINT_STRUCT.pack(
                    point.x,
                    point.y,
                    point.duration_ms,
                    point.laser,
                    point.easing,
                    point.flags,
                )
            )

    file_size = len(payload)
    if file_size > MAX_PATTERN_FILE_SIZE:
        raise ValueError(f"patterns.dat trop grand: {file_size} > {MAX_PATTERN_FILE_SIZE}")
    struct.pack_into("<I", payload, 8, file_size)
    checksum = zlib.crc32(payload) & 0xFFFFFFFF
    struct.pack_into("<I", payload, 16, checksum)
    return bytes(payload)


def read_dat_file(path: str | Path) -> BinaryPack:
    return read_dat_bytes(Path(path).read_bytes())


def read_dat_bytes(data: bytes) -> BinaryPack:
    if len(data) < HEADER_SIZE:
        raise ValueError("patterns.dat trop court")

    magic, version, header_size, file_size, pattern_count, _reserved, checksum = HEADER_STRUCT.unpack_from(data)
    if magic != MAGIC:
        raise ValueError("magic patterns.dat invalide")
    if version != FORMAT_VERSION:
        raise ValueError(f"version patterns.dat inconnue: {version}")
    if header_size != HEADER_SIZE:
        raise ValueError("taille header patterns.dat invalide")
    if file_size != len(data):
        raise ValueError("taille patterns.dat incoherente")
    if file_size > MAX_PATTERN_FILE_SIZE:
        raise ValueError("patterns.dat trop grand")
    if pattern_count > MAX_USER_PATTERNS:
        raise ValueError("trop de patterns dans patterns.dat")

    checksum_data = bytearray(data)
    struct.pack_into("<I", checksum_data, 16, 0)
    actual_checksum = zlib.crc32(checksum_data) & 0xFFFFFFFF
    if actual_checksum != checksum:
        raise ValueError("checksum patterns.dat invalide")

    patterns: list[BinaryPattern] = []
    offset = header_size
    for _ in range(pattern_count):
        if offset + PATTERN_STRUCT.size > len(data):
            raise ValueError("pattern tronque")
        raw_id, weight, mode, point_count, duration_total_ms, flags, _reserved = PATTERN_STRUCT.unpack_from(
            data, offset
        )
        offset += PATTERN_STRUCT.size
        if point_count > MAX_POINTS_PER_PATTERN:
            raise ValueError("trop de points dans un pattern")
        pattern_id = raw_id.split(b"\0", 1)[0].decode("ascii")
        points: list[BinaryPoint] = []
        for _point_index in range(point_count):
            if offset + POINT_STRUCT.size > len(data):
                raise ValueError("point tronque")
            x, y, duration_ms, laser, easing, point_flags = POINT_STRUCT.unpack_from(data, offset)
            offset += POINT_STRUCT.size
            _validate_binary_point(x, y, duration_ms, laser, easing)
            points.append(BinaryPoint(x, y, duration_ms, laser, easing, point_flags))
        patterns.append(BinaryPattern(pattern_id, weight, mode, duration_total_ms, flags, points))

    if offset != len(data):
        raise ValueError("donnees supplementaires dans patterns.dat")

    return BinaryPack(patterns=patterns, checksum_crc32=checksum, file_size=file_size)


def _validate_binary_point(x: int, y: int, duration_ms: int, laser: int, easing: int) -> None:
    if x > 1000 or y > 1000:
        raise ValueError("coordonnees point hors plage")
    if duration_ms < MIN_POINT_DURATION_MS or duration_ms > MAX_POINT_DURATION_MS:
        raise ValueError("duree point hors plage")
    if laser not in (0, 1):
        raise ValueError("laser point invalide")
    if easing not in {
        EASING_LINEAR,
        EASING_SMOOTHSTEP,
        EASING_EASE_IN,
        EASING_EASE_OUT,
        EASING_EASE_IN_OUT,
        EASING_HOLD,
    }:
        raise ValueError("easing point invalide")
