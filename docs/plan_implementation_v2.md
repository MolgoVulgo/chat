# Plan d'implementation V2

## Objectif

Mettre en place la V2 decrite dans `docs/v_2.md` sans casser le
fonctionnement actuel : le firmware doit toujours demarrer avec des patterns
embarques, puis charger optionnellement des patterns utilisateur depuis
LittleFS au format compact.

La priorite reste :

1. securite laser et arret propre ;
2. boot fiable sans fichier ;
3. moteur temps reel simple ;
4. validation forte cote Python ;
5. migration progressive depuis le pack JSON compile actuel.

## Decisions de base

- Extension runtime : `patterns.dat`.
- Format : binaire versionne `LPTN`, version `1`.
- Checksum : CRC32 sur le fichier complet avec le champ checksum mis a zero.
- Coordonnees V2 : `uint16_t` de `0` a `1000`.
- Firmware : aucun parsing JSON pour les patterns.
- Python : conserve le JSON source, valide, exporte et inspecte `patterns.dat`.
- Fallback : 5 patterns embarques toujours disponibles.
- Remplacement : un pattern utilisateur remplace un builtin s'il a le meme ID,
  sinon il est ajoute au pool actif.

## Phase 0 - Stabilisation avant migration

But : figer le comportement V1 avant de changer le format.

Actions :

- conserver la logique laser actuelle : session active force le laser ON sauf
  steps `off_hold` / `off_move` ;
- garder `LASER ON TEST` et `TEST INVERSE` comme diagnostic materiel ;
- conserver les limites de session, cooldown, retour servo et DNS AP propre ;
- verifier que `pio run -e d1_mini_pro` reste vert.

Livrables :

- firmware V1 stable ;
- documentation existante alignee ;
- base propre pour introduire les modules V2.

## Suivi d'implementation

Etat actuel :

- branche : `V2` ;
- partie Python V2 : implementee ;
- GUI V2 : export et inspection `patterns.dat` implementes ;
- partie firmware V2 : non commencee ;
- LittleFS : non commence ;
- upload `patterns.dat` : non commence.

Commandes disponibles :

```sh
python3 -m tools.chatchat_patterns export-dat tools/examples/default_patterns.json --output /tmp/chatchat_patterns.dat
python3 -m tools.chatchat_patterns inspect-dat /tmp/chatchat_patterns.dat
```

Actions GUI disponibles :

- ouvrir et valider un JSON ;
- sauvegarder le JSON ;
- exporter `patterns.dat` depuis le JSON courant ;
- inspecter un `patterns.dat` existant ;
- afficher dans le panneau validation la taille, le CRC32, les patterns et les
  erreurs eventuelles.

Resultat de reference actuel :

```text
patterns=13
points=334
bytes=3828
crc32=0x236523d4
```

Notes d'implementation Python :

- module ajoute : `tools/chatchat_patterns/binary.py` ;
- GUI mise a jour : `tools/chatchat_patterns/gui.py` ;
- format little-endian ;
- header `LPTN`, version `1`, taille de header `20` octets ;
- pattern record `36` octets ;
- point record `10` octets ;
- coordonnees converties depuis `-1.0..+1.0` vers `0..1000` ;
- `weight=0` accepte pour conserver le pattern `capture` hors tirage pondere ;
- duree point acceptee jusqu'a `10000 ms` pour exporter le pack actuel sans
  resampling.

## Phase 1 - Format compact et structures firmware

But : introduire les types V2 sans changer encore le moteur.

Fichiers probables :

- `src/pattern_format.h`
- `src/pattern_store.h`
- `src/pattern_store.c`
- `tools/chatchat_patterns/binary.py` (fait cote Python)
- `tools/chatchat_patterns/crc32.py` si besoin.

Structures :

```text
pattern_file_header_t
pattern_record_t
pattern_point_t
```

Limites proposees :

```text
MAX_BUILTIN_PATTERNS      5
MAX_USER_PATTERNS         20
MAX_TOTAL_PATTERNS        25
MAX_POINTS_PER_PATTERN    64
MAX_TOTAL_POINTS          1024
MAX_PATTERN_FILE_SIZE     65536
```

Validation firmware minimale :

- magic `LPTN` ;
- version connue ;
- taille coherente ;
- CRC32 valide ;
- bornes `pattern_count` / `point_count` ;
- `x/y` dans `0..1000` ;
- `duration_ms` dans les bornes ;
- easing connu ou remplace par `smoothstep`.

Tests :

- tests unitaires Python de pack/unpack (fait) ;
- build firmware sans LittleFS.

## Phase 2 - Generateur Python V2

But : produire `patterns.dat` depuis le JSON actuel ou un JSON V2.

Actions :

- ajouter une commande CLI :

```sh
python3 -m tools.chatchat_patterns export-dat input.json --output patterns.dat
```

- convertir les coordonnees actuelles `-1.0..+1.0` vers `0..1000` pendant la
  transition ;
- calculer `duration_total_ms` ;
- ecrire header + patterns + points + CRC32 ;
- refuser l'export en cas d'erreur bloquante.

Validation Python forte :

- ID unique et ASCII simple ;
- poids `0..255` ;
- nombre de patterns et points ;
- duree min/max par point ;
- duree totale par pattern ;
- vitesse excessive ;
- ratio laser ON/OFF ;
- taille finale du fichier.

Tests :

- export valide depuis `tools/examples/default_patterns.json` (fait) ;
- fichier relu identique (fait) ;
- erreurs sur ID duplique, point hors borne, duree invalide, CRC modifie
  (partiel : CRC couvert, les autres erreurs passent par le validateur JSON).

## Phase 3 - 5 patterns embarques V2

But : remplacer le gros pack compile par 5 patterns minimum au format interne
V2.

Patterns retenus :

- `idle_wander`
- `slow_hunt`
- `short_escape`
- `loop_play`
- `calm_end`

Actions :

- creer/generer `src/builtin_patterns.c` et `src/builtin_patterns.h` ;
- garder le meme format `pattern_t` / `point_t` que le loader LittleFS ;
- supprimer progressivement la dependance au pack compile complet ;
- garantir que `calm_end` peut etre appele en fin de session plus tard.

Tests :

- boot sans fichier ;
- selection weighted sur les 5 builtins ;
- laser OFF a l'arret ;
- servos retour repos.

## Phase 4 - Loader LittleFS

But : charger `/patterns.dat` puis fallback propre.

Fichiers probables :

- `src/pattern_store.c`
- `src/pattern_store.h`
- adaptation `src/main.c`
- adaptation `platformio.ini` si LittleFS demande un flag ou une lib.

Sequence boot :

```text
1. initialiser pool vide
2. ajouter les 5 builtins
3. monter LittleFS
4. lire /patterns.dat
5. valider
6. sinon tenter /patterns.bak
7. appliquer remplacement/ajout
8. publier le pool actif
```

Regles :

- si LittleFS echoue, boot continue ;
- si fichier invalide, il est ignore ;
- si backup valide, il est utilise ;
- aucune allocation repetee par pattern ou point.

Tests :

- LittleFS absent ;
- `/patterns.dat` absent ;
- fichier valide ;
- fichier corrompu ;
- version inconnue ;
- backup valide apres fichier principal invalide.

## Phase 5 - Moteur V2 normalise

But : executer les points `0..1000` et mapper vers les servos.

Actions :

- ajouter une couche workspace/calibration ;
- convertir `x/y 0..1000` vers coordonnees internes servo ;
- appliquer easing V2 ;
- garder le laser ON par defaut en session, OFF uniquement si point/step OFF ;
- appliquer `speed_scale`.

Configuration minimale :

```text
pan_min
pan_max
pan_center
pan_invert
tilt_min
tilt_max
tilt_center
tilt_invert
workspace_x_min/max
workspace_y_min/max
speed_scale
session_duration_ms
cooldown_duration_ms
```

Premiere version acceptable :

- conserver les constantes servo actuelles ;
- mapper `0..1000` vers la plage mecanique existante ;
- ajouter la config persistante plus tard si necessaire.

Tests :

- centre ;
- coins workspace ;
- inversion pan/tilt ;
- easing linear/smoothstep ;
- laser ON en session ;
- OFF uniquement sur point OFF.

## Phase 6 - Upload web et rollback

But : permettre de remplacer les patterns sans reflasher.

Endpoints proposes :

```text
GET  /patterns
GET  /patterns/download
POST /patterns/upload-dat
POST /patterns/reset
```

Workflow upload :

```text
1. refuser si session running
2. recevoir vers /patterns.tmp
3. valider taille + CRC + contenu
4. renommer /patterns.dat vers /patterns.bak
5. renommer /patterns.tmp vers /patterns.dat
6. recharger pool actif
7. rollback si erreur
```

Regles de securite runtime :

- laser OFF pendant upload/reload/reset ;
- pas de reload pendant `running` ;
- conserver pool actif si upload invalide ;
- reset supprime `.dat`, `.bak`, `.tmp` puis recharge builtins.

Tests :

- upload valide ;
- upload interrompu ;
- upload corrompu ;
- reset ;
- refus pendant session active.

## Phase 7 - Nettoyage V1

But : supprimer ce qui devient obsolete.

Actions :

- retirer le parsing JSON firmware patterns si encore present ;
- retirer les anciens types/convertisseurs non utilises ;
- remplacer la generation `default_patterns.c` par generation builtin V2 ;
- mettre a jour `README.md`, `docs/chatchat.md`, `docs/v_2.md` si decisions
  finales changent.

Tests finaux :

```sh
python3 -m pytest
python3 -m tools.chatchat_patterns validate tools/examples/default_patterns.json
python3 -m tools.chatchat_patterns export-dat tools/examples/default_patterns.json --output /tmp/patterns.dat
pio run -e d1_mini_pro
```

## Risques principaux

- support LittleFS avec `esp8266-rtos-sdk` PlatformIO a confirmer avant
  implementation lourde ;
- flash deja haute, environ 90 %, donc surveiller la taille firmware ;
- migration coordonnees `-1000..+1000` vers `0..1000` a faire sans inverser les
  axes ;
- upload HTTP actuel minimal : limiter taille, timeout et refus pendant session ;
- ne pas perdre le fallback embarque lors du nettoyage V1.

## Ordre recommande

1. format binaire + export/relecture Python ;
2. structures firmware + builtins V2 ;
3. moteur V2 sur builtins uniquement ;
4. validation defensive firmware sur buffer local ;
5. LittleFS lecture seule ;
6. upload/rollback ;
7. nettoyage V1.

## Definition de fini

La V2 est consideree livrable quand :

- le firmware boote et joue les 5 builtins sans fichier ;
- un `patterns.dat` valide est charge au boot ;
- un fichier invalide est rejete sans crash ;
- le backup est utilise si le fichier principal est invalide ;
- l'outil Python exporte un `patterns.dat` verifie ;
- le moteur n'a aucune dependance JSON runtime ;
- `pio run -e d1_mini_pro` et les tests Python passent.
