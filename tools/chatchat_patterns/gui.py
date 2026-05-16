from __future__ import annotations

import json
from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, X, filedialog, messagebox
from tkinter import Tk, Canvas, Listbox, StringVar, Text
from tkinter import ttk

from .binary import export_dat_file, read_dat_file, validate_binary_export_file
from .geometry import pattern_duration_ms, pattern_jitter_zones, pattern_pauses, pattern_segments
from .model import Pattern, PatternPack, pattern_pack_from_data, read_json_file
from .validator import ValidationResult, validate_pack_data


class PatternGui:
    def __init__(self, root: Tk, initial_path: Path | None = None) -> None:
        self.root = root
        self.root.title("CatChat Patterns")
        self.path: Path | None = None
        self.data: dict | None = None
        self.pack: PatternPack | None = None
        self.validation = ValidationResult()
        self.status = StringVar(value="Aucun fichier charge")
        self.zoom = 1.0
        self.pan_x = 0.0
        self.pan_y = 0.0
        self.drag_start: tuple[int, int, float, float] | None = None
        self.zoom_label = StringVar(value="100%")

        self._build_ui()

        if initial_path is not None:
            self.load_file(initial_path)

    def _build_ui(self) -> None:
        toolbar = ttk.Frame(self.root, padding=6)
        toolbar.pack(fill=X)

        ttk.Button(toolbar, text="Ouvrir", command=self.open_file).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Valider", command=self.validate_editor).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Sauver", command=self.save_file).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Sauver sous", command=self.save_file_as).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Export DAT", command=self.export_dat).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Inspect DAT", command=self.inspect_dat).pack(side=LEFT, padx=(0, 6))
        ttk.Label(toolbar, textvariable=self.status).pack(side=LEFT, padx=(12, 0))

        main = ttk.PanedWindow(self.root, orient="horizontal")
        main.pack(fill=BOTH, expand=True)

        left = ttk.Frame(main, padding=6)
        main.add(left, weight=1)
        ttk.Label(left, text="Patterns").pack(anchor="w")
        self.patterns = Listbox(left, height=18, exportselection=False)
        self.patterns.pack(fill=BOTH, expand=True)
        self.patterns.bind("<<ListboxSelect>>", lambda _event: self.select_pattern())

        center = ttk.PanedWindow(main, orient="vertical")
        main.add(center, weight=3)

        preview_frame = ttk.Frame(center, padding=6)
        center.add(preview_frame, weight=3)
        preview_toolbar = ttk.Frame(preview_frame)
        preview_toolbar.pack(fill=X)
        ttk.Label(preview_toolbar, text="Apercu trajectoire").pack(side=LEFT)
        ttk.Button(preview_toolbar, text="-", width=3, command=self.zoom_out).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="+", width=3, command=self.zoom_in).pack(side=RIGHT, padx=(4, 0))
        ttk.Button(preview_toolbar, text="100%", width=6, command=self.zoom_reset).pack(side=RIGHT, padx=(4, 0))
        ttk.Label(preview_toolbar, textvariable=self.zoom_label).pack(side=RIGHT, padx=(8, 0))
        self.canvas = Canvas(preview_frame, width=520, height=520, background="white")
        self.canvas.pack(fill=BOTH, expand=True)
        self.canvas.bind("<Configure>", lambda _event: self.redraw_selected_pattern())
        self.canvas.bind("<MouseWheel>", self.on_mouse_wheel)
        self.canvas.bind("<Button-4>", lambda _event: self.zoom_in())
        self.canvas.bind("<Button-5>", lambda _event: self.zoom_out())
        self.canvas.bind("<ButtonPress-1>", self.start_pan)
        self.canvas.bind("<B1-Motion>", self.drag_pan)
        self.canvas.bind("<ButtonRelease-1>", self.end_pan)

        detail_frame = ttk.Frame(center, padding=6)
        center.add(detail_frame, weight=2)
        ttk.Label(detail_frame, text="Details").pack(anchor="w")
        self.details = Text(detail_frame, height=10, wrap="none")
        self.details.pack(fill=BOTH, expand=True)

        right = ttk.PanedWindow(main, orient="vertical")
        main.add(right, weight=3)

        editor_frame = ttk.Frame(right, padding=6)
        right.add(editor_frame, weight=3)
        ttk.Label(editor_frame, text="JSON").pack(anchor="w")
        self.editor = Text(editor_frame, wrap="none", undo=True)
        self.editor.pack(fill=BOTH, expand=True)

        validation_frame = ttk.Frame(right, padding=6)
        right.add(validation_frame, weight=1)
        ttk.Label(validation_frame, text="Validation").pack(anchor="w")
        self.validation_text = Text(validation_frame, height=8, wrap="word")
        self.validation_text.pack(fill=BOTH, expand=True)

    def open_file(self) -> None:
        filename = filedialog.askopenfilename(
            title="Ouvrir un pack JSON",
            filetypes=(("JSON", "*.json"), ("Tous les fichiers", "*.*")),
        )
        if filename:
            self.load_file(Path(filename))

    def load_file(self, path: Path) -> None:
        try:
            data = read_json_file(path)
            if not isinstance(data, dict):
                raise ValueError("le document racine doit etre un objet JSON")
        except Exception as exc:
            messagebox.showerror("Ouverture impossible", str(exc))
            return

        self.path = path
        self.data = data
        self.editor.delete("1.0", END)
        self.editor.insert("1.0", json.dumps(data, indent=2, ensure_ascii=False))
        self.validate_editor()

    def validate_editor(self) -> None:
        try:
            data = json.loads(self.editor.get("1.0", END))
            if not isinstance(data, dict):
                raise ValueError("le document racine doit etre un objet JSON")
        except Exception as exc:
            self.data = None
            self.pack = None
            self.validation = ValidationResult(errors=[])
            self._set_validation_text(f"ERROR {exc}")
            self.status.set("JSON invalide")
            self._reload_pattern_list()
            return

        self.data = data
        self.validation = validate_pack_data(data)
        self.pack = pattern_pack_from_data(data) if self.validation.ok else None
        self._reload_pattern_list()
        self._render_validation()

        if self.validation.ok:
            self.status.set(
                f"OK - {self.validation.pattern_count} pattern(s), {self.validation.step_count} step(s)"
            )
        else:
            self.status.set(f"{len(self.validation.errors)} erreur(s)")

    def save_file(self) -> None:
        if self.path is None:
            self.save_file_as()
            return
        self._write_editor_to(self.path)

    def save_file_as(self) -> None:
        filename = filedialog.asksaveasfilename(
            title="Sauver le pack JSON",
            defaultextension=".json",
            filetypes=(("JSON", "*.json"), ("Tous les fichiers", "*.*")),
        )
        if filename:
            self.path = Path(filename)
            self._write_editor_to(self.path)

    def export_dat(self) -> None:
        if not self.validate_editor_for_action():
            return

        filename = filedialog.asksaveasfilename(
            title="Exporter patterns.dat",
            defaultextension=".dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
            initialfile="patterns.dat",
        )
        if not filename:
            return

        temp_json = self._current_json_path_for_export()
        if temp_json is None:
            return

        try:
            result = validate_binary_export_file(temp_json)
            if not result.ok:
                self._render_binary_validation_errors(result)
                messagebox.showerror("Export impossible", "Le pack contient des erreurs bloquantes.")
                return
            binary_pack = export_dat_file(temp_json, filename)
        except Exception as exc:
            messagebox.showerror("Export impossible", str(exc))
            return

        self._set_validation_text(
            self._validation_text_with_binary_summary(
                f"DAT ecrit: {filename}\n"
                f"patterns={len(binary_pack.patterns)} points={binary_pack.point_count} "
                f"bytes={binary_pack.file_size} crc32=0x{binary_pack.checksum_crc32:08x}"
            )
        )
        self.status.set(f"DAT exporte: {filename}")
        messagebox.showinfo("Export DAT", f"patterns.dat exporte:\n{filename}")

    def inspect_dat(self) -> None:
        filename = filedialog.askopenfilename(
            title="Inspecter patterns.dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
        )
        if not filename:
            return

        try:
            binary_pack = read_dat_file(filename)
        except Exception as exc:
            self._set_validation_text(f"ERROR {exc}")
            self.status.set("DAT invalide")
            messagebox.showerror("DAT invalide", str(exc))
            return

        lines = [
            "DAT OK",
            f"fichier: {filename}",
            f"patterns={len(binary_pack.patterns)}",
            f"points={binary_pack.point_count}",
            f"bytes={binary_pack.file_size}",
            f"crc32=0x{binary_pack.checksum_crc32:08x}",
            "",
        ]
        for pattern in binary_pack.patterns:
            lines.append(
                f"{pattern.pattern_id}: weight={pattern.weight} "
                f"points={len(pattern.points)} duration_ms={pattern.duration_total_ms}"
            )
        self._set_validation_text("\n".join(lines))
        self.status.set(f"DAT OK: {len(binary_pack.patterns)} pattern(s)")

    def _write_editor_to(self, path: Path) -> None:
        try:
            data = json.loads(self.editor.get("1.0", END))
            path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        except Exception as exc:
            messagebox.showerror("Sauvegarde impossible", str(exc))
            return
        self.validate_editor()
        self.status.set(f"Sauve: {path}")

    def validate_editor_for_action(self) -> bool:
        self.validate_editor()
        if self.data is None or self.pack is None or not self.validation.ok:
            messagebox.showerror("Validation requise", "Corriger le JSON avant cette action.")
            return False
        return True

    def _current_json_path_for_export(self) -> Path | None:
        if self.path is None:
            filename = filedialog.asksaveasfilename(
                title="Sauver le JSON avant export",
                defaultextension=".json",
                filetypes=(("JSON", "*.json"), ("Tous les fichiers", "*.*")),
            )
            if not filename:
                return None
            self.path = Path(filename)
        self._write_editor_to(self.path)
        return self.path

    def _render_binary_validation_errors(self, result: ValidationResult) -> None:
        lines = []
        for issue in result.errors:
            lines.append(f"ERROR {issue.format()}")
        for issue in result.warnings:
            lines.append(f"WARN {issue.format()}")
        self._set_validation_text("\n".join(lines) if lines else "OK")

    def _validation_text_with_binary_summary(self, summary: str) -> str:
        current = self.validation_text.get("1.0", END).strip()
        if current:
            return f"{summary}\n\n{current}"
        return summary

    def _reload_pattern_list(self) -> None:
        selected = self.selected_pattern_id()
        self.patterns.delete(0, END)
        if self.pack is None:
            self._clear_pattern_view()
            return

        new_index = 0
        for index, pattern in enumerate(self.pack.patterns):
            label = f"{pattern.id}  ({len(pattern.steps)} steps, {pattern_duration_ms(pattern)} ms)"
            self.patterns.insert(END, label)
            if pattern.id == selected:
                new_index = index
        if self.pack.patterns:
            self.patterns.selection_set(new_index)
            self.patterns.activate(new_index)
            self.select_pattern()

    def selected_pattern_id(self) -> str | None:
        if self.pack is None:
            return None
        selection = self.patterns.curselection()
        if not selection:
            return None
        index = int(selection[0])
        if index >= len(self.pack.patterns):
            return None
        return self.pack.patterns[index].id

    def select_pattern(self) -> None:
        pattern = self.selected_pattern()
        if pattern is None:
            self._clear_pattern_view()
            return
        self._show_details(pattern)
        self._draw_pattern(pattern)

    def selected_pattern(self) -> Pattern | None:
        if self.pack is None:
            return None
        selection = self.patterns.curselection()
        if not selection:
            return None
        index = int(selection[0])
        if index >= len(self.pack.patterns):
            return None
        return self.pack.patterns[index]

    def redraw_selected_pattern(self) -> None:
        pattern = self.selected_pattern()
        if pattern is not None:
            self._draw_pattern(pattern)

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
        self.zoom = min(max(value, 1.0), 5.0)
        self._clamp_pan()
        self.zoom_label.set(f"{int(self.zoom * 100)}%")
        self.redraw_selected_pattern()

    def start_pan(self, event) -> None:
        if self.zoom <= 1.0:
            self.drag_start = None
            return
        self.drag_start = (event.x, event.y, self.pan_x, self.pan_y)
        self.canvas.configure(cursor="fleur")

    def drag_pan(self, event) -> None:
        if self.drag_start is None:
            return
        start_x, start_y, start_pan_x, start_pan_y = self.drag_start
        plot = self._plot_bounds()
        plot_width = max(plot[2] - plot[0], 1)
        plot_height = max(plot[3] - plot[1], 1)
        visible_span = 2.0 / self.zoom
        dx = event.x - start_x
        dy = event.y - start_y
        self.pan_x = start_pan_x - (dx / plot_width) * visible_span
        self.pan_y = start_pan_y + (dy / plot_height) * visible_span
        self._clamp_pan()
        self.redraw_selected_pattern()

    def end_pan(self, _event) -> None:
        self.drag_start = None
        self.canvas.configure(cursor="")

    def _clamp_pan(self) -> None:
        if self.zoom <= 1.0:
            self.pan_x = 0.0
            self.pan_y = 0.0
            return
        half_window = 1.0 / self.zoom
        max_center = 1.0 - half_window
        self.pan_x = min(max(self.pan_x, -max_center), max_center)
        self.pan_y = min(max(self.pan_y, -max_center), max_center)

    def _plot_bounds(self) -> tuple[int, int, int, int]:
        width = max(self.canvas.winfo_width(), 200)
        height = max(self.canvas.winfo_height(), 200)
        return 48, 132, width - 28, height - 34

    def _show_details(self, pattern: Pattern) -> None:
        lines = [
            f"id: {pattern.id}",
            f"nom: {pattern.name}",
            f"categorie: {pattern.category}",
            f"poids: {pattern.weight}",
            f"intensite: {pattern.intensity}",
            f"duree: {pattern_duration_ms(pattern)} ms",
            "",
            "steps:",
        ]
        for index, step in enumerate(pattern.steps, start=1):
            position = ""
            if step.x is not None and step.y is not None:
                position = f" x={step.x} y={step.y}"
            amplitude = f" amplitude={step.amplitude}" if step.amplitude is not None else ""
            lines.append(
                f"{index:02d}. {step.type} laser={step.laser} duration_ms={step.duration_ms}"
                f"{position}{amplitude}"
            )
        self.details.delete("1.0", END)
        self.details.insert("1.0", "\n".join(lines))

    def _draw_pattern(self, pattern: Pattern) -> None:
        self.canvas.delete("all")
        width = max(self.canvas.winfo_width(), 200)
        height = max(self.canvas.winfo_height(), 200)
        plot_left, plot_top, plot_right, plot_bottom = self._plot_bounds()
        plot_width = max(plot_right - plot_left, 1)
        plot_height = max(plot_bottom - plot_top, 1)
        visible_span = 2.0 / self.zoom
        half_window = visible_span / 2.0
        visible_x_min = self.pan_x - half_window
        visible_x_max = self.pan_x + half_window
        visible_y_min = self.pan_y - half_window
        visible_y_max = self.pan_y + half_window

        def point(x: float, y: float) -> tuple[float, float]:
            px = plot_left + ((x - visible_x_min) / visible_span) * plot_width
            py = plot_bottom - ((y - visible_y_min) / visible_span) * plot_height
            return px, py

        self._draw_canvas_header(pattern, width)
        self._draw_canvas_grid(
            plot_left,
            plot_top,
            plot_right,
            plot_bottom,
            visible_x_min,
            visible_x_max,
            visible_y_min,
            visible_y_max,
        )

        for index, segment in enumerate(pattern_segments(pattern), start=1):
            x1, y1 = point(*segment.start)
            x2, y2 = point(*segment.end)
            color = "#d7263d" if segment.laser else "#2a6fbb"
            dash = None if segment.laser else (7, 5)
            width_px = 3 if segment.laser else 2
            self.canvas.create_line(
                x1,
                y1,
                x2,
                y2,
                fill=color,
                width=width_px,
                dash=dash,
                arrow="last",
                arrowshape=(12, 14, 5),
            )
            self._draw_step_badge(index, (x1 + x2) / 2, (y1 + y2) / 2, color)

        segment_count = len(pattern_segments(pattern))
        for index, pause in enumerate(pattern_pauses(pattern), start=segment_count + 1):
            x, y = point(*pause.point)
            color = "#d7263d" if pause.laser else "#2a6fbb"
            fill = "#ffe9ec" if pause.laser else "#eaf3ff"
            self.canvas.create_oval(x - 7, y - 7, x + 7, y + 7, outline=color, fill=fill, width=2)
            self._draw_step_badge(index, x + 15, y - 15, color)

        for zone in pattern_jitter_zones(pattern):
            x, y = point(*zone.center)
            radius = zone.amplitude * min(plot_width, plot_height) / visible_span
            self.canvas.create_oval(
                x - radius,
                y - radius,
                x + radius,
                y + radius,
                outline="#f28c28",
                width=2,
                dash=(2, 3),
            )
            self.canvas.create_text(x, y - radius - 10, text="jitter", fill="#9a560e", font=("TkDefaultFont", 8))

        positions = [
            (float(step.x), float(step.y))
            for step in pattern.steps
            if step.x is not None and step.y is not None
        ]
        if positions:
            sx, sy = point(*positions[0])
            ex, ey = point(*positions[-1])
            self.canvas.create_oval(sx - 9, sy - 9, sx + 9, sy + 9, outline="#1f8f45", fill="#e7f7eb", width=2)
            self.canvas.create_text(sx, sy - 18, text="depart", fill="#1f8f45", font=("TkDefaultFont", 8, "bold"))
            self.canvas.create_line(ex - 7, ey - 7, ex + 7, ey + 7, fill="#111111", width=2)
            self.canvas.create_line(ex - 7, ey + 7, ex + 7, ey - 7, fill="#111111", width=2)
            self.canvas.create_text(ex, ey + 18, text="fin", fill="#111111", font=("TkDefaultFont", 8, "bold"))

    def _draw_canvas_header(self, pattern: Pattern, width: int) -> None:
        title = f"{pattern.id} - {len(pattern.steps)} steps - {pattern_duration_ms(pattern)} ms"
        self.canvas.create_text(12, 12, text=title, anchor="nw", fill="#222222", font=("TkDefaultFont", 10, "bold"))
        self.canvas.create_text(
            12,
            32,
            text=f"zoom {int(self.zoom * 100)}% - molette, +, -, 100% - clic gauche glisse pour deplacer",
            anchor="nw",
            fill="#69717a",
            font=("TkDefaultFont", 8),
        )

        y = 62
        x = 14
        items = [
            ("#d7263d", None, "laser ON"),
            ("#2a6fbb", (7, 5), "laser OFF / deplacement invisible"),
            ("#f28c28", (2, 3), "zone jitter"),
            ("#1f8f45", None, "depart"),
            ("#111111", None, "fin"),
        ]
        for color, dash, label in items:
            item_width = 255 if "deplacement invisible" in label else 132
            if x + item_width > width - 12:
                x = 14
                y += 28
            if label == "zone jitter":
                self.canvas.create_oval(x, y - 5, x + 20, y + 5, outline=color, width=2, dash=dash)
            elif label == "depart":
                self.canvas.create_oval(x + 4, y - 6, x + 16, y + 6, outline=color, fill="#e7f7eb", width=2)
            elif label == "fin":
                self.canvas.create_line(x + 5, y - 6, x + 15, y + 6, fill=color, width=2)
                self.canvas.create_line(x + 5, y + 6, x + 15, y - 6, fill=color, width=2)
            else:
                self.canvas.create_line(x, y, x + 22, y, fill=color, width=3, dash=dash)
            self.canvas.create_text(x + 28, y, text=label, anchor="w", fill="#333333", font=("TkDefaultFont", 8))
            x += item_width

    def _draw_canvas_grid(
        self,
        left: int,
        top: int,
        right: int,
        bottom: int,
        visible_x_min: float,
        visible_x_max: float,
        visible_y_min: float,
        visible_y_max: float,
    ) -> None:
        self.canvas.create_rectangle(left, top, right, bottom, outline="#b9c0c8", width=1)

        for value in (-1.0, -0.5, 0.0, 0.5, 1.0):
            line_color = "#d1d6dd" if value == 0.0 else "#edf0f3"
            line_width = 2 if value == 0.0 else 1
            if visible_x_min <= value <= visible_x_max:
                ratio_x = (value - visible_x_min) / (visible_x_max - visible_x_min)
                x = left + ratio_x * (right - left)
                self.canvas.create_line(x, top, x, bottom, fill=line_color, width=line_width)
                self.canvas.create_text(x, bottom + 14, text=f"{value:g}", fill="#69717a", font=("TkDefaultFont", 8))
            if visible_y_min <= value <= visible_y_max:
                ratio_y = (value - visible_y_min) / (visible_y_max - visible_y_min)
                y = bottom - ratio_y * (bottom - top)
                self.canvas.create_line(left, y, right, y, fill=line_color, width=line_width)
                self.canvas.create_text(left - 16, y, text=f"{value:g}", fill="#69717a", font=("TkDefaultFont", 8))

        if self.zoom > 1.0:
            self.canvas.create_text(
                right - 4,
                top + 14,
                text=(
                    f"x {visible_x_min:.2f}..{visible_x_max:.2f}  "
                    f"y {visible_y_min:.2f}..{visible_y_max:.2f}"
                ),
                anchor="e",
                fill="#69717a",
                font=("TkDefaultFont", 8),
            )

        self.canvas.create_text(
            (left + right) / 2,
            bottom + 27,
            text="x gauche -> droite",
            fill="#69717a",
            font=("TkDefaultFont", 8),
        )
        self.canvas.create_text(left - 30, (top + bottom) / 2, text="y", fill="#69717a", font=("TkDefaultFont", 8))

    def _draw_step_badge(self, index: int, x: float, y: float, color: str) -> None:
        radius = 9
        self.canvas.create_oval(x - radius, y - radius, x + radius, y + radius, outline=color, fill="white", width=1)
        self.canvas.create_text(x, y, text=str(index), fill=color, font=("TkDefaultFont", 7, "bold"))

    def _clear_pattern_view(self) -> None:
        self.canvas.delete("all")
        self.details.delete("1.0", END)

    def _render_validation(self) -> None:
        lines = []
        for issue in self.validation.errors:
            lines.append(f"ERROR {issue.format()}")
        for issue in self.validation.warnings:
            lines.append(f"WARN {issue.format()}")
        if not lines:
            lines.append("OK")
        self._set_validation_text("\n".join(lines))

    def _set_validation_text(self, value: str) -> None:
        self.validation_text.delete("1.0", END)
        self.validation_text.insert("1.0", value)


def run_gui(initial_path: str | Path | None = None) -> int:
    try:
        root = Tk()
    except Exception as exc:
        print(f"ERROR impossible de demarrer la GUI: {exc}")
        return 1

    path = Path(initial_path) if initial_path else None
    PatternGui(root, path)
    root.geometry("1200x760")
    root.mainloop()
    return 0
