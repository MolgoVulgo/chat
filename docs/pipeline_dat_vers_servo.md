# Pipeline `patterns.dat` -> Servo (analyse moteur de rendu)

## Objet
Ce document décrit précisément comment le firmware interprète un `patterns.dat`, convertit ses instructions en coordonnées internes, puis pilote les servos.

Portée code actuelle :
- `src/pattern_dat.c`
- `src/pattern_store.c`
- `src/game.c`
- `src/hardware.c`

---

## 1. Vue d'ensemble du flux

1. Upload du fichier `patterns.dat` via web (`/patterns/upload`) vers SPIFFS.
2. Chargement du pack actif (`pattern_store_load_active_pack`) :
- lecture/validation du header + index + CRC via `pattern_dat_load`
- conservation des métadonnées patterns en RAM (`id`, `weight`, `step_count`)
- conservation des offsets de points DAT (`pattern_point_offsets[]`, `pattern_point_counts[]`)
3. En exécution (`game_movement_task`) :
- sélection du pattern (manuel / capture / pondéré)
- chargement à la demande des steps du pattern choisi (`pattern_store_load_pattern_by_index`)
- exécution step par step (`run_pattern`)
4. Conversion coordonnée -> angle servo -> impulsions PWM.

---

## 2. Format `patterns.dat` interprété

## 2.1 Header (48 octets)
Champs validés dans `pattern_dat_load` :
- `magic` = `0x4E54504C`
- `version` = `2`
- `endian` = little-endian
- `header_size` = `48`
- `file_size` cohérent
- `index_offset` = `48`
- `index_size` = `48`
- `point_size` = `10`
- `data_offset = index_offset + pattern_count * 48`
- CRC header/payload valides

Si une condition échoue : pack rejeté.

## 2.2 Index pattern (48 octets / pattern)
Pour chaque pattern, l'index fournit notamment :
- `id` (24 octets)
- `point_offset` (offset absolu dans le fichier)
- `point_count`
- `weight`

L'index est validé contre :
- bornes fichier
- `PATTERN_MAX_PATTERNS`
- `PATTERN_MAX_TOTAL_STEPS`

## 2.3 Point (10 octets / step)
Décodage dans `pattern_store_load_pattern_by_index` :
- `x_u16`, `y_u16` (0..1000)
- `duration_ms`
- `laser` (0/1)
- `action` (0..4)
- `arg` (amplitude jitter)

Mapping `action` -> `pattern_step_type_t` :
- `0` -> `STEP_HOLD`
- `1` -> `STEP_MOVE`
- `2` -> `STEP_JITTER`
- `3` -> `STEP_OFF_HOLD`
- `4` -> `STEP_OFF_MOVE`

Validation runtime point :
- `x,y <= 1000`
- `duration` dans `[PATTERN_MIN_DURATION_MS, 10000]`
- `laser` booléen
- `action` bornée

---

## 3. Conversions de coordonnées

## 3.1 DAT -> coordonnées internes moteur
Dans `pattern_store.c` :
- `x_internal = x_u16 * 2 - 1000`
- `y_internal = 1000 - y_u16 * 2`

Donc :
- `x_internal` dans `[-1000, +1000]`
- `y_internal` dans `[-1000, +1000]` avec inversion axe vertical.

## 3.2 Coordonnées moteur -> angle servo
Dans `hardware.c` (`coord_to_angle`) :
- clamp `coord` à `[-1000, 1000]`
- inversion éventuelle par axe (`SERVO_*_INVERT`)
- projection linéaire vers `[SERVO_*_MIN, SERVO_*_MAX]`

Formule :
- `angle = min + ((coord + 1000) * (max - min)) / 2000`

## 3.3 Angle servo -> largeur impulsion
Toujours dans `hardware.c` (`servo_angle_to_us`) :
- clamp angle `[0,180]`
- `pulse_us = SERVO_MIN_US + ((SERVO_MAX_US - SERVO_MIN_US) * angle) / 180`

---

## 4. Exécution temporelle moteur

## 4.1 Sélection pattern
Dans `game_movement_task` :
- manuel si `selected_pattern_index` valide
- sinon capture périodique (`capture_every`)
- sinon tirage pondéré par `weight`

## 4.2 Chargement à la demande
Si le pack actif vient de SPIFFS (`pattern_store_is_active_pack`) :
- lecture du pattern sélectionné via `pattern_store_load_pattern_by_index`
- remplissage d'un buffer `runtime_steps` (réalloué si besoin)

## 4.3 Exécution des steps
`run_pattern` appelle :
- `prepare_pattern_start` (transition vers le 1er point utile)
- puis `run_step` pour chaque step

Types :
- `HOLD` : maintien position
- `MOVE` : interpolation `smoothstep`
- `JITTER` : micro-cibles autour du centre
- `OFF_HOLD` : laser OFF + attente
- `OFF_MOVE` : déplacement invisible

Tick moteur : `MOTION_TICK_MS`.
PWM servo : `SERVO_PERIOD_MS`.

---

## 5. Pilotage servo réel

Task dédiée `hardware_servo_pwm_task` :
- lit `servo_h_pos` / `servo_v_pos`
- envoie impulsions GPIO (H puis V)
- cycle toutes les `SERVO_PERIOD_MS` (20 ms)

Le moteur de jeu ne génère pas directement la PWM :
- il écrit la consigne de position
- la task hardware applique la consigne périodiquement.

---

## 6. Points sensibles pouvant expliquer un rendu incorrect

## 6.1 Problème probable : mélange coordonnées scalées / non scalées
Code actuel dans `game.c` :
- `set_game_position()` applique `motion_scale_percent`
- mais les interpolations utilisent parfois `current_x/current_y` (déjà scalés) vers `step->x/step->y` (non scalés)

Exemple :
- `from_x = current_x` (scalé)
- `to_x = step->x` (non scalé)
- interpolation directe entre les deux

Impact :
- trajectoires déformées,
- effets de "drift" / non-linéarité,
- rendu instable quand `scale != 100`.

Correction recommandée :
- maintenir une position logique non scalée (ex: `logical_x/y`) pour tout calcul,
- appliquer le scale uniquement juste avant `hardware_set_position_from_coord`.

## 6.2 `duration_ms` plafonné à 10000 ms côté DAT runtime
Même si les outils en amont permettent plus, le runtime rejette >10s.

## 6.3 `OFF_MOVE` réutilise `run_move_step`
Le laser est tenu OFF via `step_runtime_laser_on`, OK, mais si un code externe force laser ON en parallèle, l'effet visible peut diverger.

## 6.4 Écart de timing entre task mouvement et task PWM
- Moteur : 20 ms par tick.
- PWM : 20 ms par période.
- Si charge CPU élevée, micro-jitter possible.

---

## 7. Vérifications terrain recommandées

1. Ajouter logs ponctuels sur 1 pattern court :
- `from_x/from_y`, `to_x/to_y`, `current_x/current_y`, `motion_scale_percent`.
2. Tester même pattern avec `scale=100`, `80`, `50` et comparer trajectoire.
3. Vérifier cohérence axe Y (inversion DAT + inversion servo éventuelle).
4. Vérifier limites servo `SERVO_*_MIN/MAX` (saturation peut "casser" la forme).

---

## 8. Résumé exécutif

Le parsing DAT et la conversion vers servo sont globalement cohérents.
Le principal risque de rendu incorrect actuel est la gestion du `scale` dans `game.c` (application trop tôt, puis interpolation mixte scalé/non-scalé).

Priorité de correction : séparer clairement
- trajectoire logique (non scalée)
- projection mécanique (scalée).
