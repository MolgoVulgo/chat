# Format `patterns.dat` v2

## 1. Rôle du fichier

`patterns.dat` est le fichier binaire runtime contenant les patterns de déplacement du laser.

Il remplace totalement les anciens formats :

```text
JSON        abandonné
v1          abandonné
v2          seul format valide
```

Le fichier est conçu pour être lu directement par l’ESP sans parser lourd et sans chargement complet en RAM.

Principe :

```text
ouvrir le fichier
lire le header
lire l’index
lire uniquement les points du pattern joué
```

---

## 2. Organisation générale

Le fichier est composé de trois blocs :

```text
patterns.dat
├── header
├── index des patterns
└── données des points
```

Structure logique :

```text
[HEADER][INDEX ENTRY 0][INDEX ENTRY 1][...][POINTS PATTERN 0][POINTS PATTERN 1][...]
```

Le header donne les informations globales.

L’index permet de trouver rapidement les patterns sans scanner tout le fichier.

Le bloc de données contient les points de mouvement, stockés les uns à la suite des autres.

---

## 3. Endianness

Le format est strictement little-endian.

```text
endian = 1
```

Aucun autre endianness n’est accepté.

---

## 4. Header

Le header est au début du fichier.

Taille fixe :

```text
48 octets
```

Structure :

```c
typedef struct __attribute__((packed)) {
    char     magic[4];          // "LPTN"
    uint8_t  version;           // 2
    uint8_t  endian;            // 1 = little endian
    uint16_t header_size;       // 48

    uint32_t file_size;         // taille totale du fichier
    uint32_t header_crc32;      // CRC32 du header
    uint32_t payload_crc32;     // CRC32 index + points

    uint32_t index_offset;      // début de l’index
    uint16_t pattern_count;     // nombre de patterns
    uint16_t index_entry_size;  // 48

    uint32_t data_offset;       // début des points
    uint16_t point_size;        // 10
    uint16_t flags;             // options globales

    uint32_t schema_hash;       // version logique du schéma/générateur
    uint32_t reserved[2];       // 0
} lptn_header_v2_t;
```

Valeurs attendues :

```text
magic            = "LPTN"
version          = 2
endian           = 1
header_size      = 48
index_offset     = 48
index_entry_size = 48
point_size       = 10
```

Le firmware doit rejeter le fichier si ces valeurs ne correspondent pas.

---

## 5. Index des patterns

L’index commence à l’offset indiqué par :

```text
header.index_offset
```

Chaque entrée d’index décrit un pattern.

Taille fixe d’une entrée :

```text
48 octets
```

Structure :

```c
typedef struct __attribute__((packed)) {
    char     name[24];          // nom du pattern

    uint32_t point_offset;      // offset absolu du premier point
    uint32_t duration_ms;       // durée totale du pattern
    uint32_t pattern_crc32;     // CRC32 des points du pattern

    uint16_t point_count;       // nombre de points
    uint8_t  weight;            // poids de sélection
    uint8_t  flags;             // flags du pattern

    uint16_t x_min;             // bounding box
    uint16_t x_max;
    uint16_t y_min;
    uint16_t y_max;
} lptn_index_entry_v2_t;
```

L’index sert à :

```text
lister les patterns
afficher leurs noms
connaître leur durée
connaître leur nombre de points
trouver directement les points via point_offset
sélectionner un pattern sans lire tout le fichier
```

---

## 6. Nom du pattern

Le champ `name` fait 24 octets.

Règle :

```text
23 caractères maximum + terminaison \0
```

Si le nom est plus court, le reste est rempli avec des zéros.

Exemple :

```text
slow_floor_mouse\0\0\0...
```

Le firmware doit toujours recopier le nom dans un buffer local et forcer la terminaison :

```c
char name[25];
memcpy(name, entry.name, 24);
name[24] = '\0';
```

---

## 7. Offset des points

Chaque pattern pointe vers ses points avec :

```text
point_offset
```

Cet offset est absolu depuis le début du fichier.

Exemple :

```text
point_offset = 672
```

Signifie :

```text
le premier point du pattern commence à l’octet 672 du fichier
```

Le firmware peut donc faire :

```text
seek(point_offset)
read(point)
read(point)
read(point)
...
```

Sans lire les autres patterns.

---

## 8. Bloc de points

Le bloc de points commence à :

```text
header.data_offset
```

Tous les points sont stockés en continu.

Les points d’un pattern sont contigus.

Exemple :

```text
pattern 0 : points 0 à 24
pattern 1 : points 25 à 48
pattern 2 : points 49 à 74
```

Mais le firmware n’a pas besoin de connaître cette organisation globale : il utilise `point_offset` et `point_count`.

---

## 9. Format d’un point

Taille fixe :

```text
10 octets
```

Structure :

```c
typedef struct __attribute__((packed)) {
    uint16_t x;              // coordonnée X normalisée
    uint16_t y;              // coordonnée Y normalisée
    uint16_t duration_ms;    // durée du mouvement ou de la pause

    uint8_t  laser;          // état du laser
    uint8_t  action;         // type d’action
    uint16_t arg;            // argument optionnel
} lptn_point_v2_t;
```

---

## 10. Coordonnées

Les coordonnées sont normalisées.

Plage :

```text
0..1000
```

Interprétation :

```text
x = 0      gauche
x = 500    centre
x = 1000   droite

y = 0      haut
y = 500    centre
y = 1000   bas
```

Le fichier ne contient pas d’angles servo.

Le mapping vers les servos est fait côté firmware :

```text
coordonnées normalisées -> angle servo réel
```

---

## 11. Durée

Chaque point contient :

```text
duration_ms
```

Ce champ indique la durée en millisecondes.

Exemples :

```text
500   = 0,5 seconde
1000  = 1 seconde
3000  = 3 secondes
```

La durée totale du pattern est stockée dans l’index :

```text
duration_ms = somme des duration_ms de tous les points
```

---

## 12. Laser

Champ :

```text
laser
```

Valeurs :

```text
0 = laser éteint
1 = laser allumé
```

Pour certaines actions, le laser est forcé à `0`.

---

## 13. Actions

Champ :

```text
action
```

Valeurs standardisées :

```c
#define LPTN_ACTION_HOLD      0
#define LPTN_ACTION_MOVE      1
#define LPTN_ACTION_JITTER    2
#define LPTN_ACTION_OFF_HOLD  3
#define LPTN_ACTION_OFF_MOVE  4
```

### `HOLD`

Pause sur place.

```text
servo : ne bouge pas
laser : selon champ laser
arg   : 0
```

### `MOVE`

Déplacement visible.

```text
servo : va vers x/y
laser : selon champ laser
arg   : 0
```

### `JITTER`

Micro-mouvement autour d’une position.

```text
servo : bouge légèrement autour de x/y
laser : généralement allumé
arg   : amplitude du jitter
```

### `OFF_HOLD`

Pause invisible.

```text
servo : ne bouge pas
laser : éteint
arg   : 0
```

### `OFF_MOVE`

Déplacement invisible.

```text
servo : va vers x/y
laser : éteint
arg   : 0
```

---

## 14. Champ `arg`

Le champ `arg` est un argument optionnel.

Usage actuel :

```text
JITTER : amplitude du jitter
autres actions : 0
```

Règle :

```text
arg doit être à 0 sauf pour LPTN_ACTION_JITTER
```

---

## 15. Flags globaux

Champ :

```text
header.flags
```

Flags prévus :

```c
#define LPTN_GLOBAL_FLAG_HAS_CRC       0x0001
#define LPTN_GLOBAL_FLAG_STRICT_BOUNDS 0x0002
```

`HAS_CRC` indique que les CRC sont présents et doivent être considérés valides.

`STRICT_BOUNDS` indique que les coordonnées doivent rester strictement dans la plage `0..1000`.

---

## 16. Flags de pattern

Champ :

```text
entry.flags
```

Flags prévus :

```c
#define LPTN_PATTERN_FLAG_DISABLED 0x01
#define LPTN_PATTERN_FLAG_CAPTURE  0x02
#define LPTN_PATTERN_FLAG_FINAL    0x04
```

Interprétation :

```text
DISABLED : pattern présent mais non jouable
CAPTURE  : pattern de capture/récompense
FINAL    : pattern de fin ou retour au calme
```

---

## 17. Bounding box

Chaque entrée d’index contient :

```text
x_min
x_max
y_min
y_max
```

Cela décrit la zone couverte par le pattern.

Exemple :

```text
x_min = 150
x_max = 850
y_min = 300
y_max = 700
```

Utilité :

```text
filtrage
scaling
diagnostic
affichage rapide
vérification sans relire tous les points
```

---

## 18. CRC

Le format prévoit trois niveaux de CRC.

### Header CRC

Champ :

```text
header_crc32
```

CRC calculé sur le header, avec le champ `header_crc32` temporairement mis à `0`.

### Payload CRC

Champ :

```text
payload_crc32
```

CRC calculé sur :

```text
index + points
```

### Pattern CRC

Champ :

```text
pattern_crc32
```

CRC calculé uniquement sur les points du pattern concerné.

Le générateur doit toujours calculer les CRC.

Le firmware peut les vérifier progressivement selon le niveau de robustesse souhaité.

---

## 19. Calcul des offsets

Formule générale :

```text
header_size      = 48
index_entry_size = 48
point_size       = 10

index_offset = 48
data_offset  = index_offset + pattern_count × index_entry_size
```

Exemple avec 13 patterns :

```text
index_offset = 48
index_size   = 13 × 48 = 624
data_offset  = 48 + 624 = 672
```

Le premier point du premier pattern commencera donc à l’octet :

```text
672
```

---

## 20. Lecture rapide de la liste

Pour afficher la liste des patterns, l’ESP lit seulement :

```text
header
index entries
```

Avec 13 patterns :

```text
header = 48 octets
index  = 624 octets
total  = 672 octets
```

Les points ne sont pas lus.

---

## 21. Lecture d’un pattern

Procédure :

```text
1. lire le header
2. lire l’entrée d’index du pattern
3. récupérer point_offset
4. récupérer point_count
5. seek vers point_offset
6. lire point par point
```

Pseudo-code :

```text
seek(entry.point_offset)

for i in 0..entry.point_count-1:
    read point
    exécuter point
```

RAM minimale :

```text
header      : 48 octets
index entry : 48 octets
point       : 10 octets
```

---

## 22. Conditions de rejet du fichier

Le firmware doit rejeter le fichier si :

```text
magic != "LPTN"
version != 2
endian != 1
header_size != 48
index_entry_size != 48
point_size != 10
file_size incorrect
pattern_count == 0
index_offset incorrect
data_offset incorrect
offset d’un pattern hors fichier
point_count invalide
flags réservés utilisés
```

Aucune tentative de compatibilité v1.

---

## 23. Résumé du format

```text
format       : binaire
magic        : LPTN
version      : 2
endianness   : little-endian
header       : 48 octets
index entry  : 48 octets
point        : 10 octets
coordonnées  : normalisées 0..1000
lecture      : streaming
JSON         : non
v1           : non supportée
```

Objectif :

```text
lecture rapide
RAM minimale
pas de parser lourd
offsets directs
patterns.dat = vérité runtime
```
