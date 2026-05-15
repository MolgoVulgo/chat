# agent-chatchat.md

## Contexte du dépôt

Ce fichier définit le cadre de travail d'un agent Codex pour le projet
**CatChat ESP / Laser Cat Toy ESP8266**.

Le dépôt doit être traité comme un projet bi-brique :
- **firmware embarqué** en **C / esp8266-rtos-sdk / PlatformIO** pour
  **ESP8266 / Wemos D1 mini Pro** ;
- **outils Python** dans `tools/` pour les tâches hors firmware
  (génération d'image OTA, visualisation, validation ou préparation de JSON).

Le système cible pilote un jouet laser pour chat :
- deux servos déplacent le point laser ;
- un GPIO pilote le laser via MOSFET ;
- un moteur de patterns exécute des trajectoires de type proie ;
- une interface web embarquée configure le jouet et expose les actions simples ;
- le JSON devient le format utilisateur cible pour créer, valider,
  visualiser, stocker et jouer des patterns.

---

## Règle de cadrage

- Traiter ce dépôt comme un projet **firmware C ESP8266 + app tools Python**.
- Ne pas appliquer un cadrage Arduino, ESP32, ESP-IDF natif, desktop,
  Qt/QML, backend web lourd ou application cloud.
- Respecter l'architecture existante :
  - `src/main.*` : bootstrap ESP8266 RTOS SDK, constantes et lancement tâches ;
  - `src/hardware.*` : GPIO, laser, conversion coordonnées vers servos, PWM ;
  - `src/game.*` : moteur de jeu, patterns, trajectoires et état ON/OFF ;
  - `src/web.*` : AP WiFi, portail captif, serveur HTTP et actions utilisateur ;
  - `src/ota.*` : préparation et écriture OTA ;
  - `tools/` : scripts Python exécutés sur la machine de développement.
- Préserver la sécurité laser : laser OFF au boot, sur erreur, pendant les
  transitions invisibles, en pause et en fin de session.

---

## Sources de vérité du projet

Lire d'abord, dans cet ordre :
1. `docs/chatchat.md`
2. `README.md`
3. `platformio.ini`
4. `src/main.h`
5. `src/game.c` et `src/game.h`
6. `src/hardware.c` et `src/hardware.h`
7. `src/web.c` et `src/web.h`
8. `src/ota.c` et `src/ota.h`
9. `tools/`
10. la zone réellement touchée dans le dépôt

Décisions fonctionnelles à préserver :
- cible matérielle : Wemos D1 mini Pro / ESP8266 ;
- framework : `esp8266-rtos-sdk` via PlatformIO ;
- langage firmware : C ;
- coordonnées firmware internes : `-1000` à `+1000` ;
- coordonnées JSON cible : `-1.0` à `+1.0` ;
- JSON comme format utilisateur ;
- structure interne compacte recommandée côté ESP après validation JSON ;
- parsing JSON au chargement, pas pendant les mouvements temps réel ;
- visualisation et validation hors firmware possibles via outils Python ;
- OTA optionnel via environnement `d1_mini_pro_ota`.

---

## Architecture à respecter

### Firmware ESP8266

Le firmware est responsable de :
- initialiser un état matériel sûr ;
- piloter les servos à fréquence stable ;
- piloter le laser uniquement en ON/OFF ;
- calculer les trajectoires et interpolations ;
- exécuter les patterns sans blocage long ;
- gérer ON/OFF du jouet ;
- exposer l'interface web embarquée ;
- gérer le portail captif WiFi ;
- gérer l'OTA quand l'environnement OTA est utilisé.

Découpage attendu en priorité :
- matériel : `hardware.*` ;
- moteur de patterns : `game.*` ;
- configuration et constantes : `main.h` ;
- réseau/web : `web.*` ;
- OTA : `ota.*` ;
- helpers partagés : `app_util.h`.

### Outils Python

Les outils Python sont responsables des traitements de développement ou
pré-exécution qui ne doivent pas peser sur l'ESP8266 :
- génération ou assemblage d'images OTA ;
- visualisation de trajectoires JSON ;
- validation statique de fichiers JSON ;
- diagnostics locaux ;
- préparation éventuelle d'un format compact.

Règles pour `tools/` :
- garder les scripts autonomes et sobres en dépendances ;
- ne jamais intégrer de secret WiFi ou d'adresse privée en dur ;
- produire des erreurs lisibles ;
- éviter les effets de bord implicites ;
- documenter l'usage dans `README.md` ou `docs/chatchat.md` si l'outil devient
  une voie d'usage normale.

---

## Contrat JSON cible

Le JSON utilisateur cible est décrit dans `docs/chatchat.md`.

Structure attendue :
- `schema`, par exemple `laser_cat_patterns.v1` ;
- `meta` ;
- `coordinate_system` ;
- `runtime` ;
- tableau `patterns`.

Types d'actions minimaux :
- `hold` ;
- `move` ;
- `jitter` ;
- `off_hold` ;
- `off_move`.

Contraintes à préserver :
- `x` et `y` normalisés de `-1.0` à `+1.0` ;
- conversion possible en entier interne `-1000` à `+1000` ;
- `duration_ms` explicite ;
- laser OFF pendant les transitions invisibles ;
- validation avant activation ;
- refus ou correction des patterns trop rapides ;
- aucune allocation répétée ou parsing JSON dans la boucle de mouvement.

Le MVP JSON doit rester limité à :
1. format JSON stable ;
2. chargement d'un JSON actif ;
3. upload web simple ;
4. validation minimale ;
5. exécution de `hold`, `move`, `off_hold`, `off_move` ;
6. visualiseur Python des trajectoires ON/OFF.

---

## Priorités techniques

Toujours prioriser dans cet ordre :
1. sécurité du laser et limites mécaniques des servos ;
2. stabilité runtime ESP8266 ;
3. absence de blocage dans les tâches critiques ;
4. déterminisme du moteur de patterns ;
5. validation stricte des données utilisateur ;
6. sobriété mémoire RAM/flash ;
7. clarté des responsabilités entre `game`, `hardware`, `web`, `ota` ;
8. qualité des outils Python ;
9. confort de l'interface web.

Le principe directeur :
- le firmware exécute un format déjà validé ;
- l'outil Python aide à voir et corriger les patterns avant transfert ;
- l'interface web embarquée reste simple ;
- la sécurité matérielle prime sur toute animation ou fonctionnalité.

---

## Politique de modification

- Modifier uniquement les fichiers nécessaires à l'intention du changement.
- Conserver les changements localisés, lisibles et testables.
- Ne pas toucher aux artefacts générés, builds, caches ou binaires.
- Ne pas modifier `.pio/`, fichiers de build ou images OTA générées.
- Ne pas ajouter de dépendances lourdes sans justification explicite.
- En cas de doute, préférer une correction minimale dans le module déjà
  responsable.
- Si le changement impacte le comportement utilisateur, mettre à jour
  `README.md` ou `docs/chatchat.md`.
- Si le changement impacte le format JSON, mettre à jour `docs/chatchat.md`
  et les validateurs/outils concernés.
- Si le changement impacte le pilotage matériel, vérifier les limites dans
  `src/main.h` et préserver les états sûrs.

### Collaboration obligatoire

- Exposer le constat technique avant les modifications significatives.
- Annoncer les fichiers modifiés avant édition.
- Expliciter les hypothèses quand un comportement matériel n'est pas vérifiable
  localement.
- Ne jamais flasher, monitorer ou lancer une commande matérielle sans demande
  explicite.

---

## Contraintes firmware

- Cible : ESP8266 / Wemos D1 mini Pro.
- Framework : `esp8266-rtos-sdk` via PlatformIO.
- Langage : C.
- Environnement principal : `d1_mini_pro`.
- Environnement OTA : `d1_mini_pro_ota`.
- Série : `115200`.
- Pins de référence :
  - servo horizontal : D1 / GPIO5 ;
  - servo vertical : D2 / GPIO4 ;
  - laser : D5 / GPIO14 via MOSFET.
- Le laser ne doit jamais être alimenté directement par un GPIO.
- Les masses alimentation servos, laser et ESP doivent être communes.
- Les servos doivent rester dans les limites définies par `src/main.h`.
- Privilégier les entiers et structures compactes côté firmware.
- Éviter les allocations dynamiques répétées.
- Éviter les logs verbeux en fonctionnement nominal.
- Gérer les erreurs en ramenant le laser OFF.

### Tâches FreeRTOS

Les responsabilités existantes sont :
- `hardware_servo_pwm_task` : impulsions servo ;
- `game_movement_task` : choix et exécution des patterns ;
- `web_http_server_task` : serveur HTTP ;
- `web_dns_server_task` : DNS captif ;
- `web_wifi_status_task` : suivi WiFi station.

Ne pas faire dépendre la tâche servo ou le moteur de mouvement d'une requête
HTTP synchrone ou d'un parsing coûteux.

### Logs firmware

- Utiliser les macros existantes dans `src/logging.h`.
- Respecter les flags de `platformio.ini` :
  - `APP_LOG_ENABLED` ;
  - `PATTERN_LOG_ENABLED` ;
  - `HARDWARE_LOG_ENABLED` ;
  - `WEB_LOG_ENABLED` ;
  - `WEB_DEBUG_LOG_ENABLED` ;
  - `OTA_LOG_ENABLED`.
- Ne jamais logger de mot de passe WiFi, token ou donnée sensible.

---

## Contraintes outils Python

- Utiliser Python uniquement pour les outils de développement ou de validation.
- Ne pas déplacer de logique temps réel critique dans Python.
- Garder les scripts compatibles avec une exécution locale simple.
- Préférer la bibliothèque standard quand elle suffit.
- Si un outil nécessite une dépendance graphique ou scientifique, documenter
  précisément l'installation et prévoir un mode erreur clair.
- Ne pas écrire dans le firmware, la flash ou le réseau sans action explicite de
  l'utilisateur.

---

## Commandes de validation

Validation firmware minimale :

```sh
pio run
```

Validation OTA si le changement touche `src/ota.*`, `platformio.ini` ou
`tools/build_ota_image.py` :

```sh
pio run -e d1_mini_pro_ota
```

Commandes matérielles à ne lancer que sur demande explicite :

```sh
pio run -t upload
pio device monitor
```

Pour les outils Python, utiliser la commande la plus étroite disponible, par
exemple :

```sh
python3 tools/<script>.py --help
```

ou un test ciblé si le dépôt en fournit un.

---

## Hors périmètre initial

Ne pas introduire sans demande explicite :
- cloud ;
- application mobile ;
- Alexa ou assistant vocal ;
- caméra ou calibration caméra ;
- authentification avancée ;
- éditeur graphique web complet ;
- MQTT ;
- génération automatique de patterns par IA ;
- moteur physique complexe ;
- persistance longue durée hors fichiers JSON nécessaires.
