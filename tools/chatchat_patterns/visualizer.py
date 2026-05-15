from __future__ import annotations

from pathlib import Path

from .geometry import pattern_duration_ms, pattern_jitter_zones, pattern_pauses, pattern_segments
from .model import Pattern


def render_pattern(pattern: Pattern, output: str | Path | None = None) -> None:
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Circle
    except ImportError as exc:
        raise RuntimeError(
            "matplotlib est requis pour la visualisation. Installer matplotlib ou utiliser validate/list."
        ) from exc

    fig, ax = plt.subplots(figsize=(7, 7))
    ax.set_title(f"{pattern.id} - {pattern_duration_ms(pattern)} ms")
    ax.set_xlim(-1.05, 1.05)
    ax.set_ylim(-1.05, 1.05)
    ax.set_aspect("equal", adjustable="box")
    ax.grid(True, linewidth=0.4, alpha=0.35)
    ax.axhline(0, color="0.7", linewidth=0.6)
    ax.axvline(0, color="0.7", linewidth=0.6)

    for segment in pattern_segments(pattern):
        style = "-" if segment.laser else "--"
        color = "#d62728" if segment.laser else "#1f77b4"
        ax.plot(
            [segment.start[0], segment.end[0]],
            [segment.start[1], segment.end[1]],
            linestyle=style,
            color=color,
            linewidth=2,
        )

    for pause in pattern_pauses(pattern):
        color = "#d62728" if pause.laser else "#1f77b4"
        ax.scatter([pause.point[0]], [pause.point[1]], s=40, color=color, zorder=3)

    for zone in pattern_jitter_zones(pattern):
        ax.add_patch(
            Circle(
                zone.center,
                zone.amplitude,
                fill=False,
                linestyle=":",
                linewidth=1.5,
                color="#ff7f0e",
            )
        )

    if pattern.steps:
        first = next((step for step in pattern.steps if step.x is not None and step.y is not None), None)
        last = next(
            (step for step in reversed(pattern.steps) if step.x is not None and step.y is not None),
            None,
        )
        if first is not None:
            ax.scatter([first.x], [first.y], marker="o", s=90, facecolors="none", edgecolors="green")
        if last is not None:
            ax.scatter([last.x], [last.y], marker="x", s=90, color="black")

    ax.set_xlabel("x")
    ax.set_ylabel("y")
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150)
    else:
        plt.show()

    plt.close(fig)
