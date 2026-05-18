from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .binary import BinaryPack, BinaryPattern, build_dat_bytes, read_dat_bytes, read_dat_file, write_dat_file
from .pattern_json import binary_pack_to_json_data, json_data_to_binary_patterns, load_json_patterns, save_json_patterns


@dataclass(frozen=True)
class DatInspectResult:
    path: Path
    pack: BinaryPack
    summary: str


class PatternAppService:
    def load_dat(self, dat_path: str | Path) -> DatInspectResult:
        pack = read_dat_file(dat_path)
        return DatInspectResult(path=Path(dat_path), pack=pack, summary=self._build_summary(dat_path, pack))

    def find_pattern(self, pack: BinaryPack, pattern_id: str) -> BinaryPattern | None:
        for pattern in pack.patterns:
            if pattern.pattern_id == pattern_id:
                return pattern
        return None

    def list_patterns(self, pack: BinaryPack) -> list[str]:
        lines: list[str] = []
        for pattern in pack.patterns:
            lines.append(
                f"{pattern.pattern_id}\tweight={pattern.weight}\t"
                f"points={len(pattern.points)}\tduration_ms={pattern.duration_total_ms}"
            )
        return lines

    def load_json(self, json_path: str | Path) -> DatInspectResult:
        data = load_json_patterns(json_path)
        patterns = json_data_to_binary_patterns(data)
        raw_dat = build_dat_bytes(patterns)
        pack = read_dat_bytes(raw_dat)
        return DatInspectResult(path=Path(json_path), pack=pack, summary=self._build_summary(json_path, pack))

    def save_json(self, json_path: str | Path, data: dict) -> None:
        save_json_patterns(json_path, data)

    def dat_to_json_data(self, dat_path: str | Path) -> dict:
        pack = read_dat_file(dat_path)
        return binary_pack_to_json_data(pack, source_name=Path(dat_path).stem)

    def export_json_to_dat(self, json_path: str | Path, dat_path: str | Path) -> DatInspectResult:
        data = load_json_patterns(json_path)
        patterns = json_data_to_binary_patterns(data)
        pack = write_dat_file(dat_path, patterns)
        return DatInspectResult(path=Path(dat_path), pack=pack, summary=self._build_summary(dat_path, pack))

    @staticmethod
    def _build_summary(dat_path: str | Path, pack: BinaryPack) -> str:
        lines = [
            "DAT OK",
            f"fichier: {dat_path}",
            f"patterns={len(pack.patterns)}",
            f"points={pack.point_count}",
            f"bytes={pack.file_size}",
            f"header_crc32=0x{pack.header_crc32:08x}",
            f"payload_crc32=0x{pack.payload_crc32:08x}",
            "",
        ]
        for pattern in pack.patterns:
            lines.append(
                f"{pattern.pattern_id}: weight={pattern.weight} "
                f"points={len(pattern.points)} duration_ms={pattern.duration_total_ms}"
            )
        return "\n".join(lines)
