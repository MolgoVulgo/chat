# Implémentation — Trajectoires fluides (Python-expand) + Persistance SPIFFS

## Vue d'ensemble

Ce document décrit deux chantiers indépendants mais cohérents :

1. **Trajectoires fluides** : ajout des primitives `curve` et `arc` dans le
   toolchain Python, expansion automatique en steps `MOVE` classiques avant
   export `.dat`. Firmware ESP inchangé.

2. **Persistance SPIFFS** : sauvegarde des variables d'état en flash pour
   survivre aux redémarrages. SPIFFS est déjà monté et fonctionnel.

Les deux chantiers sont indépendants et peuvent être livrés séparément.

---

# Chantier 1 — Trajectoires fluides

## Périmètre

| Fichier | Action |
|---|---|
| `tools/chatchat_patterns/model.py` | Ajouter champs optionnels à `PatternStep` |
| `tools/chatchat_patterns/geometry.py` | Ajouter `expand_curve()`, `expand_arc()` |
| `tools/chatchat_patterns/expander.py` | Créer — `expand_pattern()`, `expand_pack()` |
| `tools/chatchat_patterns/validator.py` | Étendre `STEP_TYPES`, ajouter validations |
| `tools/chatchat_patterns/binary.py` | Appeler `expand_pack()` avant sérialisation |
| Firmware ESP | **Aucun changement** |

---

## Étape 1 — `model.py` : nouveaux champs `PatternStep`

Ajouter les champs optionnels utilisés par `curve` et `arc`. Tous ont une
valeur par défaut `None` — rétrocompatibilité totale avec les steps existants.

```python
# model.py

STEP_TYPES = {
    "hold", "move", "jitter", "off_hold", "off_move",
    "curve", "arc"                                      # ← nouveaux
}

POSITION_STEP_TYPES = {
    "hold", "move", "jitter", "off_move", "curve"       # arc n'a pas de destination
}

@dataclass(frozen=True)
class PatternStep:
    type: str
    duration_ms: int
    laser: bool | None = None
    x: float | None = None
    y: float | None = None
    amplitude: float | None = None
    raw: dict[str, Any] = field(default_factory=dict)
    # nouveaux champs géométriques
    cx: float | None = None          # curve: contrôle X | arc: centre X
    cy: float | None = None          # curve: contrôle Y | arc: centre Y
    radius_x: float | None = None    # arc: rayon horizontal
    radius_y: float | None = None    # arc: rayon vertical
    angle_start_deg: float | None = None
    angle_end_deg: float | None = None
    steps: int | None = None         # nombre de segments à générer
```

Mettre à jour `pattern_pack_from_data()` pour lire les nouveaux champs :

```python
PatternStep(
    type=str(step_data.get("type", "")),
    laser=step_data.get("laser"),
    x=step_data.get("x"),
    y=step_data.get("y"),
    duration_ms=step_data.get("duration_ms", 0),
    amplitude=step_data.get("amplitude"),
    raw=step_data,
    cx=step_data.get("cx"),
    cy=step_data.get("cy"),
    radius_x=step_data.get("radius_x"),
    radius_y=step_data.get("radius_y"),
    angle_start_deg=step_data.get("angle_start_deg"),
    angle_end_deg=step_data.get("angle_end_deg"),
    steps=step_data.get("steps"),
)
```

### Tests étape 1

```python
# tests/test_model.py

def test_pattern_step_defaults():
    """Les nouveaux champs sont None par défaut — pas de régression."""
    step = PatternStep(type="move", duration_ms=1000)
    assert step.cx is None
    assert step.cy is None
    assert step.radius_x is None
    assert step.steps is None

def test_load_pack_ignores_unknown_fields():
    """Un JSON sans cx/cy charge sans erreur."""
    data = minimal_pack_json()
    pack = pattern_pack_from_data(data)
    assert pack.patterns[0].steps[0].cx is None

def test_load_pack_reads_curve_fields():
    """Un step curve avec cx/cy est lu correctement."""
    data = pack_with_curve_step()
    pack = pattern_pack_from_data(data)
    step = pack.patterns[0].steps[0]
    assert step.type == "curve"
    assert step.cx == 0.5
    assert step.cy == -0.3
    assert step.steps == 12

def test_load_pack_reads_arc_fields():
    data = pack_with_arc_step()
    pack = pattern_pack_from_data(data)
    step = pack.patterns[0].steps[0]
    assert step.type == "arc"
    assert step.radius_x == 0.4
    assert step.angle_end_deg == 360
```

---

## Étape 2 — `geometry.py` : fonctions d'expansion

Ajouter à la fin du fichier existant. Ne pas modifier les fonctions existantes.

```python
# geometry.py — ajouts

import math


def _n_steps_for(step_or_n: PatternStep | int, default: int = 10) -> int:
    """Résout le nombre de segments : champ steps du step ou défaut."""
    if isinstance(step_or_n, int):
        return max(2, step_or_n)
    n = step_or_n.steps
    return max(2, n) if n is not None else default


def _step_duration(total_ms: int, n: int) -> int:
    """Durée par segment, minimum 80 ms (contrainte binary.py)."""
    return max(80, total_ms // n)


def expand_curve(
    start: tuple[float, float],
    step: PatternStep,
) -> list[PatternStep]:
    """
    Bezier quadratique : P0=start, P1=(cx,cy), P2=(x,y).
    Retourne n steps MOVE classiques.
    """
    p0x, p0y = start
    p1x, p1y = float(step.cx), float(step.cy)
    p2x, p2y = float(step.x), float(step.y)
    n = _n_steps_for(step)
    seg_ms = _step_duration(step.duration_ms, n)
    laser = bool(step.laser) if step.laser is not None else True

    result = []
    for i in range(1, n + 1):
        t = i / n
        inv = 1.0 - t
        x = inv * inv * p0x + 2 * inv * t * p1x + t * t * p2x
        y = inv * inv * p0y + 2 * inv * t * p1y + t * t * p2y
        # clamp coords dans -1.0..1.0
        x = max(-1.0, min(1.0, x))
        y = max(-1.0, min(1.0, y))
        result.append(PatternStep(
            type="move",
            laser=laser,
            x=round(x, 4),
            y=round(y, 4),
            duration_ms=seg_ms,
            raw={},
        ))
    return result


def expand_arc(
    step: PatternStep,
) -> list[PatternStep]:
    """
    Arc d'ellipse centré en (cx,cy), rayons (radius_x, radius_y).
    angle_start_deg → angle_end_deg en degrés, sens trigonométrique.
    Retourne n steps MOVE classiques.
    """
    cx, cy = float(step.cx), float(step.cy)
    rx = float(step.radius_x)
    ry = float(step.radius_y)
    a_start = float(step.angle_start_deg)
    a_end = float(step.angle_end_deg)
    n = _n_steps_for(step, default=16)
    seg_ms = _step_duration(step.duration_ms, n)
    laser = bool(step.laser) if step.laser is not None else True

    result = []
    for i in range(1, n + 1):
        t = i / n
        angle_rad = math.radians(a_start + (a_end - a_start) * t)
        x = cx + rx * math.cos(angle_rad)
        y = cy + ry * math.sin(angle_rad)
        x = max(-1.0, min(1.0, x))
        y = max(-1.0, min(1.0, y))
        result.append(PatternStep(
            type="move",
            laser=laser,
            x=round(x, 4),
            y=round(y, 4),
            duration_ms=seg_ms,
            raw={},
        ))
    return result
```

### Tests étape 2

```python
# tests/test_geometry.py

def test_expand_curve_point_count():
    """expand_curve retourne exactement n steps."""
    step = curve_step(x=0.5, y=0.0, cx=0.0, cy=0.5, duration_ms=2000, steps=10)
    result = expand_curve((0.0, 0.0), step)
    assert len(result) == 10

def test_expand_curve_all_move():
    step = curve_step(x=0.5, y=0.0, cx=0.0, cy=0.5, duration_ms=2000, steps=8)
    result = expand_curve((0.0, 0.0), step)
    assert all(s.type == "move" for s in result)

def test_expand_curve_last_point_is_destination():
    """Le dernier step arrive à la destination."""
    step = curve_step(x=0.6, y=-0.3, cx=0.2, cy=0.4, duration_ms=2000, steps=10)
    result = expand_curve((0.0, 0.0), step)
    assert abs(result[-1].x - 0.6) < 0.01
    assert abs(result[-1].y - (-0.3)) < 0.01

def test_expand_curve_coords_in_range():
    """Toutes les coordonnées restent dans -1.0..1.0."""
    step = curve_step(x=0.9, y=0.9, cx=1.5, cy=1.5, duration_ms=3000, steps=12)
    result = expand_curve((-0.9, -0.9), step)
    for s in result:
        assert -1.0 <= s.x <= 1.0
        assert -1.0 <= s.y <= 1.0

def test_expand_arc_full_circle_returns_to_start():
    """Cercle complet : dernier point ≈ premier point."""
    step = arc_step(cx=0.0, cy=0.0, rx=0.4, ry=0.4,
                    a_start=0, a_end=360, duration_ms=4000, steps=16)
    result = expand_arc(step)
    assert len(result) == 16
    assert abs(result[-1].x - result[0].x) < 0.02
    assert abs(result[-1].y - result[0].y) < 0.02

def test_expand_arc_semicircle_endpoint():
    """Demi-cercle : point final à (cx - rx, cy)."""
    step = arc_step(cx=0.0, cy=0.0, rx=0.3, ry=0.3,
                    a_start=0, a_end=180, duration_ms=2000, steps=8)
    result = expand_arc(step)
    assert abs(result[-1].x - (-0.3)) < 0.02
    assert abs(result[-1].y - 0.0) < 0.02

def test_expand_arc_ellipse():
    """Ellipse : vérification d'un point à 90°."""
    step = arc_step(cx=0.0, cy=0.0, rx=0.4, ry=0.2,
                    a_start=0, a_end=90, duration_ms=1000, steps=4)
    result = expand_arc(step)
    # à 90°, x ≈ 0, y ≈ radius_y = 0.2
    assert abs(result[-1].x - 0.0) < 0.02
    assert abs(result[-1].y - 0.2) < 0.02

def test_expand_step_duration_minimum():
    """Durée par step jamais inférieure à 80 ms."""
    step = curve_step(x=0.5, y=0.0, cx=0.0, cy=0.5, duration_ms=400, steps=20)
    result = expand_curve((0.0, 0.0), step)
    assert all(s.duration_ms >= 80 for s in result)
```

---

## Étape 3 — `expander.py` : nouveau fichier

```python
# tools/chatchat_patterns/expander.py

from __future__ import annotations

from .geometry import expand_arc, expand_curve
from .model import Pattern, PatternPack, PatternStep, POSITION_STEP_TYPES


def _current_pos(steps_so_far: list[PatternStep]) -> tuple[float, float]:
    """Retourne la dernière position connue dans la liste déjà traitée."""
    for step in reversed(steps_so_far):
        if step.type in POSITION_STEP_TYPES and step.x is not None:
            return float(step.x), float(step.y)
    return 0.0, 0.0


def expand_pattern(pattern: Pattern) -> Pattern:
    """
    Remplace chaque step curve/arc par une séquence de steps MOVE classiques.
    Retourne un nouveau Pattern — l'original n'est pas modifié.
    """
    expanded: list[PatternStep] = []

    for step in pattern.steps:
        if step.type == "curve":
            start = _current_pos(expanded)
            expanded.extend(expand_curve(start, step))
        elif step.type == "arc":
            expanded.extend(expand_arc(step))
        else:
            expanded.append(step)

    return Pattern(
        id=pattern.id,
        name=pattern.name,
        category=pattern.category,
        weight=pattern.weight,
        intensity=pattern.intensity,
        description=pattern.description,
        steps=expanded,
        raw=pattern.raw,
    )


def expand_pack(pack: PatternPack) -> PatternPack:
    """Applique expand_pattern() à chaque pattern du pack."""
    return PatternPack(
        schema=pack.schema,
        meta=pack.meta,
        coordinate_system=pack.coordinate_system,
        runtime=pack.runtime,
        patterns=[expand_pattern(p) for p in pack.patterns],
        raw=pack.raw,
    )
```

### Tests étape 3

```python
# tests/test_expander.py

def test_expand_pattern_passthrough():
    """Un pattern sans curve/arc est retourné identique."""
    pattern = pattern_with_moves()
    result = expand_pattern(pattern)
    assert len(result.steps) == len(pattern.steps)
    assert all(s.type in {"hold", "move", "jitter"} for s in result.steps)

def test_expand_pattern_curve_replaced():
    """Un step curve est remplacé par n steps move."""
    pattern = pattern_with_one_curve(steps=10)
    result = expand_pattern(pattern)
    assert all(s.type != "curve" for s in result.steps)
    # 1 hold initial + 10 move générés
    assert len(result.steps) == 11

def test_expand_pattern_arc_replaced():
    pattern = pattern_with_one_arc(steps=16)
    result = expand_pattern(pattern)
    assert all(s.type != "arc" for s in result.steps)
    assert len(result.steps) == 16

def test_expand_pattern_curve_start_is_previous_position():
    """La courbe part de la position du step précédent."""
    pattern = pattern_hold_then_curve(
        hold_x=0.3, hold_y=0.2,
        curve_dest_x=0.7, curve_dest_y=0.5,
        cx=-0.1, cy=0.8,
        steps=8
    )
    result = expand_pattern(pattern)
    # Le premier move généré doit être proche de (0.3, 0.2), pas de (0.0, 0.0)
    first_move = result.steps[1]
    # pas exactement le point de départ mais proche de lui
    assert abs(first_move.x - 0.3) < 0.1

def test_expand_pattern_preserves_metadata():
    """id, name, weight sont préservés après expansion."""
    pattern = pattern_with_one_curve()
    result = expand_pattern(pattern)
    assert result.id == pattern.id
    assert result.name == pattern.name
    assert result.weight == pattern.weight

def test_expand_pack_all_patterns_expanded():
    pack = pack_with_mixed_patterns()
    result = expand_pack(pack)
    for pattern in result.patterns:
        assert all(s.type not in {"curve", "arc"} for s in pattern.steps)

def test_expand_pack_original_unchanged():
    """L'objet pack original n'est pas muté."""
    pack = pack_with_mixed_patterns()
    original_step_counts = [len(p.steps) for p in pack.patterns]
    expand_pack(pack)
    assert [len(p.steps) for p in pack.patterns] == original_step_counts
```

---

## Étape 4 — `validator.py` : nouvelles règles

```python
# validator.py — dans validate_pack_data(), mettre à jour les imports
from .model import POSITION_STEP_TYPES, STEP_TYPES, ...

# Dans _validate_steps(), ajouter après la vérification du type :
if step_type == "curve":
    _validate_curve_step(step, spath, result)
elif step_type == "arc":
    _validate_arc_step(step, spath, result)

# Nouvelles fonctions :

MIN_POINT_DURATION_MS = 80   # contrainte binary.py

def _validate_curve_step(step: dict, spath: str, result: ValidationResult) -> None:
    for field in ("x", "y", "cx", "cy"):
        if not isinstance(step.get(field), (int, float)):
            result.errors.append(ValidationIssue(
                f"{spath}.{field}", f"champ requis pour curve"
            ))
    for field in ("cx", "cy"):
        v = step.get(field)
        if isinstance(v, (int, float)) and not (-1.5 <= float(v) <= 1.5):
            result.errors.append(ValidationIssue(
                f"{spath}.{field}", "point de controle hors -1.5..1.5"
            ))
    n = step.get("steps", 10)
    if isinstance(n, int) and n > 0:
        seg_ms = step.get("duration_ms", 0) // n
        if seg_ms < MIN_POINT_DURATION_MS:
            result.errors.append(ValidationIssue(
                spath,
                f"duration_ms / steps = {seg_ms} ms < minimum {MIN_POINT_DURATION_MS} ms "
                f"— augmenter duration_ms ou réduire steps"
            ))


def _validate_arc_step(step: dict, spath: str, result: ValidationResult) -> None:
    for field in ("cx", "cy", "radius_x", "radius_y", "angle_start_deg", "angle_end_deg"):
        if not isinstance(step.get(field), (int, float)):
            result.errors.append(ValidationIssue(
                f"{spath}.{field}", f"champ requis pour arc"
            ))
    rx = step.get("radius_x")
    ry = step.get("radius_y")
    if isinstance(rx, (int, float)) and float(rx) <= 0:
        result.errors.append(ValidationIssue(f"{spath}.radius_x", "rayon doit être > 0"))
    if isinstance(ry, (int, float)) and float(ry) <= 0:
        result.errors.append(ValidationIssue(f"{spath}.radius_y", "rayon doit être > 0"))
    # Avertissement si l'arc dépasse la zone visible
    cx = step.get("cx", 0)
    cy = step.get("cy", 0)
    if isinstance(rx, (int, float)) and isinstance(cx, (int, float)):
        if abs(float(cx)) + float(rx) > 1.05:
            result.warnings.append(ValidationIssue(spath, "arc déborde la zone visible sur X"))
    if isinstance(ry, (int, float)) and isinstance(cy, (int, float)):
        if abs(float(cy)) + float(ry) > 1.05:
            result.warnings.append(ValidationIssue(spath, "arc déborde la zone visible sur Y"))
    n = step.get("steps", 16)
    if isinstance(n, int) and n > 0:
        seg_ms = step.get("duration_ms", 0) // n
        if seg_ms < MIN_POINT_DURATION_MS:
            result.errors.append(ValidationIssue(
                spath,
                f"duration_ms / steps = {seg_ms} ms < minimum {MIN_POINT_DURATION_MS} ms"
            ))
```

---

## Étape 5 — `binary.py` : appel à `expand_pack()`

Une ligne à ajouter dans `binary_pack_from_pattern_pack()` :

```python
# binary.py
from .expander import expand_pack   # ← import

def binary_pack_from_pattern_pack(pack: PatternPack) -> BinaryPack:
    pack = expand_pack(pack)        # ← expansion avant sérialisation
    patterns = [_binary_pattern_from_pattern(pattern) for pattern in pack.patterns]
    return BinaryPack(patterns=patterns, checksum_crc32=0, file_size=0)
```

### Tests étape 5

```python
# tests/test_binary.py

def test_export_pack_with_curve_produces_valid_dat():
    """Un pack avec une curve s'exporte en .dat valide."""
    pack = pack_with_curve_step()
    binary = binary_pack_from_pattern_pack(pack)
    data = pack_binary(binary)
    restored = read_dat_bytes(data)
    assert len(restored.patterns) == 1
    # les points générés sont tous de type BinaryPoint classique
    for pt in restored.patterns[0].points:
        assert 0 <= pt.x <= 1000
        assert 0 <= pt.y <= 1000

def test_export_pack_with_arc_produces_valid_dat():
    pack = pack_with_arc_step(steps=12)
    binary = binary_pack_from_pattern_pack(pack)
    data = pack_binary(binary)
    restored = read_dat_bytes(data)
    assert len(restored.patterns[0].points) == 12

def test_export_classic_pack_unchanged():
    """Un pack sans primitives géométriques n'est pas altéré."""
    pack = classic_pack_with_moves()
    original_binary = binary_pack_from_pattern_pack(pack)
    # appeler deux fois doit donner le même résultat
    again = binary_pack_from_pattern_pack(pack)
    assert pack_binary(original_binary) == pack_binary(again)

def test_export_curve_respects_min_point_duration():
    """Les points générés respectent MIN_POINT_DURATION_MS."""
    pack = pack_with_curve_step(duration_ms=2000, steps=10)
    binary = binary_pack_from_pattern_pack(pack)
    for pt in binary.patterns[0].points:
        assert pt.duration_ms >= MIN_POINT_DURATION_MS
```

---

## Exemples JSON

### Cercle complet

```json
{
  "type": "arc",
  "laser": true,
  "cx": 0.0, "cy": 0.0,
  "radius_x": 0.4, "radius_y": 0.4,
  "angle_start_deg": 0, "angle_end_deg": 360,
  "duration_ms": 6000,
  "steps": 24
}
```

### S fluide

```json
[
  { "type": "hold", "laser": true, "x": -0.5, "y": 0.4, "duration_ms": 1500 },
  { "type": "curve", "laser": true,
    "x":  0.0, "y":  0.0, "cx":  0.5, "cy":  0.4, "duration_ms": 2500, "steps": 10 },
  { "type": "curve", "laser": true,
    "x":  0.5, "y": -0.4, "cx": -0.5, "cy": -0.4, "duration_ms": 2500, "steps": 10 }
]
```

### Ellipse horizontale

```json
{
  "type": "arc",
  "laser": true,
  "cx": 0.0, "cy": -0.1,
  "radius_x": 0.5, "radius_y": 0.2,
  "angle_start_deg": 0, "angle_end_deg": 360,
  "duration_ms": 5000,
  "steps": 20
}
```

---

 0);
    if (fd < 0) return;  /* fichier absent — défauts OK */

    app_settings_t tmp;
    s32_t n = SPIFFS_read(&store_fs, fd, &tmp, sizeof(tmp));
    SPIFFS_close(&store_fs, fd);

    if (n != (s32_t)sizeof(tmp)) return;
    if (tmp.magic != SETTINGS_MAGIC) return;
    if (tmp.version != SETTINGS_VERSION) return;
    uint32_t expected = settings_checksum(&tmp);
    if (tmp.checksum != expected) return;

    /* Valider les plages avant d'appliquer */
    if (tmp.speed_percent < PATTERN_SPEED_MIN_PERCENT ||
        tmp.speed_percent > PATTERN_SPEED_MAX_PERCENT) return;
    if (tmp.motion_interp_mode > 1) return;

    *out = tmp;
    PATTERN_LOG("settings loaded speed=%u interp=%u spiffs_pack=%u",
                (unsigned)out->speed_percent,
                (unsigned)out->motion_interp_mode,
                (unsigned)out->use_spiffs_pack);
}

bool pattern_store_save_settings(const app_settings_t *settings)
{
    if (!store_ready) return false;

    app_settings_t tmp = *settings;
    tmp.magic   = SETTINGS_MAGIC;
    tmp.version = SETTINGS_VERSION;
    tmp.checksum = settings_checksum(&tmp);

    return write_full_file(SETTINGS_FILE, (const uint8_t *)&tmp, sizeof(tmp));
}

bool pattern_store_load_patterns_dat(uint8_t *buf, uint32_t buf_len,
                                     uint32_t *out_len)
{
    *out_len = 0;
    if (!store_ready || buf == NULL) return false;

    spiffs_file fd = SPIFFS_open(&store_fs, PATTERN_STORE_FILE, SPIFFS_RDONLY, 0);
    if (fd < 0) return false;

    s32_t n = SPIFFS_read(&store_fs, fd, buf, (s32_t)buf_len);
    SPIFFS_close(&store_fs, fd);

    if (n <= 0) return false;
    *out_len = (uint32_t)n;
    return true;
}
```

---

## Étape D — `game.c` : boot + persistance

### Au boot (`game_init()` ou début de `game_movement_task`)

```c
/* game.c */

void game_init(void)
{
    /* 1. Charger les settings */
    app_settings_t settings;
    pattern_store_load_settings(&settings);
    speed_percent     = settings.speed_percent;
    motion_interp_mode = (motion_interp_t)settings.motion_interp_mode;

    /* 2. Si patterns.dat présent et settings l'indique, l'activer */
    if (settings.use_spiffs_pack && pattern_store_is_ready()) {
        static uint8_t dat_buf[PATTERN_STORE_MAX_UPLOAD_BYTES];
        uint32_t dat_len = 0;
        if (pattern_store_load_patterns_dat(dat_buf, sizeof(dat_buf), &dat_len)
            && dat_len > 0)
        {
            /* Réutiliser le chemin existant de chargement du pack SPIFFS */
            game_load_spiffs_pack(dat_buf, dat_len);
            PATTERN_LOG("boot: spiffs pack loaded bytes=%u", (unsigned)dat_len);
        }
    }
}
```

### À chaque changement de `speed_percent` (depuis l'API web)

```c
void game_set_speed(uint16_t percent)
{
    speed_percent = percent;
    /* Persister immédiatement */
    app_settings_t s;
    pattern_store_load_settings(&s);   /* charge pour ne pas écraser les autres champs */
    s.speed_percent = percent;
    pattern_store_save_settings(&s);
}
```

Même logique pour `motion_interp_mode` et `use_spiffs_pack`.

> **Remarque** : `pattern_store_load_settings()` avant chaque `save` évite
> d'écraser un champ modifié par un autre code path. L'alternative est de
> maintenir une structure `current_settings` globale dans `game.c`.

---

## Étape E — Tests firmware (à valider sur cible)

Les tests unitaires purs ne sont pas possibles sur RTOS SDK sans émulation.
Les scénarios à valider manuellement :

```
[T1] Premier boot sans settings.dat
     → speed = 100, interp = smoothstep, pack = défaut compilé
     → vérifier via page web

[T2] Changer speed_percent à 150 via web → reboot
     → speed doit être 150 au redémarrage

[T3] Changer motion_interp à linear via web → reboot
     → mode doit être linear

[T4] Uploader patterns.dat via web → reboot
     → pack SPIFFS doit être actif (vérifier pattern_status dans /patterns)

[T5] Corrompre settings.dat manuellement (écrire des 0xFF)
     → reboot doit appliquer les valeurs par défaut sans crash

[T6] Effacer patterns.dat → reboot
     → pack compilé par défaut doit être actif

[T7] Coupure de courant pendant écriture settings (simuler en débranchant)
     → au boot suivant : soit settings valides, soit défauts
     → jamais de crash
```

---

## Résumé des fichiers modifiés

### Chantier 1 — Python

| Fichier | Lignes estimées | Risque |
|---|---|---|
| `model.py` | +15 | faible |
| `geometry.py` | +60 | faible |
| `expander.py` | +45 (nouveau) | faible |
| `validator.py` | +50 | moyen — ne pas casser les règles existantes |
| `binary.py` | +2 | très faible |

### Chantier 2 — Firmware

| Fichier | Lignes estimées | Risque |
|---|---|---|
| `pattern_store.h` | +25 | faible |
| `pattern_store.c` | +80 | moyen — tester CRC et cas d'erreur SPIFFS |
| `game.c` | +30 | moyen — ordre d'init critique |

### Pas touchés

- Firmware complet hors `game.c` et `pattern_store.*`
- Format `.dat` (version 1 inchangée)
- `web.c`
- `hardware.c`
