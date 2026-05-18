from __future__ import annotations

import argparse
from pathlib import Path

from .app_service import PatternAppService
from .visualizer import render_binary_pattern

SERVICE = PatternAppService()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m tools.chatchat_patterns")
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate_parser = subparsers.add_parser("validate", help="valider un fichier patterns.dat")
    validate_parser.add_argument("dat_file")

    list_parser = subparsers.add_parser("list", help="lister les patterns d'un .dat")
    list_parser.add_argument("dat_file")

    view_parser = subparsers.add_parser("view", help="visualiser un pattern du .dat")
    view_parser.add_argument("dat_file")
    view_parser.add_argument("--pattern", required=True)
    view_parser.add_argument("--output")

    inspect_dat_parser = subparsers.add_parser("inspect-dat", help="inspecter un patterns.dat")
    inspect_dat_parser.add_argument("dat_file")

    inspect_json_parser = subparsers.add_parser("inspect-json", help="inspecter un patterns.json")
    inspect_json_parser.add_argument("json_file")

    export_dat_parser = subparsers.add_parser("export-dat", help="convertir un JSON vers patterns.dat")
    export_dat_parser.add_argument("json_file")
    export_dat_parser.add_argument("--output", required=True)

    import_dat_parser = subparsers.add_parser("import-dat", help="convertir patterns.dat vers JSON")
    import_dat_parser.add_argument("dat_file")
    import_dat_parser.add_argument("--output", required=True)

    gui_parser = subparsers.add_parser("gui", help="ouvrir la GUI patterns.dat")
    gui_parser.add_argument("dat_file", nargs="?")

    args = parser.parse_args(argv)

    if args.command == "validate":
        return _cmd_validate(Path(args.dat_file))
    if args.command == "list":
        return _cmd_list(Path(args.dat_file))
    if args.command == "view":
        return _cmd_view(Path(args.dat_file), args.pattern, args.output)
    if args.command == "inspect-dat":
        return _cmd_inspect_dat(Path(args.dat_file))
    if args.command == "inspect-json":
        return _cmd_inspect_json(Path(args.json_file))
    if args.command == "export-dat":
        return _cmd_export_dat(Path(args.json_file), Path(args.output))
    if args.command == "import-dat":
        return _cmd_import_dat(Path(args.dat_file), Path(args.output))
    if args.command == "gui":
        from .gui import run_gui

        return run_gui(args.dat_file)

    return 2


def _cmd_validate(path: Path) -> int:
    try:
        inspected = SERVICE.load_dat(path)
    except (OSError, ValueError) as exc:
        print(f"ERROR {exc}")
        return 1
    print(inspected.summary)
    print("OK")
    return 0


def _cmd_list(path: Path) -> int:
    try:
        inspected = SERVICE.load_dat(path)
    except (OSError, ValueError) as exc:
        print(f"ERROR {exc}")
        return 1

    for line in SERVICE.list_patterns(inspected.pack):
        print(line)
    return 0


def _cmd_view(path: Path, pattern_id: str, output: str | None) -> int:
    try:
        inspected = SERVICE.load_dat(path)
    except (OSError, ValueError) as exc:
        print(f"ERROR {exc}")
        return 1

    pattern = SERVICE.find_pattern(inspected.pack, pattern_id)
    if pattern is None:
        print(f"ERROR pattern introuvable: {pattern_id}")
        return 1

    try:
        render_binary_pattern(pattern, output)
    except RuntimeError as exc:
        print(f"ERROR {exc}")
        return 1

    if output:
        print(f"PNG ecrit: {output}")
    return 0


def _cmd_inspect_dat(path: Path) -> int:
    return _cmd_validate(path)


def _cmd_inspect_json(path: Path) -> int:
    try:
        inspected = SERVICE.load_json(path)
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR {exc}")
        return 1
    print(inspected.summary)
    print("OK")
    return 0


def _cmd_export_dat(json_path: Path, dat_path: Path) -> int:
    try:
        inspected = SERVICE.export_json_to_dat(json_path, dat_path)
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR {exc}")
        return 1
    print(inspected.summary)
    print(f"DAT ecrit: {dat_path}")
    return 0


def _cmd_import_dat(dat_path: Path, json_path: Path) -> int:
    try:
        data = SERVICE.dat_to_json_data(dat_path)
        SERVICE.save_json(json_path, data)
    except (OSError, ValueError, KeyError) as exc:
        print(f"ERROR {exc}")
        return 1
    print(f"JSON ecrit: {json_path}")
    return 0
