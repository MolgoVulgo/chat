from __future__ import annotations

from dataclasses import dataclass

from .model import Pattern, PatternStep


@dataclass(frozen=True)
class Segment:
    start: tuple[float, float]
    end: tuple[float, float]
    duration_ms: int
    laser: bool
    step_type: str


@dataclass(frozen=True)
class Pause:
    point: tuple[float, float]
    duration_ms: int
    laser: bool
    step_type: str


@dataclass(frozen=True)
class JitterZone:
    center: tuple[float, float]
    amplitude: float
    duration_ms: int
    laser: bool


def normalized_to_firmware(value: float) -> int:
    if value < -1.0 or value > 1.0:
        raise ValueError("coordonne hors plage -1.0..1.0")
    return int(round(value * 1000))


def pattern_duration_ms(pattern: Pattern) -> int:
    return sum(step.duration_ms for step in pattern.steps)


def pattern_segments(pattern: Pattern) -> list[Segment]:
    segments: list[Segment] = []
    current = _first_position(pattern) or (0.0, 0.0)

    for step in pattern.steps:
        if step.type in {"move", "off_move"} and step.x is not None and step.y is not None:
            end = (float(step.x), float(step.y))
            segments.append(
                Segment(
                    start=current,
                    end=end,
                    duration_ms=step.duration_ms,
                    laser=bool(step.laser) and step.type != "off_move",
                    step_type=step.type,
                )
            )
            current = end
        elif step.type in {"hold", "jitter"} and step.x is not None and step.y is not None:
            current = (float(step.x), float(step.y))

    return segments


def pattern_pauses(pattern: Pattern) -> list[Pause]:
    pauses: list[Pause] = []
    current = _first_position(pattern) or (0.0, 0.0)

    for step in pattern.steps:
        if _has_position(step):
            current = (float(step.x), float(step.y))
        if step.type in {"hold", "off_hold"}:
            pauses.append(
                Pause(
                    point=current,
                    duration_ms=step.duration_ms,
                    laser=bool(step.laser) and step.type != "off_hold",
                    step_type=step.type,
                )
            )

    return pauses


def pattern_jitter_zones(pattern: Pattern) -> list[JitterZone]:
    zones = []
    for step in pattern.steps:
        if step.type == "jitter" and step.x is not None and step.y is not None:
            zones.append(
                JitterZone(
                    center=(float(step.x), float(step.y)),
                    amplitude=float(step.amplitude or 0.0),
                    duration_ms=step.duration_ms,
                    laser=bool(step.laser),
                )
            )
    return zones


def relative_speed(segment: Segment) -> float:
    if segment.duration_ms <= 0:
        return 0.0
    dx = segment.end[0] - segment.start[0]
    dy = segment.end[1] - segment.start[1]
    return ((dx * dx + dy * dy) ** 0.5) / (segment.duration_ms / 1000.0)


def _has_position(step: PatternStep) -> bool:
    return step.x is not None and step.y is not None


def _first_position(pattern: Pattern) -> tuple[float, float] | None:
    for step in pattern.steps:
        if _has_position(step):
            return float(step.x), float(step.y)
    return None
