# Plan de modification CatChat ESP

## Résumé

Faire évoluer le projet vers le format JSON décrit dans `docs/chatchat.md`, en gardant le firmware ESP8266 responsable du temps réel et de la sécurité laser.
Ajouter côté Python les outils de validation et de visualisation nécessaires pour vérifier les patterns avant leur transfert vers l'ESP.

## État actuel pris en compte

- Le firmware est en C avec `esp8266-rtos-sdk` via PlatformIO.
- Les patterns sont actuellement compilés en dur dans `src/game.c` sous forme de `pattern_step_t`.
- Le moteur existant sait déjà exécuter `hold`, `move`, `jitter`, `off_hold` et `off_move`.
- Les coordonnées internes firmware sont des entiers `-1000` à `+1000`.
- L'interface web existe dans `src/web.c` avec AP WiFi, portail captif, actions ON/OFF, laser manuel, WiFi et OTA.
- Le buffer HTTP statique actuel est limité (`http_body[4096]`), ce qui impose une approche prudente pour l'upload JSON.
- `tools/` contient déjà `build_ota_image.py`, mais pas encore d'outil JSON ou de visualiseur.

---

## Partie 1 - ESP sans test

Objectif : ajouter le support firmware minimal du JSON utilisateur sans introduire de tests automatisés côté ESP dans cette phase.

### Tâche 1 - Figement du contrat firmware JSON

- Définir les limites firmware dans `src/main.h` :
  - taille maximale du JSON actif ;
  - nombre maximal de patterns ;
  - nombre maximal de steps par pattern ;
  - durée minimale par type de step ;
  - amplitude maximale du jitter.
- Reprendre les types de steps existants de `src/game.c`.
- Conserver les coordonnées internes `int16_t` en `-1000` à `+1000`.
- Définir explicitement la version supportée : `laser_cat_patterns.v1`.

### Tâche 2 - Extraction du modèle de patterns

- Déplacer les types `pattern_step_type_t`, `pattern_step_t` et `pattern_t` dans un module réutilisable, par exemple `src/pattern.h`.
- Adapter `src/game.c` pour consommer ce modèle commun.
- Garder les patterns compilés existants comme fallback de sécurité.
- Ne pas changer le comportement moteur tant que le chargement JSON n'est pas prêt.

### Tâche 3 - Ajout d'un stockage JSON actif minimal

- Choisir le stockage minimal compatible ESP8266 RTOS SDK :
  - version simple : un fichier `/patterns.json` ;
  - version étendue seulement si le filesystem disponible est confirmé.
- Ajouter un module dédié, par exemple `src/pattern_store.c` / `src/pattern_store.h`.
- Prévoir les opérations :
  - lecture du JSON actif ;
  - écriture atomique ou la plus sûre possible ;
  - fallback vers les patterns compilés si lecture impossible ;
  - taille maximale refusée avant écriture.

### Tâche 4 - Validation JSON côté ESP

- Ajouter un module dédié, par exemple `src/pattern_json.c` / `src/pattern_json.h`.
- Valider au minimum :
  - JSON bien formé ;
  - champ `schema` présent et supporté ;
  - tableau `patterns` présent ;
  - `id` unique pour chaque pattern ;
  - au moins une step par pattern ;
  - `type` dans la liste supportée ;
  - `duration_ms` présent et supérieur au minimum ;
  - `x/y` présents pour `hold`, `move`, `jitter`, `off_move` ;
  - `x/y` dans `-1.0` à `+1.0` ;
  - `amplitude` présent et valide pour `jitter`.
- Convertir les coordonnées JSON vers les entiers firmware `-1000` à `+1000`.
- Refuser le pattern complet en cas d'erreur bloquante.
- Produire un message d'erreur court affichable par l'interface web.

### Tâche 5 - Conversion en structure interne compacte

- Transformer le JSON validé en tableaux `pattern_t` / `pattern_step_t`.
- Éviter le parsing pendant `game_movement_task`.
- Éviter les allocations répétées pendant l'exécution des steps.
- Prévoir une zone active et une zone candidate pour ne jamais remplacer le pack actif par un pack invalide.
- Garder les patterns compilés comme pack de secours si aucun JSON valide n'est chargé.

### Tâche 6 - Intégration au moteur `game.c`

- Ajouter une API de remplacement du pack actif, par exemple `game_load_patterns(...)`.
- Redémarrer proprement le moteur de patterns après activation d'un nouveau JSON :
  - laser OFF ;
  - transition ou retour position sûr ;
  - reset des compteurs de sélection.
- Conserver :
  - `game_set_enabled()` ;
  - `game_is_enabled()` ;
  - `game_movement_task()`.
- Préserver la capture régulière ou définir son équivalent depuis `runtime.capture_every`.
- Ne jamais changer de pattern en plein step sans passer par un arrêt sûr.

### Tâche 7 - Extension de l'interface web

- Ajouter une page patterns, par exemple `/patterns`.
- Fonctions minimales :
  - afficher l'état du pack actif ;
  - afficher le dernier message de validation ;
  - uploader un JSON ;
  - valider avant activation ;
  - enregistrer comme JSON actif ;
  - télécharger le JSON actif.
- Ajouter des routes simples :
  - `GET /patterns` ;
  - `GET /patterns/download` ;
  - `POST /patterns/upload`.
- Réutiliser les protections existantes pendant OTA :
  - refuser l'upload pattern si une OTA est en cours ;
  - couper le jeu ou le laser avant activation d'un nouveau pack.
- Tenir compte du buffer HTTP actuel : ne pas supposer que tout upload JSON tient dans `http_body`.

### Tâche 8 - Sécurité runtime

- Garantir laser OFF :
  - au boot ;
  - si le JSON actif est invalide ;
  - avant remplacement du pack actif ;
  - pendant les transitions invisibles ;
  - si une erreur moteur est détectée.
- Ajouter un timeout général de session si demandé pour la première version.
- Refuser les durées trop courtes qui créent des mouvements dangereux.
- Limiter les jumps visibles trop rapides.

### Tâche 9 - Documentation ESP

- Mettre à jour `README.md` :
  - format JSON actif ;
  - routes web patterns ;
  - limites firmware ;
  - comportement fallback.
- Mettre à jour `docs/chatchat.md` si une décision technique diffère du cahier fonctionnel.

### Tâche 10 - Validation manuelle ESP uniquement

- Compiler avec :

```sh
pio run
```

- Compiler l'image OTA si les routes ou le script OTA sont touchés :

```sh
pio run -e d1_mini_pro_ota
```

- Ne pas ajouter de tests automatisés ESP dans cette partie.
- Ne pas flasher ni ouvrir le moniteur série sans demande explicite.

---

## Partie 2 - Tools Python avec test

Objectif : créer les outils Python nécessaires pour valider et visualiser les fichiers JSON avant transfert vers l'ESP, avec tests automatisés.

### Tâche 1 - Créer un package tools testable

- Organiser les nouveaux modules Python sans casser `tools/build_ota_image.py`.
- Proposition :
  - `tools/chatchat_patterns/__init__.py` ;
  - `tools/chatchat_patterns/model.py` ;
  - `tools/chatchat_patterns/validator.py` ;
  - `tools/chatchat_patterns/geometry.py` ;
  - `tools/chatchat_patterns/visualizer.py` ;
  - `tools/chatchat_patterns/cli.py`.
- Garder une CLI simple appelable par `python3 -m tools.chatchat_patterns`.

### Tâche 2 - Modèle Python du JSON

- Représenter :
  - pack ;
  - metadata ;
  - runtime ;
  - pattern ;
  - step.
- Charger un JSON depuis un fichier.
- Conserver les champs utiles non bloquants si possible.
- Produire des erreurs structurées avec chemin logique, par exemple
  `patterns[2].steps[4].duration_ms`.

### Tâche 3 - Validation Python stricte

- Reprendre les contrôles minimaux de `docs/chatchat.md` :
  - JSON bien formé ;
  - `schema` supporté ;
  - `patterns` présent et non vide ;
  - `id` unique ;
  - type de step valide ;
  - coordonnées obligatoires selon le type ;
  - coordonnées dans `-1.0` à `+1.0` ;
  - durées minimales ;
  - amplitude obligatoire pour `jitter`.
- Ajouter des avertissements comportementaux :
  - mouvement visible inférieur à 1000 ms ;
  - pause visible trop courte ;
  - jitter trop rapide ;
  - saut trop grand pour une durée faible ;
  - disparition trop longue.
- Aligner autant que possible les limites avec celles prévues côté ESP.

### Tâche 4 - Conversion et géométrie

- Ajouter la conversion `-1.0..+1.0` vers `-1000..+1000`.
- Calculer :
  - durée totale d'un pattern ;
  - segments visibles ;
  - segments invisibles ;
  - pauses ;
  - zones de jitter ;
  - vitesse relative entre deux points.
- Servir ces calculs au visualiseur et aux tests.

### Tâche 5 - CLI de validation

- Ajouter une commande de validation :

```sh
python3 -m tools.chatchat_patterns validate <fichier.json>
```

- Sortie attendue :
  - erreurs bloquantes ;
  - avertissements ;
  - résumé du nombre de patterns et steps ;
  - code retour non nul en cas d'erreur.

### Tâche 6 - CLI de liste

- Ajouter une commande :

```sh
python3 -m tools.chatchat_patterns list <fichier.json>
```

- Afficher :
  - id ;
  - nom ;
  - catégorie ;
  - poids ;
  - intensité ;
  - nombre de steps ;
  - durée totale.

### Tâche 7 - Visualiseur 2D

- Ajouter une commande :

```sh
python3 -m tools.chatchat_patterns view <fichier.json> --pattern <id>
```

- Représentation minimale :
  - segment visible en ligne continue ;
  - segment invisible en pointillé ;
  - pause visible par point ou cercle ;
  - jitter par cercle de rayon amplitude ;
  - début et fin identifiables ;
  - durée totale affichée.
- Prévoir un export PNG :

```sh
python3 -m tools.chatchat_patterns view <fichier.json> --pattern <id> --output out.png
```

- Si `matplotlib` est utilisé, documenter la dépendance et isoler l'import dans
  la commande de visualisation pour que la validation reste légère.

### Tâche 8 - Exemples JSON

- Ajouter un exemple valide, par exemple `tools/examples/default_patterns.json`.
- Ajouter des exemples invalides ciblés pour les tests :
  - coordonnées hors plage ;
  - duration manquante ;
  - type inconnu ;
  - id dupliqué ;
  - jitter sans amplitude.

### Tâche 9 - Tests automatisés Python

- Ajouter des tests dans `test/` ou `tests/` selon la convention retenue.
- Couvrir :
  - chargement JSON valide ;
  - rejet JSON invalide ;
  - unicité des ids ;
  - règles par type de step ;
  - conversion coordonnées ;
  - calcul de durée totale ;
  - extraction segments ON/OFF ;
  - codes retour CLI `validate`.
- Prévoir un test léger du visualiseur en mode export si la dépendance graphique
  est disponible, sinon le marquer comme optionnel.

### Tâche 10 - Commande de test Python

- Si aucun runner n'est encore déclaré, utiliser `pytest`.
- Ajouter la commande documentée :

```sh
python3 -m pytest
```

- Si les dépendances Python sont formalisées ensuite, ajouter un fichier dédié
  uniquement si nécessaire :
  - `requirements-dev.txt` pour `pytest` et éventuellement `matplotlib`.
- Ne pas modifier les dépendances firmware pour les outils Python.

### Tâche 11 - Documentation tools

- Documenter dans `README.md` ou `docs/chatchat.md` :
  - validation ;
  - liste des patterns ;
  - visualisation ;
  - export PNG ;
  - relation entre coordonnées JSON et coordonnées firmware.
- Inclure un flux recommandé :
  1. écrire le JSON ;
  2. valider avec l'outil Python ;
  3. visualiser ;
  4. corriger ;
  5. uploader via l'interface web ESP.
