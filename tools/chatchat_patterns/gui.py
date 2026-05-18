from __future__ import annotations

import copy
import json
from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, X, Menu, filedialog, messagebox, simpledialog
from tkinter import Canvas, Listbox, StringVar, Text, Tk
from tkinter import ttk

from .app_service import PatternAppService
from .binary import (
    BinaryPattern,
    BinaryPoint,
    LPTN_ACTION_HOLD,
    LPTN_ACTION_JITTER,
    LPTN_ACTION_MOVE,
    LPTN_ACTION_OFF_HOLD,
    LPTN_ACTION_OFF_MOVE,
    write_dat_file,
)
from .pattern_json import binary_pack_to_json_data, json_data_to_binary_patterns

ACTION_TO_NAME = {
    LPTN_ACTION_HOLD: "hold",
    LPTN_ACTION_MOVE: "move",
    LPTN_ACTION_JITTER: "jitter",
    LPTN_ACTION_OFF_HOLD: "off_hold",
    LPTN_ACTION_OFF_MOVE: "off_move",
}
NAME_TO_ACTION = {v: k for k, v in ACTION_TO_NAME.items()}


class PatternGui:
    def __init__(self, root: Tk, initial_path: Path | None = None) -> None:
        self.service = PatternAppService()
        self.root = root
        self.root.title("CatChat Patterns DAT")

        self.path: Path | None = None
        self.json_path: Path | None = None
        self.source_json_data: dict | None = None
        self.pack = None
        self.status = StringVar(value="Aucun fichier DAT/JSON charge")

        self.zoom = 1.0
        self.pan_x = 0.0
        self.pan_y = 0.0
        self.drag_start: tuple[int, int, float, float] | None = None
        self.zoom_label = StringVar(value="100%")

        self.edit_mode = StringVar(value="0")
        self.edited_points: dict[str, list[dict[str, int]]] = {}
        self.selected_point_index: int | None = None
        self.selected_indices: set[int] = set()
        self.point_drag_start: tuple[int, int, dict[int, tuple[int, int]]] | None = None
        self.selection_box_start: tuple[int, int] | None = None
        self.selection_box_item: int | None = None

        self.undo_max = 5
        self.history: dict[str, list[list[dict[str, int]]]] = {}
        self.redo_history: dict[str, list[list[dict[str, int]]]] = {}

        self.state_file = Path("tools/.chatchat_gui_state.json")

        self._build_ui()

        if initial_path is not None:
            try:
                self.load_dat(initial_path)
            except Exception as exc:
                messagebox.showerror("Ouverture impossible", str(exc))
                self.status.set("Demarrage sans fichier (ouverture initiale invalide)")
        else:
            self._prompt_reload_previous_file()

    def _build_ui(self) -> None:
        self._build_menu()

        self.btn_undo = None
        self.btn_redo = None

        toolbar = ttk.Frame(self.root, padding=6)
        toolbar.pack(fill=X)
        ttk.Checkbutton(
            toolbar,
            text="Mode edition",
            variable=self.edit_mode,
            onvalue="1",
            offvalue="0",
            command=self._on_edit_mode_changed,
        ).pack(side=LEFT, padx=(0, 6))
        ttk.Label(toolbar, textvariable=self.status).pack(side=LEFT, padx=(12, 0))

        main = ttk.PanedWindow(self.root, orient="horizontal")
        main.pack(fill=BOTH, expand=True)

        left = ttk.Frame(main, padding=6)
        main.add(left, weight=1)
        ttk.Label(left, text="Patterns").pack(anchor="w")
        self.patterns = Listbox(left, height=18, exportselection=False)
        self.patterns.pack(fill=BOTH, expand=True)
        self.patterns.bind("<<ListboxSelect>>", lambda _event: self.select_pattern())

        center = ttk.Frame(main, padding=6)
        main.add(center, weight=3)
        preview_toolbar = ttk.Frame(center)
        preview_toolbar.pack(fill=X)
        ttk.Label(preview_toolbar, text="Apercu trajectoire").pack(side=LEFT)
        ttk.Button(preview_toolbar, text="25%", width=5, command=lambda: self._set_zoom(0.25)).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="50%", width=5, command=lambda: self._set_zoom(0.50)).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="75%", width=5, command=lambda: self._set_zoom(0.75)).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="-", width=3, command=self.zoom_out).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="+", width=3, command=self.zoom_in).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="100%", width=6, command=self.zoom_reset).pack(side=RIGHT, padx=(4, 0))
        ttk.Label(preview_toolbar, textvariable=self.zoom_label).pack(side=RIGHT, padx=(8, 0))

        self.canvas = Canvas(center, width=520, height=520, background="white")
        self.canvas.pack(fill=BOTH, expand=True)
        self.canvas.bind("<Configure>", lambda _event: self.select_pattern())
        self.canvas.bind("<MouseWheel>", self.on_mouse_wheel)
        self.canvas.bind("<Button-4>", lambda _event: self.zoom_in())
        self.canvas.bind("<Button-5>", lambda _event: self.zoom_out())
        self.canvas.bind("<ButtonPress-1>", self.on_left_press)
        self.canvas.bind("<B1-Motion>", self.drag_pan)
        self.canvas.bind("<ButtonRelease-1>", self.end_pan)
        self.canvas.bind("<Control-ButtonPress-1>", self.on_ctrl_left_press)

        self.right_pane = ttk.PanedWindow(main, orient="vertical")
        main.add(self.right_pane, weight=3)

        detail_frame = ttk.Frame(self.right_pane, padding=6)
        self.right_pane.add(detail_frame, weight=2)
        ttk.Label(detail_frame, text="Details").pack(anchor="w")
        self.details = Text(detail_frame, height=16, wrap="none")
        self.details.pack(fill=BOTH, expand=True)

        self.edit_frame = ttk.Frame(self.right_pane, padding=6)
        self.right_pane.add(self.edit_frame, weight=2)
        ttk.Label(self.edit_frame, text="Edition points").grid(row=0, column=0, columnspan=6, sticky="w")

        ttk.Label(self.edit_frame, text="Index").grid(row=1, column=0, sticky="w")
        self.point_index_var = StringVar(value="")
        ttk.Entry(self.edit_frame, textvariable=self.point_index_var, width=6).grid(row=1, column=1, sticky="w")

        ttk.Label(self.edit_frame, text="x").grid(row=2, column=0, sticky="w")
        self.point_x_var = StringVar(value="500")
        ttk.Entry(self.edit_frame, textvariable=self.point_x_var, width=8).grid(row=2, column=1, sticky="w")

        ttk.Label(self.edit_frame, text="y").grid(row=2, column=2, sticky="w")
        self.point_y_var = StringVar(value="500")
        ttk.Entry(self.edit_frame, textvariable=self.point_y_var, width=8).grid(row=2, column=3, sticky="w")

        ttk.Label(self.edit_frame, text="duration_ms").grid(row=3, column=0, sticky="w")
        self.point_duration_var = StringVar(value="1000")
        ttk.Entry(self.edit_frame, textvariable=self.point_duration_var, width=8).grid(row=3, column=1, sticky="w")

        ttk.Label(self.edit_frame, text="laser 0/1").grid(row=3, column=2, sticky="w")
        self.point_laser_var = StringVar(value="1")
        ttk.Entry(self.edit_frame, textvariable=self.point_laser_var, width=8).grid(row=3, column=3, sticky="w")

        ttk.Label(self.edit_frame, text="action").grid(row=4, column=0, sticky="w")
        self.point_action_var = StringVar(value="hold")
        ttk.Combobox(self.edit_frame, textvariable=self.point_action_var, values=list(NAME_TO_ACTION.keys()), width=10).grid(row=4, column=1, sticky="w")

        ttk.Label(self.edit_frame, text="arg").grid(row=4, column=2, sticky="w")
        self.point_arg_var = StringVar(value="0")
        ttk.Entry(self.edit_frame, textvariable=self.point_arg_var, width=8).grid(row=4, column=3, sticky="w")

        ttk.Button(self.edit_frame, text="Ajouter point", command=self.add_point).grid(row=5, column=0, sticky="w", pady=(6, 0))
        ttk.Button(self.edit_frame, text="Modifier point", command=self.update_point).grid(row=5, column=1, sticky="w", pady=(6, 0))
        ttk.Button(self.edit_frame, text="Supprimer point", command=self.delete_point).grid(row=5, column=2, sticky="w", pady=(6, 0))
        ttk.Button(self.edit_frame, text="Tout selectionner", command=self.select_all_points).grid(row=5, column=3, sticky="w", pady=(6, 0))
        ttk.Button(self.edit_frame, text="Effacer selection", command=self.clear_selection).grid(row=5, column=4, sticky="w", pady=(6, 0))

        ttk.Separator(self.edit_frame, orient="horizontal").grid(row=6, column=0, columnspan=6, sticky="ew", pady=6)
        ttk.Label(self.edit_frame, text="Move selection dx").grid(row=7, column=0, sticky="w")
        self.move_dx_var = StringVar(value="0")
        ttk.Entry(self.edit_frame, textvariable=self.move_dx_var, width=8).grid(row=7, column=1, sticky="w")
        ttk.Label(self.edit_frame, text="dy").grid(row=7, column=2, sticky="w")
        self.move_dy_var = StringVar(value="0")
        ttk.Entry(self.edit_frame, textvariable=self.move_dy_var, width=8).grid(row=7, column=3, sticky="w")
        ttk.Button(self.edit_frame, text="Appliquer move", command=self.move_shape).grid(row=7, column=4, sticky="w")

        ttk.Label(self.edit_frame, text="Scale selection").grid(row=8, column=0, sticky="w")
        self.scale_var = StringVar(value="1.0")
        ttk.Entry(self.edit_frame, textvariable=self.scale_var, width=8).grid(row=8, column=1, sticky="w")
        ttk.Button(self.edit_frame, text="Appliquer scale", command=self.scale_shape).grid(row=8, column=4, sticky="w")

        inspect_frame = ttk.Frame(self.right_pane, padding=6)
        self.right_pane.add(inspect_frame, weight=1)
        ttk.Label(inspect_frame, text="Inspection").pack(anchor="w")
        self.inspect_text = Text(inspect_frame, height=8, wrap="word")
        self.inspect_text.pack(fill=BOTH, expand=True)

        self._update_edit_pane_visibility()
        self._update_undo_redo_state()

    def _build_menu(self) -> None:
        menubar = Menu(self.root)

        file_menu = Menu(menubar, tearoff=0)
        file_menu.add_command(label="Ouvrir DAT...", command=self.open_dat)
        file_menu.add_command(label="Ouvrir JSON...", command=self.open_json)
        file_menu.add_separator()
        file_menu.add_command(label="Enregistrer DAT", command=self.save_dat)
        file_menu.add_command(label="Enregistrer DAT sous...", command=self.save_dat_as)
        file_menu.add_command(label="Enregistrer JSON", command=self.save_json)
        file_menu.add_separator()
        file_menu.add_command(label="Quitter", command=self.root.destroy)
        menubar.add_cascade(label="Fichier", menu=file_menu)

        edit_menu = Menu(menubar, tearoff=0)
        edit_menu.add_command(label="Arriere", command=self.undo_last)
        edit_menu.add_command(label="Avant", command=self.redo_last)
        edit_menu.add_command(label="Regler historique annulation...", command=self.set_undo_depth)
        menubar.add_cascade(label="Edition", menu=edit_menu)

        tools_menu = Menu(menubar, tearoff=0)
        tools_menu.add_command(label="Exporter DAT", command=self.export_dat)
        tools_menu.add_command(label="Importer DAT -> JSON", command=self.import_dat_to_json)
        tools_menu.add_separator()
        tools_menu.add_command(label="Inspecter", command=self.inspect_dat)
        menubar.add_cascade(label="Outils", menu=tools_menu)

        self.root.config(menu=menubar)

    def _on_edit_mode_changed(self) -> None:
        self._update_edit_pane_visibility()
        self._update_undo_redo_state()
        self.select_pattern()

    def _update_edit_pane_visibility(self) -> None:
        if self.edit_mode.get() == "1":
            try:
                self.right_pane.add(self.edit_frame, weight=2)
            except Exception:
                pass
        else:
            try:
                self.right_pane.forget(self.edit_frame)
            except Exception:
                pass

    def _update_undo_redo_state(self) -> None:
        state = "normal" if self.edit_mode.get() == "1" else "disabled"
        if self.btn_undo is not None:
            self.btn_undo.configure(state=state)
        if self.btn_redo is not None:
            self.btn_redo.configure(state=state)

    def _prompt_reload_previous_file(self) -> None:
        if not self.state_file.exists():
            return
        try:
            state = json.loads(self.state_file.read_text(encoding="utf-8"))
            last_path = Path(state.get("last_path", state.get("last_dat_path", "")))
        except Exception:
            return

        if not str(last_path) or not last_path.exists() or last_path.suffix.lower() not in (".dat", ".json"):
            return

        if messagebox.askyesno("Recharger", f"Recharger le dernier fichier ouvert ?\n{last_path}"):
            # Robust startup: keep app empty if stale/invalid file.
            try:
                if last_path.suffix.lower() == ".json":
                    self.load_json(last_path)
                else:
                    self.load_dat(last_path)
            except Exception:
                pass

    def _save_last_file(self, path: Path) -> None:
        try:
            self.state_file.parent.mkdir(parents=True, exist_ok=True)
            self.state_file.write_text(json.dumps({"last_path": str(path)}), encoding="utf-8")
        except Exception:
            pass

    def open_dat(self) -> None:
        filename = filedialog.askopenfilename(
            title="Ouvrir patterns.dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
        )
        if filename:
            self.load_dat(Path(filename))

    def open_json(self) -> None:
        filename = filedialog.askopenfilename(
            title="Ouvrir patterns.json",
            filetypes=(("Patterns JSON", "*.json"), ("Tous les fichiers", "*.*")),
        )
        if filename:
            self.load_json(Path(filename))

    def load_dat(self, path: Path) -> None:
        inspected = self.service.load_dat(path)
        self.path = path
        self.json_path = None
        self.source_json_data = binary_pack_to_json_data(inspected.pack, source_name=path.stem)
        self.pack = inspected.pack
        self.edited_points = {}
        self.history = {}
        self.redo_history = {}
        self.selected_point_index = None
        self.selected_indices.clear()
        self._set_inspect_text(inspected.summary)
        self._reload_pattern_list()
        self.status.set(f"DAT charge: {path}")
        self._save_last_file(path)

    def load_json(self, path: Path) -> None:
        inspected = self.service.load_json(path)
        self.path = None
        self.json_path = path
        self.source_json_data = json.loads(path.read_text(encoding="utf-8"))
        self.pack = inspected.pack
        self.edited_points = {}
        self.history = {}
        self.redo_history = {}
        self.selected_point_index = None
        self.selected_indices.clear()
        self._set_inspect_text(inspected.summary)
        self._reload_pattern_list()
        self.status.set(f"JSON charge: {path}")
        self._save_last_file(path)

    def inspect_dat(self) -> None:
        if self.path is None and self.json_path is None:
            self.open_dat()
            return
        try:
            if self.path is not None:
                self.load_dat(self.path)
            elif self.json_path is not None:
                self.load_json(self.json_path)
        except Exception as exc:
            messagebox.showerror("Ouverture impossible", str(exc))

    def _sync_json_from_edited_points(self) -> None:
        if self.pack is None or self.source_json_data is None:
            return
        patterns = self.source_json_data.get("patterns", [])
        by_id = {p.pattern_id: p for p in self.pack.patterns}
        for row in patterns:
            pid = str(row.get("id", ""))
            if pid not in by_id:
                continue
            p = by_id[pid]
            src = self.edited_points.get(
                p.pattern_id,
                [{"x": pp.x, "y": pp.y, "duration_ms": pp.duration_ms, "laser": pp.laser, "action": pp.action, "arg": pp.arg} for pp in p.points],
            )
            steps = []
            for sp in src:
                action_name = ACTION_TO_NAME.get(int(sp["action"]), "move")
                step = {
                    "type": action_name,
                    "laser": bool(int(sp["laser"])),
                    "duration_ms": int(sp["duration_ms"]),
                }
                if action_name in ("hold", "move", "jitter", "off_move"):
                    step["x"] = round((int(sp["x"]) - 500) / 500.0, 4)
                    step["y"] = round((int(sp["y"]) - 500) / 500.0, 4)
                if action_name == "jitter":
                    step["amplitude"] = round(int(sp["arg"]) / 1000.0, 4)
                steps.append(step)
            row["steps"] = steps

    def save_json(self) -> None:
        if self.source_json_data is None:
            messagebox.showerror("JSON", "Aucun JSON source charge.")
            return
        self._sync_json_from_edited_points()
        if self.json_path is None:
            filename = filedialog.asksaveasfilename(
                title="Enregistrer patterns.json",
                defaultextension=".json",
                filetypes=(("Patterns JSON", "*.json"), ("Tous les fichiers", "*.*")),
                initialfile="patterns.json",
            )
            if not filename:
                return
            self.json_path = Path(filename)
        try:
            self.service.save_json(self.json_path, self.source_json_data)
            self.status.set(f"JSON enregistre: {self.json_path}")
            self._save_last_file(self.json_path)
        except Exception as exc:
            messagebox.showerror("JSON", str(exc))

    def export_dat(self) -> None:
        if self.source_json_data is None:
            messagebox.showerror("Export DAT", "Charge un JSON source d'abord.")
            return
        self._sync_json_from_edited_points()
        filename = filedialog.asksaveasfilename(
            title="Exporter patterns.dat",
            defaultextension=".dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
            initialfile="patterns.dat",
        )
        if not filename:
            return
        try:
            patterns = json_data_to_binary_patterns(self.source_json_data)
            pack = write_dat_file(Path(filename), patterns)
            self.path = Path(filename)
            self.pack = pack
            inspected = self.service.load_dat(self.path)
            self._set_inspect_text(inspected.summary)
            self._reload_pattern_list()
            self.status.set(f"DAT exporte: {self.path}")
            self._save_last_file(self.path)
        except Exception as exc:
            messagebox.showerror("Export DAT", str(exc))

    def import_dat_to_json(self) -> None:
        filename = filedialog.askopenfilename(
            title="Importer patterns.dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
        )
        if not filename:
            return
        out = filedialog.asksaveasfilename(
            title="Enregistrer JSON importe",
            defaultextension=".json",
            filetypes=(("Patterns JSON", "*.json"), ("Tous les fichiers", "*.*")),
            initialfile=f"{Path(filename).stem}.json",
        )
        if not out:
            return
        try:
            data = self.service.dat_to_json_data(filename)
            self.service.save_json(out, data)
            self.load_json(Path(out))
            self.status.set(f"DAT importe vers JSON: {out}")
        except Exception as exc:
            messagebox.showerror("Import DAT", str(exc))

    def _reload_pattern_list(self) -> None:
        self.patterns.delete(0, END)
        if self.pack is None:
            return
        for pattern in self.pack.patterns:
            self.patterns.insert(END, f"{pattern.pattern_id} ({len(pattern.points)} pts, {pattern.duration_total_ms} ms)")
        if self.pack.patterns:
            self.patterns.selection_set(0)
            self.select_pattern()

    def selected_pattern(self) -> BinaryPattern | None:
        if self.pack is None:
            return None
        selection = self.patterns.curselection()
        if not selection:
            return None
        idx = int(selection[0])
        if idx >= len(self.pack.patterns):
            return None
        return self.pack.patterns[idx]

    def _pattern_points(self, pattern: BinaryPattern) -> list[dict[str, int]]:
        if pattern.pattern_id not in self.edited_points:
            self.edited_points[pattern.pattern_id] = [
                {"x": p.x, "y": p.y, "duration_ms": p.duration_ms, "laser": p.laser, "action": p.action, "arg": p.arg}
                for p in pattern.points
            ]
        return self.edited_points[pattern.pattern_id]

    def select_pattern(self) -> None:
        pattern = self.selected_pattern()
        if pattern is None:
            return
        points = self._pattern_points(pattern)
        self._show_details(pattern, points)
        self._draw_pattern(points)

    def zoom_in(self) -> None:
        self._set_zoom(self.zoom * 1.25)

    def zoom_out(self) -> None:
        self._set_zoom(self.zoom / 1.25)

    def zoom_reset(self) -> None:
        self.pan_x = 0.0
        self.pan_y = 0.0
        self._set_zoom(1.0)

    def on_mouse_wheel(self, event) -> None:
        if getattr(event, "delta", 0) > 0:
            self.zoom_in()
        else:
            self.zoom_out()

    def _set_zoom(self, value: float) -> None:
        self.zoom = min(max(value, 0.25), 5.0)
        self._clamp_pan()
        self.zoom_label.set(f"{int(self.zoom * 100)}%")
        self.select_pattern()

    def on_left_press(self, event) -> None:
        if self.edit_mode.get() == "1":
            pattern, points = self._current_pattern_and_points()
            if pattern is None or points is None:
                return
            hit = self._nearest_point_index(event.x, event.y, points, max_dist2=250)
            if hit is not None:
                if hit not in self.selected_indices:
                    self.selected_indices = {hit}
                self.selected_point_index = hit
                self._fill_form_from_point(points[hit], hit)
                self._push_history(pattern.pattern_id, points)
                original = {i: (points[i]["x"], points[i]["y"]) for i in self.selected_indices}
                self.point_drag_start = (event.x, event.y, original)
                self.select_pattern()
                return
            self.selection_box_start = (event.x, event.y)
            if self.selection_box_item is not None:
                self.canvas.delete(self.selection_box_item)
            self.selection_box_item = self.canvas.create_rectangle(event.x, event.y, event.x, event.y, outline="#2a6fbb", dash=(4, 3))
            return

        if self.zoom <= 1.0:
            self.drag_start = None
            return
        self.drag_start = (event.x, event.y, self.pan_x, self.pan_y)
        self.canvas.configure(cursor="fleur")

    def on_ctrl_left_press(self, event) -> None:
        if self.edit_mode.get() != "1":
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None:
            return
        hit = self._nearest_point_index(event.x, event.y, points, max_dist2=250)
        if hit is None:
            return
        if hit in self.selected_indices:
            self.selected_indices.remove(hit)
        else:
            self.selected_indices.add(hit)
        self.selected_point_index = hit
        self._fill_form_from_point(points[hit], hit)
        self.select_pattern()

    def drag_pan(self, event) -> None:
        if self.edit_mode.get() == "1":
            pattern, points = self._current_pattern_and_points()
            if pattern is None or points is None:
                return
            if self.point_drag_start is not None:
                start_x, start_y, original = self.point_drag_start
                sx, sy = self._canvas_to_coord(start_x, start_y)
                cx, cy = self._canvas_to_coord(event.x, event.y)
                dx = cx - sx
                dy = cy - sy
                for idx in self.selected_indices:
                    ox, oy = original[idx]
                    points[idx]["x"] = max(0, min(1000, ox + dx))
                    points[idx]["y"] = max(0, min(1000, oy + dy))
                self.select_pattern()
                return
            if self.selection_box_start is not None and self.selection_box_item is not None:
                x0, y0 = self.selection_box_start
                self.canvas.coords(self.selection_box_item, x0, y0, event.x, event.y)
                return

        if self.drag_start is None:
            return
        start_x, start_y, start_pan_x, start_pan_y = self.drag_start
        left, top, right, bottom = self._plot_bounds()
        plot_w = max(right - left, 1)
        plot_h = max(bottom - top, 1)
        visible_span = 1000.0 / self.zoom
        dx = event.x - start_x
        dy = event.y - start_y
        self.pan_x = start_pan_x - (dx / plot_w) * visible_span
        self.pan_y = start_pan_y + (dy / plot_h) * visible_span
        self._clamp_pan()
        self.select_pattern()

    def end_pan(self, _event) -> None:
        if self.edit_mode.get() == "1":
            pattern, points = self._current_pattern_and_points()
            if pattern is not None and points is not None:
                if self.point_drag_start is not None:
                    self.point_drag_start = None
                    self.select_pattern()
                    return
                if self.selection_box_start is not None:
                    self._finalize_box_selection(points)
                    self.selection_box_start = None
                    if self.selection_box_item is not None:
                        self.canvas.delete(self.selection_box_item)
                        self.selection_box_item = None
                    self.select_pattern()
                    return
        self.drag_start = None
        self.canvas.configure(cursor="")

    def _clamp_pan(self) -> None:
        if self.zoom <= 1.0:
            self.pan_x = 0.0
            self.pan_y = 0.0
            return
        half_window = 500.0 / self.zoom
        max_center = 500.0 - half_window
        self.pan_x = min(max(self.pan_x, -max_center), max_center)
        self.pan_y = min(max(self.pan_y, -max_center), max_center)

    def _plot_bounds(self) -> tuple[int, int, int, int]:
        width = max(self.canvas.winfo_width(), 260)
        height = max(self.canvas.winfo_height(), 260)
        return 52, 70, width - 26, height - 42

    def _draw_pattern(self, points: list[dict[str, int]]) -> None:
        self.canvas.delete("all")
        left, top, right, bottom = self._plot_bounds()
        plot_w = max(right - left, 1)
        plot_h = max(bottom - top, 1)
        self.canvas.create_rectangle(left, top, right, bottom, outline="#b9c0c8", width=1)
        self.canvas.create_text(left, 16, anchor="w", text="Legende: point plein=laser ON, point vide=laser OFF, cercle orange=jitter, numero=index point", fill="#444444", font=("TkDefaultFont", 8))
        self.canvas.create_text(left, 34, anchor="w", text=f"Zoom {int(self.zoom * 100)}% - molette zoom, clic gauche glisser deplace", fill="#666666", font=("TkDefaultFont", 8))

        visible_span = 1000.0 / self.zoom
        vx0 = 500.0 + self.pan_x - (visible_span / 2.0)
        vx1 = 500.0 + self.pan_x + (visible_span / 2.0)
        vy0 = 500.0 + self.pan_y - (visible_span / 2.0)
        vy1 = 500.0 + self.pan_y + (visible_span / 2.0)

        def p(x: int, y: int) -> tuple[float, float]:
            return left + ((x - vx0) / visible_span) * plot_w, top + ((y - vy0) / visible_span) * plot_h

        self._draw_axes(left, top, right, bottom, vx0, vx1, vy0, vy1)

        prev = None
        for idx, point in enumerate(points, start=1):
            cur = p(point["x"], point["y"])
            action = point["action"]
            laser_on = point["laser"] == 1 and action not in {LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE}
            color = "#d7263d" if laser_on else "#2a6fbb"
            if prev is not None and action in {LPTN_ACTION_MOVE, LPTN_ACTION_OFF_MOVE}:
                self.canvas.create_line(prev[0], prev[1], cur[0], cur[1], fill=color, width=2, dash=None if laser_on else (7, 5))
            self.canvas.create_oval(cur[0] - 4, cur[1] - 4, cur[0] + 4, cur[1] + 4, fill=color if laser_on else "white", outline=color, width=2 if not laser_on else 1)
            if action == LPTN_ACTION_JITTER:
                r = (point["arg"] / 1000.0) * min(right - left, bottom - top)
                self.canvas.create_oval(cur[0] - r, cur[1] - r, cur[0] + r, cur[1] + r, outline="#f28c28", dash=(2, 3))
            lbl = "#0f5" if (idx - 1) in self.selected_indices else "#111"
            self.canvas.create_text(cur[0] + 9, cur[1] - 9, text=str(idx), fill=lbl, font=("TkDefaultFont", 7))
            prev = cur

    def _draw_axes(self, left: int, top: int, right: int, bottom: int, vx0: float, vx1: float, vy0: float, vy1: float) -> None:
        for raw in (0, 250, 500, 750, 1000):
            color = "#d1d6dd" if raw == 500 else "#edf0f3"
            width = 2 if raw == 500 else 1
            if vx0 <= raw <= vx1:
                x = left + ((raw - vx0) / (vx1 - vx0)) * (right - left)
                self.canvas.create_line(x, top, x, bottom, fill=color, width=width)
                self.canvas.create_text(x, bottom + 14, text=str(int(round((raw - 500) * 2))), fill="#69717a", font=("TkDefaultFont", 8))
            if vy0 <= raw <= vy1:
                y = top + ((raw - vy0) / (vy1 - vy0)) * (bottom - top)
                self.canvas.create_line(left, y, right, y, fill=color, width=width)
                self.canvas.create_text(left - 26, y, text=str(int(round((raw - 500) * 2))), fill="#69717a", font=("TkDefaultFont", 8))
        self.canvas.create_text((left + right) / 2, bottom + 30, text="Axe X (-1000 -> +1000)", fill="#69717a", font=("TkDefaultFont", 8))
        self.canvas.create_text(left - 36, (top + bottom) / 2, text="Axe Y (-1000..+1000)", fill="#69717a", font=("TkDefaultFont", 8))

    def _show_details(self, pattern: BinaryPattern, points: list[dict[str, int]]) -> None:
        duration = sum(p["duration_ms"] for p in points)
        xs = [p["x"] for p in points] if points else [0]
        ys = [p["y"] for p in points] if points else [0]
        lines = [
            f"id: {pattern.pattern_id}",
            f"weight: {pattern.weight}",
            f"flags: 0x{pattern.flags:02x}",
            f"duree: {duration} ms",
            f"points: {len(points)}",
            f"bbox: x={min(xs)}..{max(xs)} y={min(ys)}..{max(ys)}",
            "",
            "liste des points:",
        ]
        for i, point in enumerate(points, start=1):
            marker = "*" if (i - 1) in self.selected_indices else " "
            lines.append(
                f"{marker}{i:02d}. x={point['x']:4d} y={point['y']:4d} duration_ms={point['duration_ms']:5d} "
                f"laser={point['laser']} action={ACTION_TO_NAME.get(point['action'], 'unknown')} arg={point['arg']}"
            )
        self.details.delete("1.0", END)
        self.details.insert("1.0", "\n".join(lines))

    def _set_inspect_text(self, text: str) -> None:
        self.inspect_text.delete("1.0", END)
        self.inspect_text.insert("1.0", text)

    def _coord_to_canvas(self, x: int, y: int) -> tuple[float, float]:
        left, top, right, bottom = self._plot_bounds()
        plot_w = max(right - left, 1)
        plot_h = max(bottom - top, 1)
        span = 1000.0 / self.zoom
        vx0 = 500.0 + self.pan_x - (span / 2.0)
        vy0 = 500.0 + self.pan_y - (span / 2.0)
        return left + ((x - vx0) / span) * plot_w, top + ((y - vy0) / span) * plot_h

    def _canvas_to_coord(self, x: int, y: int) -> tuple[int, int]:
        left, top, right, bottom = self._plot_bounds()
        plot_w = max(right - left, 1)
        plot_h = max(bottom - top, 1)
        span = 1000.0 / self.zoom
        vx0 = 500.0 + self.pan_x - (span / 2.0)
        vy0 = 500.0 + self.pan_y - (span / 2.0)
        cx = int(round(vx0 + ((x - left) / plot_w) * span))
        cy = int(round(vy0 + ((y - top) / plot_h) * span))
        return max(0, min(1000, cx)), max(0, min(1000, cy))

    def _nearest_point_index(self, x: int, y: int, points: list[dict[str, int]], max_dist2: int) -> int | None:
        best_idx = None
        best_dist2 = None
        for idx, p in enumerate(points):
            cx, cy = self._coord_to_canvas(p["x"], p["y"])
            d2 = (cx - x) ** 2 + (cy - y) ** 2
            if best_dist2 is None or d2 < best_dist2:
                best_dist2 = d2
                best_idx = idx
        if best_idx is None or best_dist2 is None or best_dist2 > max_dist2:
            return None
        return best_idx

    def _fill_form_from_point(self, point: dict[str, int], index: int) -> None:
        self.point_index_var.set(str(index + 1))
        self.point_x_var.set(str(point["x"]))
        self.point_y_var.set(str(point["y"]))
        self.point_duration_var.set(str(point["duration_ms"]))
        self.point_laser_var.set(str(point["laser"]))
        self.point_action_var.set(ACTION_TO_NAME.get(point["action"], "hold"))
        self.point_arg_var.set(str(point["arg"]))

    def _finalize_box_selection(self, points: list[dict[str, int]]) -> None:
        if self.selection_box_item is None:
            return
        x0, y0, x1, y1 = self.canvas.coords(self.selection_box_item)
        min_x, max_x = min(x0, x1), max(x0, x1)
        min_y, max_y = min(y0, y1), max(y0, y1)
        chosen: set[int] = set()
        for idx, p in enumerate(points):
            cx, cy = self._coord_to_canvas(p["x"], p["y"])
            if min_x <= cx <= max_x and min_y <= cy <= max_y:
                chosen.add(idx)
        self.selected_indices = chosen
        if chosen:
            self.selected_point_index = sorted(chosen)[0]
            self._fill_form_from_point(points[self.selected_point_index], self.selected_point_index)

    def _require_edit_mode(self) -> bool:
        if self.edit_mode.get() != "1":
            messagebox.showerror("Edition", "Activer 'Mode edition' pour modifier les points.")
            return False
        return True

    def _current_pattern_and_points(self) -> tuple[BinaryPattern | None, list[dict[str, int]] | None]:
        pattern = self.selected_pattern()
        if pattern is None:
            return None, None
        return pattern, self._pattern_points(pattern)

    def _target_indices(self, points: list[dict[str, int]]) -> list[int]:
        if self.selected_indices:
            return sorted(i for i in self.selected_indices if 0 <= i < len(points))
        return list(range(len(points)))

    @staticmethod
    def _max_pair_distance(points: list[tuple[int, int]]) -> float:
        if len(points) < 2:
            return 0.0
        max_d2 = 0
        for i in range(len(points)):
            x1, y1 = points[i]
            for j in range(i + 1, len(points)):
                x2, y2 = points[j]
                dx = x2 - x1
                dy = y2 - y1
                d2 = dx * dx + dy * dy
                if d2 > max_d2:
                    max_d2 = d2
        return max_d2 ** 0.5

    def _push_history(self, pattern_id: str, points: list[dict[str, int]]) -> None:
        stack = self.history.setdefault(pattern_id, [])
        stack.append(copy.deepcopy(points))
        if len(stack) > self.undo_max:
            del stack[0]
        # new edit invalidates redo chain
        self.redo_history[pattern_id] = []

    def undo_last(self) -> None:
        pattern = self.selected_pattern()
        if pattern is None:
            return
        stack = self.history.get(pattern.pattern_id, [])
        if not stack:
            self.status.set("Annulation: rien a annuler")
            return
        current = copy.deepcopy(self.edited_points.get(pattern.pattern_id, []))
        redo_stack = self.redo_history.setdefault(pattern.pattern_id, [])
        redo_stack.append(current)
        if len(redo_stack) > self.undo_max:
            del redo_stack[0]
        self.edited_points[pattern.pattern_id] = stack.pop()
        self.select_pattern()
        self.status.set("Annulation appliquee")

    def redo_last(self) -> None:
        pattern = self.selected_pattern()
        if pattern is None:
            return
        redo_stack = self.redo_history.get(pattern.pattern_id, [])
        if not redo_stack:
            self.status.set("Redo: rien a refaire")
            return
        current = copy.deepcopy(self.edited_points.get(pattern.pattern_id, []))
        undo_stack = self.history.setdefault(pattern.pattern_id, [])
        undo_stack.append(current)
        if len(undo_stack) > self.undo_max:
            del undo_stack[0]
        self.edited_points[pattern.pattern_id] = redo_stack.pop()
        self.select_pattern()
        self.status.set("Redo applique")

    def set_undo_depth(self) -> None:
        value = simpledialog.askinteger("Historique", "Nombre d'annulations max:", initialvalue=self.undo_max, minvalue=1, maxvalue=100)
        if value is None:
            return
        self.undo_max = int(value)
        for pid, stack in self.history.items():
            if len(stack) > self.undo_max:
                self.history[pid] = stack[-self.undo_max :]
        for pid, stack in self.redo_history.items():
            if len(stack) > self.undo_max:
                self.redo_history[pid] = stack[-self.undo_max :]
        self.status.set(f"Historique annulation = {self.undo_max}")

    def _read_point_form(self) -> dict[str, int]:
        action_name = self.point_action_var.get().strip().lower()
        if action_name not in NAME_TO_ACTION:
            raise ValueError("action invalide")
        x = max(0, min(1000, int(self.point_x_var.get())))
        y = max(0, min(1000, int(self.point_y_var.get())))
        duration_ms = max(80, min(10000, int(self.point_duration_var.get())))
        laser = 1 if int(self.point_laser_var.get()) else 0
        action = NAME_TO_ACTION[action_name]
        arg = int(self.point_arg_var.get())
        if action == LPTN_ACTION_JITTER:
            arg = max(0, min(1000, arg))
        else:
            arg = 0
        if action in {LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE}:
            laser = 0
        return {"x": x, "y": y, "duration_ms": duration_ms, "laser": laser, "action": action, "arg": arg}

    def add_point(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None:
            return
        try:
            new_point = self._read_point_form()
        except Exception as exc:
            messagebox.showerror("Edition", str(exc))
            return
        self._push_history(pattern.pattern_id, points)
        insert_at = len(points)
        if self.point_index_var.get().strip():
            insert_at = max(0, min(len(points), int(self.point_index_var.get()) - 1))
        points.insert(insert_at, new_point)
        self.selected_point_index = insert_at
        self.selected_indices = {insert_at}
        self.select_pattern()

    def update_point(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None:
            return
        idx = int(self.point_index_var.get()) - 1 if self.point_index_var.get().strip() else self.selected_point_index
        if idx is None or idx < 0 or idx >= len(points):
            messagebox.showerror("Edition", "Index point invalide")
            return
        try:
            new_point = self._read_point_form()
        except Exception as exc:
            messagebox.showerror("Edition", str(exc))
            return
        self._push_history(pattern.pattern_id, points)
        points[idx] = new_point
        self.selected_point_index = idx
        self.selected_indices = {idx}
        self.select_pattern()

    def delete_point(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None or not points:
            return
        idx = int(self.point_index_var.get()) - 1 if self.point_index_var.get().strip() else self.selected_point_index
        if idx is None or idx < 0 or idx >= len(points):
            messagebox.showerror("Edition", "Index point invalide")
            return
        self._push_history(pattern.pattern_id, points)
        points.pop(idx)
        self.selected_point_index = min(idx, len(points) - 1) if points else None
        self.selected_indices = {self.selected_point_index} if self.selected_point_index is not None else set()
        self.select_pattern()

    def select_all_points(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None:
            return
        self.selected_indices = set(range(len(points)))
        if points:
            self.selected_point_index = 0
            self._fill_form_from_point(points[0], 0)
        self.select_pattern()

    def clear_selection(self) -> None:
        self.selected_indices.clear()
        self.selected_point_index = None
        self.select_pattern()

    def move_shape(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None:
            return
        try:
            dx = int(self.move_dx_var.get())
            dy = int(self.move_dy_var.get())
        except Exception:
            messagebox.showerror("Edition", "dx/dy invalides")
            return
        self._push_history(pattern.pattern_id, points)
        for i in self._target_indices(points):
            points[i]["x"] = max(0, min(1000, points[i]["x"] + dx))
            points[i]["y"] = max(0, min(1000, points[i]["y"] + dy))
        self.select_pattern()

    def scale_shape(self) -> None:
        if not self._require_edit_mode():
            return
        pattern, points = self._current_pattern_and_points()
        if pattern is None or points is None or not points:
            return
        try:
            factor = float(self.scale_var.get())
        except Exception:
            messagebox.showerror("Edition", "facteur de scale invalide")
            return
        if factor <= 0:
            messagebox.showerror("Edition", "facteur de scale > 0 requis")
            return
        target = self._target_indices(points)
        if not target:
            return

        cx = sum(points[i]["x"] for i in target) / len(target)
        cy = sum(points[i]["y"] for i in target) / len(target)

        proposed: dict[int, tuple[int, int]] = {}
        for i in target:
            nx = int(round(cx + (points[i]["x"] - cx) * factor))
            ny = int(round(cy + (points[i]["y"] - cy) * factor))
            proposed[i] = (max(0, min(1000, nx)), max(0, min(1000, ny)))

        max_dist = self._max_pair_distance(list(proposed.values()))
        if max_dist > 950.0:
            messagebox.showerror(
                "Edition",
                (
                    f"Scale refuse: distance max entre points = {max_dist:.1f} (> 950).\n"
                    "Forme consideree hors champ (marge de securite 100)."
                ),
            )
            return

        self._push_history(pattern.pattern_id, points)
        for i, (nx, ny) in proposed.items():
            points[i]["x"] = nx
            points[i]["y"] = ny
        self.select_pattern()

    def _build_patterns_for_save(self) -> list[BinaryPattern]:
        if self.pack is None:
            raise ValueError("Aucun DAT charge")
        out: list[BinaryPattern] = []
        for p in self.pack.patterns:
            src = self.edited_points.get(
                p.pattern_id,
                [{"x": pp.x, "y": pp.y, "duration_ms": pp.duration_ms, "laser": pp.laser, "action": pp.action, "arg": pp.arg} for pp in p.points],
            )
            pts = [
                BinaryPoint(x=int(sp["x"]), y=int(sp["y"]), duration_ms=int(sp["duration_ms"]), laser=int(sp["laser"]), action=int(sp["action"]), arg=int(sp["arg"]))
                for sp in src
            ]
            out.append(
                BinaryPattern(
                    pattern_id=p.pattern_id,
                    weight=p.weight,
                    flags=p.flags,
                    duration_total_ms=0,
                    point_offset=0,
                    pattern_crc32=0,
                    x_min=0,
                    x_max=0,
                    y_min=0,
                    y_max=0,
                    points=pts,
                )
            )
        return out

    def save_dat(self) -> None:
        if self.path is None:
            self.save_dat_as()
            return
        try:
            patterns = self._build_patterns_for_save()
            write_dat_file(self.path, patterns, flags=self.pack.flags if self.pack is not None else None)
            self.status.set(f"DAT enregistre: {self.path}")
            self.load_dat(self.path)
        except Exception as exc:
            messagebox.showerror("Enregistrement impossible", str(exc))

    def save_dat_as(self) -> None:
        filename = filedialog.asksaveasfilename(
            title="Enregistrer patterns.dat",
            defaultextension=".dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
            initialfile="patterns.dat",
        )
        if not filename:
            return
        self.path = Path(filename)
        self.save_dat()


def run_gui(initial_path: str | Path | None = None) -> int:
    try:
        root = Tk()
    except Exception as exc:
        print(f"ERROR impossible de demarrer la GUI: {exc}")
        return 1

    path = Path(initial_path) if initial_path else None
    PatternGui(root, path)
    root.geometry("1320x820")
    root.mainloop()
    return 0
