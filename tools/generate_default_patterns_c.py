import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "examples" / "default_patterns.json"
HEADER = ROOT / "src" / "default_patterns.h"
OUTPUT = ROOT / "src" / "default_patterns.c"

STEP_TYPES = {
    "hold": "STEP_HOLD",
    "move": "STEP_MOVE",
    "jitter": "STEP_JITTER",
    "off_hold": "STEP_OFF_HOLD",
    "off_move": "STEP_OFF_MOVE",
}

MAX_PATTERN_COUNT = 16
MAX_TOTAL_STEPS = 360
MAX_PATTERN_ID_LEN = 31
MAX_PATTERN_NAME_LEN = 47
MAX_STEP_COUNT = 65535
MAX_WEIGHT = 255
MAX_CAPTURE_EVERY = 255


def coord_to_int(value):
    return int(round(float(value) * 1000))


def amplitude_to_int(value):
    return int(round(float(value) * 1000))


def ident(value):
    out = []
    for ch in value:
        if ch.isalnum():
            out.append(ch.lower())
        else:
            out.append("_")
    name = "".join(out).strip("_")
    return name or "pattern"


def c_string(value):
    return json.dumps(str(value), ensure_ascii=False)


def main():
    data = json.loads(SOURCE.read_text(encoding="utf-8"))
    patterns = data["patterns"]
    capture_every = int(data.get("runtime", {}).get("capture_every", 0) or 0)
    total_steps = sum(len(pattern.get("steps", [])) for pattern in patterns)

    if capture_every < 0 or capture_every > MAX_CAPTURE_EVERY:
        raise SystemExit(f"capture_every out of firmware range: {capture_every} > {MAX_CAPTURE_EVERY}")
    if len(patterns) > MAX_PATTERN_COUNT:
        raise SystemExit(f"too many patterns for firmware: {len(patterns)} > {MAX_PATTERN_COUNT}")
    if total_steps > MAX_TOTAL_STEPS:
        raise SystemExit(f"too many steps for firmware: {total_steps} > {MAX_TOTAL_STEPS}")
    for pattern in patterns:
        if len(str(pattern["id"])) > MAX_PATTERN_ID_LEN:
            raise SystemExit(f"pattern id too long: {pattern['id']}")
        if len(str(pattern.get("name", pattern["id"]))) > MAX_PATTERN_NAME_LEN:
            raise SystemExit(f"pattern name too long: {pattern['id']}")
        weight = int(pattern.get("weight", 1) or 0)
        if weight < 0 or weight > MAX_WEIGHT:
            raise SystemExit(f"pattern weight out of firmware range: {pattern['id']}={weight}")
        if len(pattern.get("steps", [])) > MAX_STEP_COUNT:
            raise SystemExit(f"too many steps in pattern: {pattern['id']}")

    HEADER.write_text(
        "#ifndef LASER_CAT_TOY_DEFAULT_PATTERNS_H\n"
        "#define LASER_CAT_TOY_DEFAULT_PATTERNS_H\n\n"
        '#include "pattern.h"\n\n'
        "extern const pattern_pack_t default_pattern_pack;\n\n"
        "#endif\n",
        encoding="utf-8",
    )

    lines = [
        "/* Generated from tools/examples/default_patterns.json. */",
        "#include <stddef.h>",
        "",
        '#include "esp_common.h"',
        '#include "default_patterns.h"',
        "",
        "#define BUILD_ASSERT(name, cond) typedef char build_assert_##name[(cond) ? 1 : -1]",
        "",
    ]

    for pattern in patterns:
        name = ident(pattern["id"])
        lines.append(f"static const pattern_step_t steps_{name}[] ICACHE_RODATA_ATTR = {{")
        for step in pattern["steps"]:
            step_type = STEP_TYPES[step["type"]]
            laser = "true" if bool(step.get("laser", step["type"] not in ("off_hold", "off_move"))) else "false"
            has_position = step["type"] in ("hold", "move", "jitter", "off_move")
            x = coord_to_int(step.get("x", 0.0)) if has_position else 0
            y = coord_to_int(step.get("y", 0.0)) if has_position else 0
            duration = int(step["duration_ms"])
            amplitude = amplitude_to_int(step.get("amplitude", 0.0)) if step["type"] == "jitter" else 0
            lines.append(f"    {{ {step_type}, {laser}, {x}, {y}, {duration}, {amplitude} }},")
        lines.append("};")
        lines.append(f"BUILD_ASSERT({name}_step_count_fits, "
                     f"(sizeof(steps_{name}) / sizeof(steps_{name}[0])) <= 65535);")
        lines.append("")

    lines.append(f"BUILD_ASSERT(default_pattern_count_fits, {len(patterns)} <= 65535);")
    lines.append(f"BUILD_ASSERT(default_total_steps_fits, {total_steps} <= {MAX_TOTAL_STEPS});")
    lines.append("")
    lines.append("static const pattern_t default_patterns[] ICACHE_RODATA_ATTR = {")
    for pattern in patterns:
        name = ident(pattern["id"])
        lines.append(
            f"    {{ {c_string(pattern['id'])}, {c_string(pattern.get('name', pattern['id']))}, "
            f"{int(pattern.get('weight', 1) or 0)}, "
            f"(uint16_t)(sizeof(steps_{name}) / sizeof(steps_{name}[0])), steps_{name} }},"
        )
    lines.append("};")
    lines.append("")
    lines.append("const pattern_pack_t default_pattern_pack ICACHE_RODATA_ATTR = {")
    lines.append("    default_patterns,")
    lines.append("    (uint16_t)(sizeof(default_patterns) / sizeof(default_patterns[0])),")
    lines.append(f"    {capture_every},")
    lines.append('    "compiled default_patterns.json",')
    lines.append("    NULL,")
    lines.append("    0")
    lines.append("};")
    lines.append("")

    OUTPUT.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
