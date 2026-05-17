from __future__ import annotations

from pathlib import Path

from .binary import (
    BinaryPattern,
    LPTN_ACTION_HOLD,
    LPTN_ACTION_JITTER,
    LPTN_ACTION_MOVE,
    LPTN_ACTION_OFF_HOLD,
    LPTN_ACTION_OFF_MOVE,
)


def render_binary_pattern(pattern: BinaryPattern, output: str | Path | None = None) -> None:
    try:
        import matplotlib.pyplot as plt
        from matplotlib.patches import Circle
    except ImportError as exc:
        raise RuntimeError("matplotlib est requis pour la visualisation") from exc

    fig, ax = plt.subplots(figsize=(7, 7))
    ax.set_title(f"{pattern.pattern_id} - {pattern.duration_total_ms} ms")
    ax.set_xlim(-1.05, 1.05)
    ax.set_ylim(-1.05, 1.05)
    ax.set_aspect("equal", adjustable="box")
    ax.grid(True, linewidth=0.4, alpha=0.35)

    prev = None
    for point in pattern.points:
        x = _norm(point.x)
        y = _norm(point.y)
        cur = (x, y)
        laser_on = point.laser == 1 and point.action not in {LPTN_ACTION_OFF_HOLD, LPTN_ACTION_OFF_MOVE}

        if prev is not None and point.action in {LPTN_ACTION_MOVE, LPTN_ACTION_OFF_MOVE}:
            ax.plot(
                [prev[0], cur[0]],
                [prev[1], cur[1]],
                linestyle="-" if laser_on else "--",
                color="#d62728" if laser_on else "#1f77b4",
                linewidth=2,
            )
        elif point.action in {LPTN_ACTION_HOLD, LPTN_ACTION_OFF_HOLD}:
            ax.scatter([x], [y], s=35, color="#d62728" if laser_on else "#1f77b4", zorder=3)

        if point.action == LPTN_ACTION_JITTER:
            amp = point.arg / 1000.0
            ax.add_patch(Circle((x, y), amp, fill=False, linestyle=":", linewidth=1.5, color="#ff7f0e"))

        prev = cur

    if pattern.points:
        first = pattern.points[0]
        last = pattern.points[-1]
        ax.scatter([_norm(first.x)], [_norm(first.y)], marker="o", s=90, facecolors="none", edgecolors="green")
        ax.scatter([_norm(last.x)], [_norm(last.y)], marker="x", s=90, color="black")

    ax.set_xlabel("x")
    ax.set_ylabel("y")
    fig.tight_layout()

    if output:
        fig.savefig(output, dpi=150)
    else:
        plt.show()

    plt.close(fig)


def _norm(v: int) -> float:
    return (float(v) - 500.0) / 500.0
