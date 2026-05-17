from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .binary import BinaryPack, BinaryPattern, read_dat_file


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
