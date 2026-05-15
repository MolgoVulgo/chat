from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .model import POSITION_STEP_TYPES, STEP_TYPES, SUPPORTED_SCHEMA, pattern_pack_from_data


MIN_DURATION_MS = 100
MIN_VISIBLE_DURATION_MS = 1000
MIN_JITTER_DURATION_MS = 1000
MAX_INVISIBLE_HOLD_MS = 10000
MAX_COORD = 1.0


@dataclass(frozen=True)
class ValidationIssue:
    path: str
    message: str

    def format(self) -> str:
        return f"{self.path}: {self.message}" if self.path else self.message


@dataclass
class ValidationResult:
    errors: list[ValidationIssue] = field(default_factory=list)
    warnings: list[ValidationIssue] = field(default_factory=list)
    pattern_count: int = 0
    step_count: int = 0

    @property
    def ok(self) -> bool:
        return not self.errors


def validate_pack_file(path: str | Path) -> ValidationResult:
    try:
        with Path(path).open("r", encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as exc:
        return ValidationResult(
            errors=[ValidationIssue("", f"JSON invalide ligne {exc.lineno}: {exc.msg}")]
        )
    except OSError as exc:
        return ValidationResult(errors=[ValidationIssue("", f"lecture impossible: {exc}")])

    return validate_pack_data(data)


def validate_pack_data(data: Any) -> ValidationResult:
    result = ValidationResult()

    if not isinstance(data, dict):
        result.errors.append(ValidationIssue("", "le document racine doit etre un objet"))
        return result

    schema = data.get("schema")
    if schema != SUPPORTED_SCHEMA:
        result.errors.append(
            ValidationIssue("schema", f"schema supporte attendu: {SUPPORTED_SCHEMA}")
        )

    patterns = data.get("patterns")
    if not isinstance(patterns, list) or not patterns:
        result.errors.append(ValidationIssue("patterns", "tableau non vide attendu"))
        return result

    result.pattern_count = len(patterns)
    seen_ids: set[str] = set()

    for pattern_index, pattern in enumerate(patterns):
        ppath = f"patterns[{pattern_index}]"
        if not isinstance(pattern, dict):
            result.errors.append(ValidationIssue(ppath, "objet pattern attendu"))
            continue

        pattern_id = pattern.get("id")
        if not isinstance(pattern_id, str) or not pattern_id:
            result.errors.append(ValidationIssue(f"{ppath}.id", "id non vide attendu"))
        elif pattern_id in seen_ids:
            result.errors.append(ValidationIssue(f"{ppath}.id", "id duplique"))
        else:
            seen_ids.add(pattern_id)

        steps = pattern.get("steps")
        if not isinstance(steps, list) or not steps:
            result.errors.append(ValidationIssue(f"{ppath}.steps", "tableau non vide attendu"))
            continue

        result.step_count += len(steps)
        _validate_steps(steps, ppath, result)

    return result


def _validate_steps(steps: list[Any], ppath: str, result: ValidationResult) -> None:
    previous_pos: tuple[float, float] | None = None

    for step_index, step in enumerate(steps):
        spath = f"{ppath}.steps[{step_index}]"
        if not isinstance(step, dict):
            result.errors.append(ValidationIssue(spath, "objet step attendu"))
            continue

        step_type = step.get("type")
        if step_type not in STEP_TYPES:
            result.errors.append(ValidationIssue(f"{spath}.type", "type de step inconnu"))
            continue

        duration = _validate_duration(step, spath, result)
        has_position = step_type in POSITION_STEP_TYPES
        current_pos: tuple[float, float] | None = None

        if has_position:
            current_pos = _validate_position(step, spath, result)

        if step_type == "jitter":
            _validate_jitter(step, spath, duration, result)

        _warn_behavior(step_type, spath, duration, previous_pos, current_pos, result)

        if current_pos is not None:
            previous_pos = current_pos


def _validate_duration(step: dict[str, Any], spath: str, result: ValidationResult) -> int | None:
    duration = step.get("duration_ms")
    if not isinstance(duration, int):
        result.errors.append(ValidationIssue(f"{spath}.duration_ms", "entier attendu"))
        return None
    if duration < MIN_DURATION_MS:
        result.errors.append(
            ValidationIssue(f"{spath}.duration_ms", f"duree minimale {MIN_DURATION_MS} ms")
        )
    return duration


def _validate_position(
    step: dict[str, Any], spath: str, result: ValidationResult
) -> tuple[float, float] | None:
    x = step.get("x")
    y = step.get("y")
    ok = True

    if not isinstance(x, (int, float)):
        result.errors.append(ValidationIssue(f"{spath}.x", "coordonne x numerique attendue"))
        ok = False
    elif not -MAX_COORD <= float(x) <= MAX_COORD:
        result.errors.append(ValidationIssue(f"{spath}.x", "coordonne hors plage -1.0..1.0"))
        ok = False

    if not isinstance(y, (int, float)):
        result.errors.append(ValidationIssue(f"{spath}.y", "coordonne y numerique attendue"))
        ok = False
    elif not -MAX_COORD <= float(y) <= MAX_COORD:
        result.errors.append(ValidationIssue(f"{spath}.y", "coordonne hors plage -1.0..1.0"))
        ok = False

    if not ok:
        return None
    return float(x), float(y)


def _validate_jitter(
    step: dict[str, Any], spath: str, duration: int | None, result: ValidationResult
) -> None:
    amplitude = step.get("amplitude")
    if not isinstance(amplitude, (int, float)):
        result.errors.append(ValidationIssue(f"{spath}.amplitude", "amplitude numerique attendue"))
    elif amplitude < 0 or amplitude > MAX_COORD:
        result.errors.append(ValidationIssue(f"{spath}.amplitude", "amplitude hors plage 0..1.0"))

    if duration is not None and duration < MIN_JITTER_DURATION_MS:
        result.warnings.append(
            ValidationIssue(f"{spath}.duration_ms", "jitter potentiellement trop court")
        )


def _warn_behavior(
    step_type: str,
    spath: str,
    duration: int | None,
    previous_pos: tuple[float, float] | None,
    current_pos: tuple[float, float] | None,
    result: ValidationResult,
) -> None:
    if duration is None:
        return

    if step_type in {"hold", "move"} and duration < MIN_VISIBLE_DURATION_MS:
        result.warnings.append(
            ValidationIssue(f"{spath}.duration_ms", "mouvement ou pause visible sous 1000 ms")
        )
    if step_type == "off_hold" and duration > MAX_INVISIBLE_HOLD_MS:
        result.warnings.append(
            ValidationIssue(f"{spath}.duration_ms", "disparition invisible longue")
        )
    if (
        step_type in {"move", "off_move"}
        and previous_pos is not None
        and current_pos is not None
        and duration < MIN_VISIBLE_DURATION_MS
    ):
        dx = current_pos[0] - previous_pos[0]
        dy = current_pos[1] - previous_pos[1]
        distance = (dx * dx + dy * dy) ** 0.5
        if distance > 0.75:
            result.warnings.append(
                ValidationIssue(spath, "saut de coordonnees important pour une duree courte")
            )


def load_valid_pack(path: str | Path):
    result = validate_pack_file(path)
    if not result.ok:
        messages = "\n".join(issue.format() for issue in result.errors)
        raise ValueError(messages)
    with Path(path).open("r", encoding="utf-8") as f:
        return pattern_pack_from_data(json.load(f))
