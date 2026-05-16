import json
import subprocess
import sys
from pathlib import Path

import pytest

from tools.chatchat_patterns.binary import (
    MAGIC,
    binary_pack_from_pattern_pack,
    export_dat_file,
    normalized_to_v2_coord,
    pack_binary,
    read_dat_bytes,
)
from tools.chatchat_patterns.geometry import (
    normalized_to_firmware,
    pattern_duration_ms,
    pattern_jitter_zones,
    pattern_pauses,
    pattern_segments,
)
from tools.chatchat_patterns.model import load_pattern_pack, pattern_pack_from_data
from tools.chatchat_patterns.validator import validate_pack_data, validate_pack_file


ROOT = Path(__file__).resolve().parents[1]
EXAMPLES = ROOT / "tools" / "examples"
VALID_EXAMPLE = EXAMPLES / "default_patterns.json"

MINIMAL_PACK = {
    "schema": "laser_cat_patterns.v1",
    "patterns": [
        {
            "id": "mouse_cautious",
            "name": "Souris prudente",
            "category": "prey_simulation",
            "weight": 45,
            "intensity": "low",
            "steps": [
                {
                    "type": "hold",
                    "laser": True,
                    "x": -0.25,
                    "y": -0.15,
                    "duration_ms": 3000,
                },
                {
                    "type": "move",
                    "laser": True,
                    "x": -0.05,
                    "y": -0.12,
                    "duration_ms": 6000,
                },
                {"type": "off_hold", "laser": False, "duration_ms": 1200},
                {
                    "type": "off_move",
                    "laser": False,
                    "x": 0.18,
                    "y": -0.08,
                    "duration_ms": 1800,
                },
                {
                    "type": "jitter",
                    "laser": True,
                    "x": 0.18,
                    "y": -0.08,
                    "duration_ms": 2200,
                    "amplitude": 0.04,
                },
            ],
        }
    ],
}


def test_valid_example_loads_without_errors():
    result = validate_pack_file(VALID_EXAMPLE)

    assert result.ok
    assert result.pattern_count > 0
    assert result.step_count > 0


def test_invalid_examples_are_rejected():
    for filename in [
        "invalid_coord.json",
        "invalid_duration.json",
        "invalid_duplicate_id.json",
        "invalid_jitter.json",
        "invalid_type.json",
    ]:
        result = validate_pack_file(EXAMPLES / filename)
        assert not result.ok, filename


def test_duplicate_ids_report_logical_path():
    result = validate_pack_file(EXAMPLES / "invalid_duplicate_id.json")

    assert any(issue.path == "patterns[1].id" for issue in result.errors)


def test_step_type_rules_require_position_and_amplitude():
    result = validate_pack_file(EXAMPLES / "invalid_jitter.json")

    assert any(issue.path.endswith(".amplitude") for issue in result.errors)


def test_coordinate_conversion():
    assert normalized_to_firmware(-1.0) == -1000
    assert normalized_to_firmware(0.0) == 0
    assert normalized_to_firmware(1.0) == 1000

    with pytest.raises(ValueError):
        normalized_to_firmware(1.01)


def test_v2_coordinate_conversion():
    assert normalized_to_v2_coord(-1.0) == 0
    assert normalized_to_v2_coord(0.0) == 500
    assert normalized_to_v2_coord(1.0) == 1000


def test_pattern_geometry_extracts_duration_segments_pauses_and_jitter():
    pack = pattern_pack_from_data(MINIMAL_PACK)
    pattern = pack.pattern_by_id("mouse_cautious")

    assert pattern is not None
    assert pattern_duration_ms(pattern) == 14200

    segments = pattern_segments(pattern)
    assert len(segments) == 2
    assert segments[0].laser is True
    assert segments[1].laser is False

    pauses = pattern_pauses(pattern)
    assert len(pauses) == 2
    assert pauses[0].laser is True
    assert pauses[1].laser is False

    jitters = pattern_jitter_zones(pattern)
    assert len(jitters) == 1
    assert jitters[0].amplitude == pytest.approx(0.04)


def test_behavior_warning_for_fast_visible_move():
    data = json.loads(json.dumps(MINIMAL_PACK))
    data["patterns"][0]["steps"][1]["duration_ms"] = 500

    result = validate_pack_data(data)

    assert result.ok
    assert any("visible sous 1000 ms" in issue.message for issue in result.warnings)


def test_behavior_warning_for_long_fast_step_and_on_ratio():
    data = json.loads(json.dumps(MINIMAL_PACK))
    steps = data["patterns"][0]["steps"]
    steps[:] = [
        {"type": "hold", "laser": True, "x": 0.0, "y": 0.0, "duration_ms": 3000},
        {"type": "move", "laser": True, "x": 0.9, "y": 0.9, "duration_ms": 3000},
        {"type": "hold", "laser": True, "x": 0.9, "y": 0.9, "duration_ms": 50000},
    ]

    result = validate_pack_data(data)

    assert result.ok
    assert any("distance importante" in issue.message for issue in result.warnings)
    assert any("vitesse relative" in issue.message for issue in result.warnings)
    assert any("sequence laser ON longue" in issue.message for issue in result.warnings)


def test_behavior_errors_for_extreme_motion_and_duration():
    data = json.loads(json.dumps(MINIMAL_PACK))
    steps = data["patterns"][0]["steps"]
    steps[:] = [
        {"type": "hold", "laser": True, "x": -1.0, "y": -1.0, "duration_ms": 3000},
        {"type": "move", "laser": True, "x": 1.0, "y": 1.0, "duration_ms": 3000},
        {"type": "hold", "laser": True, "x": 1.0, "y": 1.0, "duration_ms": 181000},
    ]

    result = validate_pack_data(data)

    assert not result.ok
    assert any("distance excessive" in issue.message for issue in result.errors)
    assert any("vitesse relative excessive" in issue.message for issue in result.errors)
    assert any("duree pattern trop longue" in issue.message for issue in result.errors)


def test_firmware_limits_reject_excessive_ids_and_weight():
    data = json.loads(json.dumps(MINIMAL_PACK))
    data["patterns"][0]["id"] = "x" * 40
    data["patterns"][0]["name"] = "n" * 60
    data["patterns"][0]["weight"] = 300

    result = validate_pack_data(data)

    assert not result.ok
    assert any(issue.path.endswith(".id") for issue in result.errors)
    assert any(issue.path.endswith(".name") for issue in result.errors)
    assert any(issue.path.endswith(".weight") for issue in result.errors)


def test_cli_validate_returns_success_for_valid_file():
    completed = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "validate", str(VALID_EXAMPLE)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 0
    assert "OK" in completed.stdout


def test_cli_validate_returns_failure_for_invalid_file():
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.chatchat_patterns",
            "validate",
            str(EXAMPLES / "invalid_type.json"),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 1
    assert "ERROR" in completed.stdout


def test_cli_list_outputs_pattern_summary():
    first_pattern_id = load_pattern_pack(VALID_EXAMPLE).patterns[0].id
    completed = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "list", str(VALID_EXAMPLE)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 0
    assert first_pattern_id in completed.stdout
    assert "duration_ms=" in completed.stdout


def test_cli_help_lists_gui_command():
    completed = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "--help"],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 0
    assert "gui" in completed.stdout


def test_binary_pack_roundtrip_from_minimal_pack():
    pack = pattern_pack_from_data(MINIMAL_PACK)
    binary_pack = binary_pack_from_pattern_pack(pack)
    data = pack_binary(binary_pack)
    loaded = read_dat_bytes(data)

    assert data[:4] == MAGIC
    assert len(loaded.patterns) == 1
    assert loaded.patterns[0].pattern_id == "mouse_cautious"
    assert loaded.patterns[0].points[0].x == 375
    assert loaded.patterns[0].points[0].y == 425
    assert loaded.patterns[0].points[0].laser == 1
    assert loaded.patterns[0].points[2].laser == 0


def test_binary_pack_rejects_bad_checksum():
    pack = pattern_pack_from_data(MINIMAL_PACK)
    data = bytearray(pack_binary(binary_pack_from_pattern_pack(pack)))
    data[-1] ^= 0x01

    with pytest.raises(ValueError, match="checksum"):
        read_dat_bytes(bytes(data))


def test_cli_export_and_inspect_dat(tmp_path):
    output = tmp_path / "patterns.dat"
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.chatchat_patterns",
            "export-dat",
            str(VALID_EXAMPLE),
            "--output",
            str(output),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 0
    assert output.exists()
    assert "DAT ecrit" in completed.stdout

    inspected = subprocess.run(
        [sys.executable, "-m", "tools.chatchat_patterns", "inspect-dat", str(output)],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert inspected.returncode == 0
    assert "OK" in inspected.stdout
    assert "slow_floor_mouse" in inspected.stdout


def test_export_dat_file_returns_metadata(tmp_path):
    output = tmp_path / "patterns.dat"
    binary_pack = export_dat_file(VALID_EXAMPLE, output)

    assert output.exists()
    assert len(binary_pack.patterns) == 13
    assert binary_pack.point_count == 334
    assert binary_pack.file_size == output.stat().st_size


def test_visualizer_export_png_when_matplotlib_available(tmp_path):
    pytest.importorskip("matplotlib")

    output = tmp_path / "pattern.png"
    first_pattern_id = load_pattern_pack(VALID_EXAMPLE).patterns[0].id
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "tools.chatchat_patterns",
            "view",
            str(VALID_EXAMPLE),
            "--pattern",
            first_pattern_id,
            "--output",
            str(output),
        ],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )

    assert completed.returncode == 0
    assert output.exists()
    assert output.stat().st_size > 0
