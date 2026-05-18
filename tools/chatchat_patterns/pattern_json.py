from __future__ import annotations

import json
import math
from pathlib import Path

from .binary import (
    BinaryPattern,
    BinaryPoint,
    LPTN_ACTION_HOLD,
    LPTN_ACTION_JITTER,
    LPTN_ACTION_MOVE,
    LPTN_ACTION_OFF_HOLD,
    LPTN_ACTION_OFF_MOVE,
    MAX_JITTER_AMPLITUDE,
    MAX_POINTS_PER_PATTERN,
    MIN_POINT_DURATION_MS,
)

ACTION_BY_TYPE = {
    "hold": LPTN_ACTION_HOLD,
    "move": LPTN_ACTION_MOVE,
    "jitter": LPTN_ACTION_JITTER,
    "off_hold": LPTN_ACTION_OFF_HOLD,
    "off_move": LPTN_ACTION_OFF_MOVE,
}

DEFAULT_SCHEMA = "laser_cat_patterns.v1"


def _clamp_norm(v: float) -> float:
    return max(-1.0, min(1.0, float(v)))


def _norm_to_u16(v: float) -> int:
    clamped = _clamp_norm(v)
    return int(round((clamped + 1.0) * 500.0))


def _u16_to_norm(v: int) -> float:
    return (float(v) - 500.0) / 500.0


def _split_duration(total_ms: int, count: int) -> list[int]:
    base = total_ms // count
    rem = total_ms % count
    return [base + (1 if i < rem else 0) for i in range(count)]


def _duration_min(ms: int) -> int:
    return max(MIN_POINT_DURATION_MS, int(ms))


def _expand_curve(step: dict, start_x: float, start_y: float) -> list[dict]:
    end_x = _clamp_norm(step.get("x", start_x))
    end_y = _clamp_norm(step.get("y", start_y))
    cx = _clamp_norm(step["cx"])
    cy = _clamp_norm(step["cy"])
    duration_ms = int(step["duration_ms"])
    n = max(2, int(step.get("steps", 10)))
    if duration_ms // n < MIN_POINT_DURATION_MS:
        n = max(2, duration_ms // MIN_POINT_DURATION_MS)
    durations = _split_duration(duration_ms, n)
    laser = bool(step.get("laser", True))

    out: list[dict] = []
    for i in range(1, n + 1):
        t = i / n
        inv = 1.0 - t
        x = inv * inv * start_x + 2.0 * inv * t * cx + t * t * end_x
        y = inv * inv * start_y + 2.0 * inv * t * cy + t * t * end_y
        out.append(
            {
                "type": "move",
                "laser": laser,
                "x": _clamp_norm(x),
                "y": _clamp_norm(y),
                "duration_ms": _duration_min(durations[i - 1]),
            }
        )
    return out


def _expand_arc(step: dict) -> list[dict]:
    cx = _clamp_norm(step["cx"])
    cy = _clamp_norm(step["cy"])
    rx = abs(float(step.get("radius_x", 0.0)))
    ry = abs(float(step.get("radius_y", rx)))
    a0 = float(step["angle_start_deg"])
    a1 = float(step["angle_end_deg"])
    duration_ms = int(step["duration_ms"])
    n = max(2, int(step.get("steps", 16)))
    if duration_ms // n < MIN_POINT_DURATION_MS:
        n = max(2, duration_ms // MIN_POINT_DURATION_MS)
    durations = _split_duration(duration_ms, n)
    laser = bool(step.get("laser", True))

    out: list[dict] = []
    for i in range(1, n + 1):
        t = i / n
        a = math.radians(a0 + (a1 - a0) * t)
        x = cx + rx * math.cos(a)
        y = cy + ry * math.sin(a)
        out.append(
            {
                "type": "move",
                "laser": laser,
                "x": _clamp_norm(x),
                "y": _clamp_norm(y),
                "duration_ms": _duration_min(durations[i - 1]),
            }
        )
    return out


def _expand_lemniscate(step: dict) -> list[dict]:
    cx = _clamp_norm(step["cx"])
    cy = _clamp_norm(step["cy"])
    r = abs(float(step["radius"]))
    duration_ms = int(step["duration_ms"])
    n = max(4, int(step.get("steps", 24)))
    if duration_ms // n < MIN_POINT_DURATION_MS:
        n = max(4, duration_ms // MIN_POINT_DURATION_MS)
    n1 = n // 2
    n2 = n - n1
    d1 = (duration_ms * n1) // n
    d2 = duration_ms - d1
    laser = bool(step.get("laser", True))
    arc_a = {
        "type": "arc",
        "laser": laser,
        "cx": cx - (r / 2.0),
        "cy": cy,
        "radius_x": r / 2.0,
        "radius_y": r,
        "angle_start_deg": -90.0,
        "angle_end_deg": 90.0,
        "duration_ms": d1,
        "steps": n1,
    }
    arc_b = {
        "type": "arc",
        "laser": laser,
        "cx": cx + (r / 2.0),
        "cy": cy,
        "radius_x": r / 2.0,
        "radius_y": r,
        "angle_start_deg": 90.0,
        "angle_end_deg": 270.0,
        "duration_ms": d2,
        "steps": n2,
    }
    return _expand_arc(arc_a) + _expand_arc(arc_b)


def expand_steps(steps: list[dict]) -> list[dict]:
    expanded: list[dict] = []
    current_x = 0.0
    current_y = 0.0

    for step in steps:
        t = str(step.get("type", "")).lower()
        if t == "curve":
            chunk = _expand_curve(step, current_x, current_y)
        elif t == "arc":
            chunk = _expand_arc(step)
        elif t == "lemniscate":
            chunk = _expand_lemniscate(step)
        else:
            chunk = [dict(step)]

        for point in chunk:
            pt = str(point["type"]).lower()
            if pt in ("hold", "move", "jitter", "off_move"):
                current_x = _clamp_norm(point.get("x", current_x))
                current_y = _clamp_norm(point.get("y", current_y))
                point["x"] = current_x
                point["y"] = current_y
            point["duration_ms"] = _duration_min(int(point["duration_ms"]))
            expanded.append(point)

    return expanded


def json_data_to_binary_patterns(data: dict) -> list[BinaryPattern]:
    if "patterns" not in data or not isinstance(data["patterns"], list):
        raise ValueError("JSON invalide: champ patterns absent")
    out: list[BinaryPattern] = []
    for p in data["patterns"]:
        pattern_id = str(p["id"])
        weight = int(p.get("weight", 1))
        flags = int(p.get("flags", 0))
        expanded = expand_steps(list(p.get("steps", [])))
        points: list[BinaryPoint] = []
        for step in expanded:
            t = str(step.get("type", "")).lower()
            if t not in ACTION_BY_TYPE:
                raise ValueError(f"type de step non supporte: {t}")
            action = ACTION_BY_TYPE[t]
            laser = 1 if bool(step.get("laser", t not in ("off_hold", "off_move"))) else 0
            if action in (LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE):
                laser = 0
            x = _norm_to_u16(step.get("x", 0.0)) if t in ("hold", "move", "jitter", "off_move") else 500
            y = _norm_to_u16(step.get("y", 0.0)) if t in ("hold", "move", "jitter", "off_move") else 500
            duration_ms = _duration_min(int(step["duration_ms"]))
            arg = 0
            if action == LPTN_ACTION_JITTER:
                arg = max(0, min(MAX_JITTER_AMPLITUDE, int(round(abs(float(step.get("amplitude", 0.0))) * 1000.0))))
            points.append(BinaryPoint(x=x, y=y, duration_ms=duration_ms, laser=laser, action=action, arg=arg))
        if len(points) == 0:
            raise ValueError(f"pattern vide: {pattern_id}")
        if len(points) > MAX_POINTS_PER_PATTERN:
            raise ValueError(f"trop de points (> {MAX_POINTS_PER_PATTERN}) pour {pattern_id}")
        out.append(
            BinaryPattern(
                pattern_id=pattern_id,
                weight=weight,
                flags=flags,
                duration_total_ms=0,
                point_offset=0,
                pattern_crc32=0,
                x_min=0,
                x_max=0,
                y_min=0,
                y_max=0,
                points=points,
            )
        )
    return out


def binary_pack_to_json_data(pack, source_name: str = "imported_dat") -> dict:
    patterns = []
    for p in pack.patterns:
        steps = []
        for pt in p.points:
            step_type = "move"
            if pt.action == LPTN_ACTION_HOLD:
                step_type = "hold"
            elif pt.action == LPTN_ACTION_MOVE:
                step_type = "move"
            elif pt.action == LPTN_ACTION_JITTER:
                step_type = "jitter"
            elif pt.action == LPTN_ACTION_OFF_HOLD:
                step_type = "off_hold"
            elif pt.action == LPTN_ACTION_OFF_MOVE:
                step_type = "off_move"
            row = {
                "type": step_type,
                "laser": bool(pt.laser),
                "duration_ms": int(pt.duration_ms),
            }
            if step_type in ("hold", "move", "jitter", "off_move"):
                row["x"] = round(_u16_to_norm(pt.x), 4)
                row["y"] = round(_u16_to_norm(pt.y), 4)
            if step_type == "jitter":
                row["amplitude"] = round(float(pt.arg) / 1000.0, 4)
            steps.append(row)
        patterns.append(
            {
                "id": p.pattern_id,
                "name": p.pattern_id,
                "weight": int(p.weight),
                "flags": int(p.flags),
                "steps": steps,
            }
        )
    return {
        "schema": DEFAULT_SCHEMA,
        "meta": {"name": source_name, "version": 1},
        "runtime": {"selection_mode": "weighted_random"},
        "patterns": patterns,
    }


def load_json_patterns(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def save_json_patterns(path: str | Path, data: dict) -> None:
    Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
