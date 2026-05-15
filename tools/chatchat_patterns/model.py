from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


SUPPORTED_SCHEMA = "laser_cat_patterns.v1"
STEP_TYPES = {"hold", "move", "jitter", "off_hold", "off_move"}
POSITION_STEP_TYPES = {"hold", "move", "jitter", "off_move"}


@dataclass(frozen=True)
class PatternStep:
    type: str
    duration_ms: int
    laser: bool | None = None
    x: float | None = None
    y: float | None = None
    amplitude: float | None = None
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Pattern:
    id: str
    name: str
    category: str
    weight: int
    intensity: str
    description: str
    steps: list[PatternStep]
    raw: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class PatternPack:
    schema: str
    meta: dict[str, Any]
    coordinate_system: dict[str, Any]
    runtime: dict[str, Any]
    patterns: list[Pattern]
    raw: dict[str, Any] = field(default_factory=dict)

    def pattern_by_id(self, pattern_id: str) -> Pattern | None:
        for pattern in self.patterns:
            if pattern.id == pattern_id:
                return pattern
        return None


def read_json_file(path: str | Path) -> Any:
    with Path(path).open("r", encoding="utf-8") as f:
        return json.load(f)


def load_pattern_pack(path: str | Path) -> PatternPack:
    return pattern_pack_from_data(read_json_file(path))


def pattern_pack_from_data(data: dict[str, Any]) -> PatternPack:
    patterns = []
    for pattern_data in data.get("patterns", []):
        steps = [
            PatternStep(
                type=str(step_data.get("type", "")),
                laser=step_data.get("laser"),
                x=step_data.get("x"),
                y=step_data.get("y"),
                duration_ms=step_data.get("duration_ms", 0),
                amplitude=step_data.get("amplitude"),
                raw=step_data,
            )
            for step_data in pattern_data.get("steps", [])
            if isinstance(step_data, dict)
        ]
        patterns.append(
            Pattern(
                id=str(pattern_data.get("id", "")),
                name=str(pattern_data.get("name", "")),
                category=str(pattern_data.get("category", "")),
                weight=int(pattern_data.get("weight", 0) or 0),
                intensity=str(pattern_data.get("intensity", "")),
                description=str(pattern_data.get("description", "")),
                steps=steps,
                raw=pattern_data,
            )
        )

    return PatternPack(
        schema=str(data.get("schema", "")),
        meta=data.get("meta", {}) if isinstance(data.get("meta", {}), dict) else {},
        coordinate_system=(
            data.get("coordinate_system", {})
            if isinstance(data.get("coordinate_system", {}), dict)
            else {}
        ),
        runtime=data.get("runtime", {}) if isinstance(data.get("runtime", {}), dict) else {},
        patterns=patterns,
        raw=data,
    )
