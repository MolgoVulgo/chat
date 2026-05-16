from __future__ import annotations

import argparse
from pathlib import Path

from .binary import export_dat_file, read_dat_file, validate_binary_export_file
from .geometry import pattern_duration_ms
from .model import load_pattern_pack
from .validator import validate_pack_file
from .visualizer import render_pattern


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m tools.chatchat_patterns")
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate", help="valider un fichier JSON")
    validate_parser.add_argument("json_file")

    list_parser = subparsers.add_parser("list", help="lister les patterns")
    list_parser.add_argument("json_file")

    view_parser = subparsers.add_parser("view", help="visualiser un pattern")
    view_parser.add_argument("json_file")
    view_parser.add_argument("--pattern", required=True)
    view_parser.add_argument("--output")

    gui_parser = subparsers.add_parser("gui", help="ouvrir la GUI de gestion des patterns")
    gui_parser.add_argument("json_file", nargs="?")

    export_dat_parser = subparsers.add_parser("export-dat", help="exporter un JSON vers patterns.dat")
    export_dat_parser.add_argument("json_file")
    export_dat_parser.add_argument("--output", required=True)

    inspect_dat_parser = subparsers.add_parser("inspect-dat", help="inspecter un patterns.dat")
    inspect_dat_parser.add_argument("dat_file")

    args = parser.parse_args(argv)

    if args.command == "validate":
        return _cmd_validate(Path(args.json_file))
    if args.command == "list":
        return _cmd_list(Path(args.json_file))
    if args.command == "view":
        return _cmd_view(Path(args.json_file), args.pattern, args.output)
    if args.command == "gui":
        from .gui import run_gui

        return run_gui(args.json_file)
    if args.command == "export-dat":
        return _cmd_export_dat(Path(args.json_file), Path(args.output))
    if args.command == "inspect-dat":
        return _cmd_inspect_dat(Path(args.dat_file))

    return 2


def _cmd_validate(path: Path) -> int:
    result = validate_pack_file(path)

    print(f"patterns={result.pattern_count} steps={result.step_count}")
    for issue in result.errors:
        print(f"ERROR {issue.format()}")
    for issue in result.warnings:
        print(f"WARN {issue.format()}")

    if result.ok:
        print("OK")
        return 0
    return 1


def _cmd_list(path: Path) -> int:
    result = validate_pack_file(path)
    if not result.ok:
        for issue in result.errors:
            print(f"ERROR {issue.format()}")
        return 1

    pack = load_pattern_pack(path)
    for pattern in pack.patterns:
        print(
            f"{pattern.id}\t{pattern.name}\t{pattern.category}\t"
            f"weight={pattern.weight}\tintensity={pattern.intensity}\t"
            f"steps={len(pattern.steps)}\tduration_ms={pattern_duration_ms(pattern)}"
        )
    return 0


def _cmd_view(path: Path, pattern_id: str, output: str | None) -> int:
    result = validate_pack_file(path)
    if not result.ok:
        for issue in result.errors:
            print(f"ERROR {issue.format()}")
        return 1

    pack = load_pattern_pack(path)
    pattern = pack.pattern_by_id(pattern_id)
    if pattern is None:
        print(f"ERROR pattern introuvable: {pattern_id}")
        return 1

    try:
        render_pattern(pattern, output)
    except RuntimeError as exc:
        print(f"ERROR {exc}")
        return 1

    if output:
        print(f"PNG ecrit: {output}")
    return 0


def _cmd_export_dat(path: Path, output: Path) -> int:
    result = validate_binary_export_file(path)

    print(f"patterns={result.pattern_count} steps={result.step_count}")
    for issue in result.errors:
        print(f"ERROR {issue.format()}")
    for issue in result.warnings:
        print(f"WARN {issue.format()}")
    if not result.ok:
        return 1

    try:
        binary_pack = export_dat_file(path, output)
    except (OSError, ValueError) as exc:
        print(f"ERROR {exc}")
        return 1

    print(
        f"DAT ecrit: {output} patterns={len(binary_pack.patterns)} "
        f"points={binary_pack.point_count} bytes={binary_pack.file_size} "
        f"crc32=0x{binary_pack.checksum_crc32:08x}"
    )
    return 0


def _cmd_inspect_dat(path: Path) -> int:
    try:
        binary_pack = read_dat_file(path)
    except (OSError, ValueError) as exc:
        print(f"ERROR {exc}")
        return 1

    print(
        f"patterns={len(binary_pack.patterns)} points={binary_pack.point_count} "
        f"bytes={binary_pack.file_size} crc32=0x{binary_pack.checksum_crc32:08x}"
    )
    for pattern in binary_pack.patterns:
        print(
            f"{pattern.pattern_id}\tweight={pattern.weight}\t"
            f"points={len(pattern.points)}\tduration_ms={pattern.duration_total_ms}"
        )
    print("OK")
    return 0
