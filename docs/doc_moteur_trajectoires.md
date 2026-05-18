# Moteur de trajectoires fluides — Documentation fonctionnelle

## Contexte architectural

L'ESP reçoit des séquences de steps `STEP_MOVE` et les exécute en interpolant
les coordonnées entre le point courant et la destination. Il ne connaît pas la
notion de courbe ou d'arc — c'est intentionnel.

Tout le calcul de trajectoire est fait **côté Python**, qui génère des séquences
de points rapprochés à partir de primitives géométriques de haut niveau. L'ESP
exécute ces points sans en connaître l'origine.

```
JSON (primitives haut niveau)
        │
        ▼
  Python toolchain
  ┌─────────────────────────────────────┐
  │  expand_curves()                    │
  │  Bezier → N steps MOVE              │
  │  Arc     → N steps MOVE             │
  │  Spirale → N steps MOVE             │
  └─────────────────────────────────────┘
        │
        ▼
  patterns.dat (points MOVE classiques)
        │
        ▼
  ESP firmware (inchangé)
  interpolation point à point
```

---

## Partie Python — chatchat_patterns

### 1. Nouveaux types de steps JSON

Les primitives géométriques sont exprimées dans le JSON source. Elles n'existent
pas dans le format binaire `.dat` — elles sont toujours transformées en steps
`MOVE` classiques par l'outil avant export.

#### 1.1 `curve` — Bezier quadratique

Trajectoire courbée définie par un point de départ implicite (position courante),
un point de contrôle, et une destination.

```json
{
  "type": "curve",
  "laser": true,
  "x": 0.4,
  "y": 0.1,
  "cx": 0.6,
  "cy": -0.5,
  "duration_ms": 3200,
  "steps": 12
}
```

| Champ | Description |
|---|---|
| `x`, `y` | Destination finale (coordonnées normalisées -1.0..1.0) |
| `cx`, `cy` | Point de contrôle de la courbe |
| `duration_ms` | Durée totale du mouvement |
| `steps` | Nombre de segments générés (optionnel, défaut 10) |

**Formule Bezier quadratique** pour t ∈ [0, 1] :

```
B(t) = (1-t)² · P0  +  2·(1-t)·t · P1  +  t² · P2

P0 = position courante (départ)
P1 = (cx, cy) point de contrôle
P2 = (x, y)   destination
```

Chaque segment généré a une durée de `duration_ms / steps` ms.

**Exemples de formes obtenues :**

```
P1 au-dessus du segment P0→P2 : arc convexe vers le haut
P1 très décalé latéralement    : virage prononcé
Deux curves consécutives        : S fluide
```

#### 1.2 `arc` — Arc de cercle ou ellipse

Portion de cercle centrée sur `cx`, `cy`.

```json
{
  "type": "arc",
  "laser": true,
  "cx": 0.0,
  "cy": 0.0,
  "radius_x": 0.4,
  "radius_y": 0.3,
  "angle_start_deg": 0,
  "angle_end_deg": 360,
  "duration_ms": 6000,
  "steps": 24
}
```

| Champ | Description |
|---|---|
| `cx`, `cy` | Centre de l'arc |
| `radius_x` | Rayon horizontal (cercle si égal à `radius_y`) |
| `radius_y` | Rayon vertical |
| `angle_start_deg` | Angle de départ en degrés (0 = droite, sens trigonométrique) |
| `angle_end_deg` | Angle de fin. 360 = cercle complet |
| `duration_ms` | Durée totale |
| `steps` | Nombre de segments (défaut 16) |

**Exemples de formes :**

```
angle_start=0, angle_end=360, radius_x=radius_y  → cercle complet
angle_start=0, angle_end=180                      → demi-cercle
radius_x ≠ radius_y                               → ellipse
Deux arcs avec centres différents                 → figure 8
Arc + radius croissant en steps successifs        → spirale
```

#### 1.3 `lemniscate` — Figure 8 (sucre syntaxique)

Génère automatiquement deux arcs formant une lemniscate de Bernoulli.
Évite d'écrire deux arcs manuellement.

```json
{
  "type": "lemniscate",
  "laser": true,
  "cx": 0.0,
  "cy": 0.0,
  "radius": 0.35,
  "duration_ms": 8000,
  "steps": 32
}
```

---

### 2. Module `geometry.py` — extensions

Le fichier `geometry.py` existant contient `pattern_segments()` et les
primitives de base. On y ajoute les fonctions de développement de courbes.

#### 2.1 `expand_curve()`

```python
def expand_curve(
    start: tuple[float, float],
    end: tuple[float, float],
    control: tuple[float, float],
    duration_ms: int,
    n_steps: int,
    laser: bool,
) -> list[PatternStep]:
    """
    Génère n_steps PatternStep de type 'move' le long d'une Bezier quadratique.
    Chaque step a une durée de duration_ms // n_steps ms.
    """
```

#### 2.2 `expand_arc()`

```python
def expand_arc(
    cx: float,
    cy: float,
    radius_x: float,
    radius_y: float,
    angle_start_deg: float,
    angle_end_deg: float,
    duration_ms: int,
    n_steps: int,
    laser: bool,
) -> list[PatternStep]:
    """
    Génère n_steps PatternStep de type 'move' le long d'un arc d'ellipse.
    Les angles sont en degrés, sens trigonométrique.
    """
```

#### 2.3 `expand_lemniscate()`

```python
def expand_lemniscate(
    cx: float,
    cy: float,
    radius: float,
    duration_ms: int,
    n_steps: int,
    laser: bool,
) -> list[PatternStep]:
    """
    Génère n_steps PatternStep formant une figure 8.
    Implémenté comme deux arcs de demi-cercle décalés.
    """
```

---

### 3. Module `expander.py` — nouveau fichier

Responsable de la transformation d'un `Pattern` contenant des primitives
géométriques en `Pattern` ne contenant que des steps classiques.

```python
def expand_pattern(pattern: Pattern) -> Pattern:
    """
    Parcourt les steps du pattern.
    Pour chaque step de type curve/arc/lemniscate :
      - calcule la position de départ (position courante)
      - appelle la fonction expand_* correspondante
      - remplace le step par la liste de steps MOVE générés
    Retourne un nouveau Pattern avec uniquement des types classiques.
    """

def expand_pack(pack: PatternPack) -> PatternPack:
    """
    Applique expand_pattern() à chaque pattern du pack.
    """
```

**Règle de position de départ :** la position courante au moment d'un step
`curve` ou `arc` est la dernière coordonnée explicite des steps précédents,
exactement comme le fait déjà `pattern_segments()`.

---

### 4. Intégration dans le pipeline existant

#### 4.1 Export `.dat`

Dans `binary.py`, `binary_pack_from_pattern_pack()` appelle `expand_pack()`
avant de sérialiser :

```python
def binary_pack_from_pattern_pack(pack: PatternPack) -> BinaryPack:
    expanded = expand_pack(pack)           # ← ajout
    patterns = [_binary_pattern_from_pattern(p) for p in expanded.patterns]
    ...
```

Le format binaire `.dat` ne change pas. Les nouveaux types n'y apparaissent
jamais.

#### 4.2 Validation

Dans `validator.py`, mettre à jour `STEP_TYPES` et `POSITION_STEP_TYPES` :

```python
STEP_TYPES = {"hold", "move", "jitter", "off_hold", "off_move",
              "curve", "arc", "lemniscate"}

# curve/arc/lemniscate ont une destination finale à valider
POSITION_STEP_TYPES = {"hold", "move", "jitter", "off_move", "curve"}
```

Règles de validation à ajouter :

| Type | Champs requis | Contraintes |
|---|---|---|
| `curve` | `x`, `y`, `cx`, `cy` | `cx`, `cy` dans -1.5..1.5 (hors plage visible autorisé) |
| `arc` | `cx`, `cy`, `radius_x`, `radius_y`, `angle_start_deg`, `angle_end_deg` | rayons > 0, angles valides |
| `lemniscate` | `cx`, `cy`, `radius` | radius > 0 et < 0.6 |

Le validateur calcule également le nombre de steps après expansion pour vérifier
que `MAX_TOTAL_STEPS` n'est pas dépassé.

#### 4.3 Modèle — `model.py`

Ajouter les champs optionnels à `PatternStep` :

```python
@dataclass(frozen=True)
class PatternStep:
    type: str
    duration_ms: int
    laser: bool | None = None
    x: float | None = None
    y: float | None = None
    amplitude: float | None = None
    # nouveaux champs pour les primitives géométriques
    cx: float | None = None
    cy: float | None = None
    radius_x: float | None = None
    radius_y: float | None = None
    radius: float | None = None
    angle_start_deg: float | None = None
    angle_end_deg: float | None = None
    steps: int | None = None
    raw: dict[str, Any] = field(default_factory=dict)
```

---

### 5. Choix du nombre de steps

Le paramètre `steps` est optionnel. Valeurs par défaut recommandées :

| Type | Défaut | Logique |
|---|---|---|
| `curve` | 10 | suffisant pour une courbe douce |
| `arc` < 90° | 6 | peu de points pour un petit arc |
| `arc` 360° | 24 | cercle fluide |
| `lemniscate` | 32 | deux demi-cercles × 16 |

Règle : l'ESP interpole entre chaque point avec smoothstep, donc 8 à 16 points
donnent déjà une courbe visuellement fluide. Au-delà de 24 points par primitive,
le gain est imperceptible sur un servo.

---

### 6. Contraintes de vitesse après expansion

Le validateur doit vérifier la vitesse entre points **après expansion**, pas
avant. Une courbe avec `duration_ms=500` et `steps=20` génère des segments de
25 ms chacun — la vitesse par segment peut être acceptable même si la distance
totale est grande.

```python
# dans validator.py, après expand_pattern()
for segment in pattern_segments(expanded_pattern):
    speed = relative_speed(segment)
    if speed > HARD_MAX_RELATIVE_SPEED:
        result.errors.append(...)
```

---

## Partie ESP — firmware

### Aucun changement requis

Le firmware reçoit des steps `MOVE` classiques et les exécute. Il n'a pas
connaissance des primitives curve/arc. L'interpolation smoothstep existante
entre deux points consécutifs proches produit naturellement la trajectoire
fluide voulue.

### Paramètre `steps` et fréquence de mise à jour servo

La fréquence de mise à jour des servos (`MOTION_TICK_MS`) détermine la
résolution d'interpolation **à l'intérieur** d'un step. Pour des steps de 25 ms
générés par expansion (arc en 24 steps sur 600 ms), le firmware exécutera
environ 2 ticks par step — ce qui est suffisant.

Si des steps très courts (< 80 ms) sont nécessaires pour une primitive très
rapide, il faut réduire la durée totale plutôt que d'augmenter `steps`, afin de
rester dans les plages validées par `MIN_POINT_DURATION_MS`.

### Contrainte `MIN_POINT_DURATION_MS = 80 ms`

Limite existante dans `binary.py`. Elle s'applique à chaque step généré par
expansion. La durée par step générée est :

```
durée_step = duration_ms / n_steps

ex: arc duration_ms=2000, steps=20 → 100 ms par step ✓
ex: arc duration_ms=800,  steps=20 → 40 ms par step  ✗ refusé à l'export
```

Le validateur doit vérifier cette contrainte après expansion et remonter une
erreur explicite avec suggestion de réduire `steps` ou augmenter `duration_ms`.

---

## Exemples de patterns utilisant les nouvelles primitives

### Cercle complet lent

```json
{
  "id": "circle_slow",
  "name": "Cercle lent",
  "weight": 10,
  "steps": [
    { "type": "hold", "laser": true, "x": 0.4, "y": 0.0, "duration_ms": 2000 },
    {
      "type": "arc",
      "laser": true,
      "cx": 0.0, "cy": 0.0,
      "radius_x": 0.4, "radius_y": 0.4,
      "angle_start_deg": 0, "angle_end_deg": 360,
      "duration_ms": 6000,
      "steps": 24
    }
  ]
}
```

Après expansion : 1 hold + 24 move = 25 steps classiques.

### S fluide

```json
{
  "id": "s_curve",
  "name": "S fluide",
  "weight": 8,
  "steps": [
    { "type": "hold", "laser": true, "x": -0.5, "y": 0.5, "duration_ms": 1800 },
    {
      "type": "curve",
      "laser": true,
      "x": 0.0, "y": 0.0,
      "cx": 0.5, "cy": 0.5,
      "duration_ms": 3000, "steps": 12
    },
    {
      "type": "curve",
      "laser": true,
      "x": 0.5, "y": -0.5,
      "cx": -0.5, "cy": -0.5,
      "duration_ms": 3000, "steps": 12
    }
  ]
}
```

Après expansion : 1 hold + 24 move = 25 steps classiques.

### Figure 8

```json
{
  "id": "figure_eight",
  "name": "Figure 8",
  "weight": 6,
  "steps": [
    {
      "type": "lemniscate",
      "laser": true,
      "cx": 0.0, "cy": 0.0,
      "radius": 0.3,
      "duration_ms": 8000,
      "steps": 32
    }
  ]
}
```

Après expansion : 32 steps move classiques.

---

## Résumé des fichiers à modifier ou créer

| Fichier | Action |
|---|---|
| `model.py` | Ajouter les champs optionnels à `PatternStep` |
| `geometry.py` | Ajouter `expand_curve()`, `expand_arc()`, `expand_lemniscate()` |
| `expander.py` | Créer — `expand_pattern()`, `expand_pack()` |
| `validator.py` | Mettre à jour `STEP_TYPES`, ajouter validation curve/arc/lemniscate, vérifier contraintes après expansion |
| `binary.py` | Appeler `expand_pack()` avant sérialisation |
| Firmware ESP | **Aucun changement** |
