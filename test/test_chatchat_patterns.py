import struct
import subprocess
import sys
import zlib
from pathlib import Path

from tools.chatchat_patterns.app_service import PatternAppService
from tools.chatchat_patterns.binary import (
    FORMAT_VERSION,
    HEADER_SIZE,
    HEADER_STRUCT,
    INDEX_SIZE,
    INDEX_STRUCT,
    LPTN_ENDIAN_LITTLE,
    LPTN_GLOBAL_FLAG_HAS_CRC,
    LPTN_GLOBAL_FLAG_STRICT_BOUNDS,
    MAGIC,
    POINT_SIZE,
    POINT_STRUCT,
    read_dat_bytes,
    read_dat_file,
)

ROOT = Path(__file__).resolve().parents[1]


def _build_sample_dat_bytes() -> bytes:
    points = [
        POINT_STRUCT.pack(500, 500, 1000, 1, 0, 0),
        POINT_STRUCT.pack(700, 500, 1200, 1, 1, 0),
        POINT_STRUCT.pack(700, 500, 900, 1, 2, 40),
    ]
    points_blob = b"".join(points)
    pattern_crc32 = zlib.crc32(points_blob) & 0xFFFFFFFF

    pattern_count = 1
    index_offset = HEADER_SIZE
    data_offset = index_offset + pattern_count * INDEX_SIZE

    entry = INDEX_STRUCT.pack(
        b"demo\0".ljust(24, b"\0"),
        data_offset,
        3100,
        pattern_crc32,
        3,
        20,
        0,
        500,
        700,
        500,
        500,
    )
    payload = entry + points_blob
    payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF

    file_size = HEADER_SIZE + len(payload)
    flags = LPTN_GLOBAL_FLAG_HAS_CRC | LPTN_GLOBAL_FLAG_STRICT_BOUNDS
    schema_hash = 0x76320001

    header_wo_crc = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        LPTN_ENDIAN_LITTLE,
        HEADER_SIZE,
        file_size,
        0,
        payload_crc32,
        index_offset,
        pattern_count,
        INDEX_SIZE,
        data_offset,
        POINT_SIZE,
        flags,
        schema_hash,
        0,
        0,
    )
    header_crc32 = zlib.crc32(header_wo_crc) & 0xFFFFFFFF

    header = HEADER_STRUCT.pack(
        MAGIC,
        FORMAT_VERSION,
        LPTN_ENDIAN_LITTLE,
        HEADER_SIZE,
        file_size,
        header_crc32,
        payload_crc32,
        index_offset,
        pattern_count,
        INDEX_SIZE,
        data_offset,
        POINT_SIZE,
        flags,
        schema_hash,
        0,
        0,
    )

    return header + payload


def _write_sample_dat(tmp_path: Path) -> Path:
    path = tmp_path / "patterns.dat"
    path.write_bytes(_build_sample_dat_bytes())
    return path


def test_read_dat_file_v2_works(tmp_path: Path):
    sample = _write_sample_dat(tmp_path)
    pack = read_dat_file(sample)
    assert len(pack.patterns) == 1
    assert pack.point_count == 3
    assert pack.patterns[0].pattern_id == "demo"


def test_header_signature_is_valid():
    raw = _build_sample_dat_bytes()
    assert raw[:4] == MAGIC
    assert raw[4] == FORMAT_VERSION


def test_invalid_payload_crc_is_rejected():
    raw = bytearray(_build_sample_dat_bytes())
    raw[-1] ^= 0x01
    try:
        read_dat_bytes(bytes(raw))
    except ValueError as exc:
        assert "CRC" in str(exc)
    else:
        raise AssertionError("invalid CRC should fail")


def test_app_service_lists_patterns(tmp_path: Path):
    sample = _write_sample_dat(tmp_path)
    service = PatternAppService()
    inspected = service.load_dat(sample)
    lines = service.list_patterns(inspected.pack)
    assert len(lines) == 1
    assert lines[0].startswith("demo")


def test_cli_validate_returns_success_for_valid_dat(tmp_path: Path):
    sample = _write_sample_dat(tmp_path)
    completed = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "validate", str(sample)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0
    assert "DAT OK" in completed.stdout


def test_cli_list_returns_success_for_valid_dat(tmp_path: Path):
    sample = _write_sample_dat(tmp_path)
    completed = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "list", str(sample)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0
    assert "demo" in completed.stdout


def test_struct_point_size_matches_spec_v2():
    assert POINT_SIZE == 10
