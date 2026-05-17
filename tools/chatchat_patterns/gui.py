from __future__ import annotations

from pathlib import Path
from tkinter import BOTH, END, LEFT, RIGHT, X, filedialog, messagebox
from tkinter import Canvas, Listbox, StringVar, Text, Tk
from tkinter import ttk

from .app_service import PatternAppService
from .binary import BinaryPattern, LPTN_ACTION_HOLD, LPTN_ACTION_JITTER, LPTN_ACTION_MOVE, LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE


class PatternGui:
    def __init__(self, root: Tk, initial_path: Path | None = None) -> None:
        self.service = PatternAppService()
        self.root = root
        self.root.title("CatChat Patterns DAT")
        self.path: Path | None = None
        self.pack = None
        self.status = StringVar(value="Aucun fichier DAT charge")
        self.zoom = 1.0
        self.pan_x = 0.0
        self.pan_y = 0.0
        self.drag_start: tuple[int, int, float, float] | None = None
        self.zoom_label = StringVar(value="100%")

        self._build_ui()

        if initial_path is not None:
            self.load_dat(initial_path)

    def _build_ui(self) -> None:
        toolbar = ttk.Frame(self.root, padding=6)
        toolbar.pack(fill=X)

        ttk.Button(toolbar, text="Ouvrir DAT", command=self.open_dat).pack(side=LEFT, padx=(0, 6))
        ttk.Button(toolbar, text="Inspecter", command=self.inspect_dat).pack(side=LEFT, padx=(0, 6))
        ttk.Label(toolbar, textvariable=self.status).pack(side=LEFT, padx=(12, 0))

        main = ttk.PanedWindow(self.root, orient="horizontal")
        main.pack(fill=BOTH, expand=True)

        left = ttk.Frame(main, padding=6)
        main.add(left, weight=1)
        ttk.Label(left, text="Patterns DAT").pack(anchor="w")
        self.patterns = Listbox(left, height=18, exportselection=False)
        self.patterns.pack(fill=BOTH, expand=True)
        self.patterns.bind("<<ListboxSelect>>", lambda _event: self.select_pattern())

        center = ttk.Frame(main, padding=6)
        main.add(center, weight=3)
        preview_toolbar = ttk.Frame(center)
        preview_toolbar.pack(fill=X)
        ttk.Label(preview_toolbar, text="Apercu trajectoire").pack(side=LEFT)
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
        self.canvas.bind("<ButtonPress-1>", self.start_pan)
        self.canvas.bind("<B1-Motion>", self.drag_pan)
        self.canvas.bind("<ButtonRelease-1>", self.end_pan)

        right = ttk.PanedWindow(main, orient="vertical")
        main.add(right, weight=2)

        detail_frame = ttk.Frame(right, padding=6)
        right.add(detail_frame, weight=1)
        ttk.Label(detail_frame, text="Details").pack(anchor="w")
        self.details = Text(detail_frame, height=10, wrap="none")
        self.details.pack(fill=BOTH, expand=True)

        inspect_frame = ttk.Frame(right, padding=6)
        right.add(inspect_frame, weight=1)
        ttk.Label(inspect_frame, text="Inspection DAT").pack(anchor="w")
        self.inspect_text = Text(inspect_frame, height=10, wrap="word")
        self.inspect_text.pack(fill=BOTH, expand=True)

    def open_dat(self) -> None:
        filename = filedialog.askopenfilename(
            title="Ouvrir patterns.dat",
            filetypes=(("Patterns DAT", "*.dat"), ("Tous les fichiers", "*.*")),
        )
        if filename:
            self.load_dat(Path(filename))

    def load_dat(self, path: Path) -> None:
        try:
            inspected = self.service.load_dat(path)
        except Exception as exc:
            messagebox.showerror("Ouverture impossible", str(exc))
            return

        self.path = path
        self.pack = inspected.pack
        self._set_inspect_text(inspected.summary)
        self._reload_pattern_list()
        self.status.set(f"DAT charge: {path}")

    def inspect_dat(self) -> None:
        if self.path is None:
            self.open_dat()
            return
        self.load_dat(self.path)

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
        sel = self.patterns.curselection()
        if not sel:
            return None
        idx = int(sel[0])
        if idx >= len(self.pack.patterns):
            return None
        return self.pack.patterns[idx]

    def select_pattern(self) -> None:
        pattern = self.selected_pattern()
        if pattern is None:
            return
        self._show_details(pattern)
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
        self.select_pattern()

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
        left, top, right, bottom = self._plot_bounds()
        plot_width = max(right - left, 1)
        plot_height = max(bottom - top, 1)
        visible_span = 1000.0 / self.zoom
        dx = event.x - start_x
        dy = event.y - start_y
        self.pan_x = start_pan_x - (dx / plot_width) * visible_span
        self.pan_y = start_pan_y + (dy / plot_height) * visible_span
        self._clamp_pan()
        self.select_pattern()

    def end_pan(self, _event) -> None:
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

    def _show_details(self, pattern: BinaryPattern) -> None:
        lines = [
            f"id: {pattern.pattern_id}",
            f"weight: {pattern.weight}",
            f"flags: 0x{pattern.flags:02x}",
            f"duree: {pattern.duration_total_ms} ms",
            f"points: {len(pattern.points)}",
            f"bbox: x={pattern.x_min}..{pattern.x_max} y={pattern.y_min}..{pattern.y_max}",
            "",
            "liste des points:",
        ]
        for i, point in enumerate(pattern.points, start=1):
            action_name = {
                LPTN_ACTION_HOLD: "hold",
                LPTN_ACTION_MOVE: "move",
                LPTN_ACTION_JITTER: "jitter",
                LPTN_ACTION_OFF_HOLD: "off_hold",
                LPTN_ACTION_OFF_MOVE: "off_move",
            }.get(point.action, "unknown")
            lines.append(
                f"{i:02d}. x={point.x:4d} y={point.y:4d} duration_ms={point.duration_ms:5d} "
                f"laser={point.laser} action={action_name} arg={point.arg}"
            )
        self.details.delete("1.0", END)
        self.details.insert("1.0", "\n".join(lines))

    def _draw_pattern(self, pattern: BinaryPattern) -> None:
        self.canvas.delete("all")
        w = max(self.canvas.winfo_width(), 200)
        h = max(self.canvas.winfo_height(), 200)
        left, top, right, bottom = self._plot_bounds()
        plot_w = max(right - left, 1)
        plot_h = max(bottom - top, 1)
        self.canvas.create_rectangle(left, top, right, bottom, outline="#b9c0c8", width=1)
        self.canvas.create_text(
            left,
            16,
            anchor="w",
            text="Legende: point plein=laser ON, point vide=laser OFF, cercle orange=jitter, numero=index point",
            fill="#444444",
            font=("TkDefaultFont", 8),
        )
        self.canvas.create_text(
            left,
            34,
            anchor="w",
            text=f"Zoom {int(self.zoom * 100)}% - molette pour zoomer, clic gauche + glisser pour deplacer",
            fill="#666666",
            font=("TkDefaultFont", 8),
        )

        visible_span = 1000.0 / self.zoom
        visible_x_min = 500.0 + self.pan_x - (visible_span / 2.0)
        visible_x_max = 500.0 + self.pan_x + (visible_span / 2.0)
        visible_y_min = 500.0 + self.pan_y - (visible_span / 2.0)
        visible_y_max = 500.0 + self.pan_y + (visible_span / 2.0)

        def p(x: int, y: int) -> tuple[float, float]:
            xn = (x - visible_x_min) / visible_span
            yn = (y - visible_y_min) / visible_span
            return left + xn * plot_w, top + yn * plot_h

        self._draw_axes(left, top, right, bottom, visible_x_min, visible_x_max, visible_y_min, visible_y_max)

        prev = None
        for idx, point in enumerate(pattern.points, start=1):
            cur = p(point.x, point.y)
            laser_on = point.laser == 1 and point.action not in {LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE}
            color = "#d7263d" if laser_on else "#2a6fbb"

            if prev is not None and point.action in {LPTN_ACTION_MOVE, LPTN_ACTION_OFF_MOVE}:
                self.canvas.create_line(prev[0], prev[1], cur[0], cur[1], fill=color, width=2, dash=None if laser_on else (7, 5))
            # Show every point explicitly, regardless of action.
            if laser_on:
                self.canvas.create_oval(cur[0] - 4, cur[1] - 4, cur[0] + 4, cur[1] + 4, fill=color, outline=color)
            else:
                self.canvas.create_oval(cur[0] - 4, cur[1] - 4, cur[0] + 4, cur[1] + 4, fill="white", outline=color, width=2)
            if point.action == LPTN_ACTION_JITTER:
                r = (point.arg / 1000.0) * min(right - left, bottom - top)
                self.canvas.create_oval(cur[0] - r, cur[1] - r, cur[0] + r, cur[1] + r, outline="#f28c28", dash=(2, 3))
            self.canvas.create_text(cur[0] + 9, cur[1] - 9, text=str(idx), fill="#111111", font=("TkDefaultFont", 7))

            prev = cur

    def _draw_axes(
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
        for raw in (0, 250, 500, 750, 1000):
            color = "#d1d6dd" if raw == 500 else "#edf0f3"
            width = 2 if raw == 500 else 1
            if visible_x_min <= raw <= visible_x_max:
                x = left + ((raw - visible_x_min) / (visible_x_max - visible_x_min)) * (right - left)
                self.canvas.create_line(x, top, x, bottom, fill=color, width=width)
                self.canvas.create_text(x, bottom + 14, text=str(raw), fill="#69717a", font=("TkDefaultFont", 8))
            if visible_y_min <= raw <= visible_y_max:
                y = top + ((raw - visible_y_min) / (visible_y_max - visible_y_min)) * (bottom - top)
                self.canvas.create_line(left, y, right, y, fill=color, width=width)
                self.canvas.create_text(left - 18, y, text=str(raw), fill="#69717a", font=("TkDefaultFont", 8))

        self.canvas.create_text((left + right) / 2, bottom + 30, text="Axe X (0 -> 1000)", fill="#69717a", font=("TkDefaultFont", 8))
        self.canvas.create_text(left - 30, (top + bottom) / 2, text="Axe Y", fill="#69717a", font=("TkDefaultFont", 8))

    def _set_inspect_text(self, text: str) -> None:
        self.inspect_text.delete("1.0", END)
        self.inspect_text.insert("1.0", text)


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
