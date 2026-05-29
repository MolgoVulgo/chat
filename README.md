# Laser Cat Toy ESP8266

Jouet laser automatique pour chat basé sur un Wemos D1 mini Pro / ESP8266 avec deux servos et un module laser 5 V piloté par MOSFET.

Le firmware utilise `esp8266-rtos-sdk` via PlatformIO. Il ne s'agit pas d'un sketch Arduino.

## Fonctionnement

Le laser ne suit plus des mouvements aléatoires indépendants. Le code utilise un moteur de patterns coordonnés :

- les deux servos bougent ensemble dans un espace de coordonnées interne ;
- les transitions sont interpolées avec une courbe `smoothstep` pour éviter les mouvements brusques ;
- le laser peut s'éteindre pendant certains déplacements invisibles ;
- plusieurs patterns simulent des comportements de proie ;
- le runtime peut charger des patterns au format binaire `patterns.dat` v2.

Les patterns par défaut compilés sont générés en C dans `src/default_patterns.c` via :

```sh
python3 tools/generate_default_patterns_c.py
```

Par défaut, l'ESP utilise ce pack compilé pour préserver la RAM du serveur web.

## Matériel

Cible testée :

- Wemos D1 mini Pro / ESP8266
- 2 servos
- 1 module laser 5 V
- 1 MOSFET 2N7000 pour piloter le laser
- résistance de 100 ohms entre GPIO et gate du MOSFET
- alimentation 5 V adaptée aux servos et au laser

Le laser ne doit pas être alimenté directement depuis un GPIO ESP8266.

## Câblage

| Fonction | Pin Wemos | GPIO ESP8266 | Note |
| --- | --- | --- | --- |
| Servo horizontal | D1 | GPIO5 | Signal servo |
| Servo vertical | D2 | GPIO4 | Signal servo |
| Laser | D5 | GPIO14 | Gate MOSFET via 100 ohms |

Les masses du Wemos, de l'alimentation servos et du module laser doivent être communes.

## Configuration

Les réglages principaux sont dans `src/main.h`.

### Servos

```c
#define SERVO_HORIZONTAL_MIN    50
#define SERVO_HORIZONTAL_MAX    130

#define SERVO_VERTICAL_MIN      26
#define SERVO_VERTICAL_MAX      60
```

Ces limites évitent de forcer mécaniquement les servos. À ajuster selon le montage.

Le mapping impulsion utilise la plage utile suivante :

```c
#define SERVO_PULSE_ANGLE_MIN   20
#define SERVO_PULSE_ANGLE_MAX   160
#define SERVO_MIN_US            500
#define SERVO_MAX_US            2400
```

Soit `20° -> 500 us`, `90° -> 1450 us`, `160° -> 2400 us`.

Si un axe part dans le mauvais sens :

```c
#define SERVO_HORIZONTAL_INVERT 0
#define SERVO_VERTICAL_INVERT   0
```

Mettre `1` pour inverser l'axe concerné.

### Coordonnées

Le moteur travaille en coordonnées normalisées :

- `x = -1000` : gauche
- `x = +1000` : droite
- `y = -1000` : bas
- `y = +1000` : haut

La position de démarrage est :

```c
#define START_X 0
#define START_Y 0
```

### Timing

```c
#define MOTION_TICK_MS                  20
#define PATTERN_TRANSITION_MIN_MS       400
#define PATTERN_TRANSITION_MAX_MS       800
#define PATTERN_CAPTURE_EVERY           5
#define JITTER_POINT_INTERVAL_MS        140
```

- `MOTION_TICK_MS` règle la fréquence de mise à jour des trajectoires.
- `PATTERN_TRANSITION_*` règle les transitions laser éteint entre deux scènes.
- `PATTERN_CAPTURE_EVERY` force le pattern `capture` toutes les N scènes.
- `JITTER_POINT_INTERVAL_MS` règle la nervosité des petits mouvements aléatoires.

## Sessions et laser

Le laser est uniquement ON/OFF. Il n'y a pas de PWM laser.
Le jeu est lance sous forme de session bornee :

- `GAME_SESSION_MAX_MS` limite la duree d'une session ;
- `GAME_COOLDOWN_MS` bloque une relance immediate apres une fin automatique ;
- au demarrage d'une session, le laser est force ON et reste actif pendant le
  jeu ;
- seuls les steps `off_hold` et `off_move` coupent explicitement le laser ;
- l'arret manuel coupe immediatement le jeu et le laser ;
- le bouton `LASER ON TEST` coupe le jeu et allume le laser pour test jusqu'a
  `LASER OFF` ou `LASER_TEST_MAX_MS` ;
- le bouton `TEST INVERSE` force le niveau GPIO oppose pour verifier rapidement
  si `LASER_ACTIVE_LOW` doit etre inverse ;
- le pulse laser court reste reserve aux builds `DEBUG_HARDWARE_ENABLED=1`.

Le pilotage est fait par :

```c
hardware_laser_set(true);
hardware_laser_set(false);
```

Cette fonction met à jour l'état interne et applique directement le niveau GPIO.

## Logs série

La vitesse série est configurée à `115200` dans :

- `src/main.h` avec `SERIAL_BAUD_RATE`
- `platformio.ini` avec `monitor_speed`

Le firmware affiche au démarrage :

```text
Laser Cat Toy ESP8266 RTOS start - pattern engine
servo_h_gpio=5 servo_v_gpio=4 laser_gpio=14
```

Puis il affiche le pattern en cours :

```text
pattern: mouse_cautious - Souris prudente
```

Les logs firmware sont controles par tags de compilation :

```ini
build_flags =
    -DAPP_LOG_ENABLED=1
    -DPATTERN_LOG_ENABLED=1
    -DWEB_LOG_ENABLED=1
    -DWEB_DEBUG_LOG_ENABLED=0
```

Mettre un flag à `0` ou le retirer pour désactiver le log correspondant. `WEB_DEBUG_LOG_ENABLED` ajoute les traces très verboses HTTP/scan. Le mot de passe WiFi n'est jamais affiche, seule sa longueur est loggee.

Le boot ROM de l'ESP8266 peut encore afficher quelques caractères illisibles avant le démarrage du firmware. C'est normal.

## Interface Web

Au démarrage, l'ESP8266 lance un point d'acces de configuration :

```text
SSID: LaserCatToy
Mot de passe: lasercat123
URL: http://192.168.4.1/
```

Un mini DNS captif ecoute aussi sur le port 53 et renvoie `192.168.4.1` pour les requetes DNS. Sur la plupart des telephones et ordinateurs, la page de configuration s'ouvre donc automatiquement apres connexion au WiFi `LaserCatToy`.

Le firmware demarre en `STATIONAP_MODE`, puis bascule en `STATION_MODE` quand
la station obtient une adresse IP :

- le point d'acces `LaserCatToy` sert a la configuration initiale ;
- le point d'acces disparait apres connexion au WiFi choisi ;
- le DNS captif ne repond que lorsque l'AP de configuration est actif.

### Page principale

```text
http://192.168.4.1/
```

Cette page permet :

- de voir l'etat du jouet : `ON` ou `OFF` ;
- d'activer le jouet avec `/on` ;
- de couper le jouet avec `/off` ;
- de tester le laser avec `/laser/test/on`, puis de l'eteindre avec
  `/laser/off` ;
- de tester le niveau GPIO inverse avec `/laser/test/invert` si le jeu bouge
  mais que le laser reste eteint ;
- d'aller vers la configuration WiFi.

Quand le jouet est coupe :

- le laser est eteint ;
- les servos reviennent vers la position de repos ;
- les pulses servo restent actifs pendant `SERVO_REST_SETTLE_MS` avant coupure ;
- le moteur de patterns attend avant de lancer une nouvelle scene.

### Page WiFi

```text
http://192.168.4.1/wifi
```

Cette page permet :

- d'afficher les reseaux WiFi detectes ;
- de lancer automatiquement un scan si aucune liste n'est disponible ;
- de relancer un scan manuel avec `/scan` ;
- de choisir un SSID dans la liste ;
- de saisir un SSID manuel ;
- de saisir le mot de passe ;
- de demander la connexion station.

Pendant un scan, la page se recharge automatiquement toutes les 2 secondes jusqu'a affichage de la liste.

La configuration WiFi est appliquee avec `wifi_station_set_config()`, donc elle est sauvegardee dans la zone de configuration flash du SDK ESP8266.

### Page Patterns

```text
http://192.168.4.1/patterns
http://<ip-esp>/patterns
```

Cette page permet :

- afficher le pack actif ;
- choisir un pattern précis ou revenir au mode automatique pondéré ;
- régler la vitesse globale d'exécution ;
- télécharger le pack actif généré depuis les structures runtime.

Le firmware supporte le chargement d'un pack `patterns.dat` au format v2.
Le format exact est décrit dans [docs/patterns_dat_v2_format_explication.md](/home/kaj/Develop/000-PlatformIO/chat/docs/patterns_dat_v2_format_explication.md).

Limites firmware principales :

- patterns maximum : `PATTERN_MAX_PATTERNS` ;
- steps maximum : `PATTERN_MAX_TOTAL_STEPS` ;
- vitesse globale : `PATTERN_SPEED_MIN_PERCENT` à `PATTERN_SPEED_MAX_PERCENT` ;
- coordonnées runtime firmware : `-1000` à `+1000`.

La vitesse est un pourcentage : `100%` garde les durées nominales, `150%` accélère
les mouvements, `75%` les ralentit. Le réglage s'applique aux mouvements,
pauses, jitter, transitions et pauses entre patterns.

## Compilation

Compiler :

```sh
pio run
```

Flasher :

```sh
pio run -t upload
```

Ouvrir le moniteur série :

```sh
pio device monitor
```

## Outils Python DAT

Les outils Python sont centrés sur `patterns.dat` v2 (validation, inspection, visualisation).

Ouvrir la GUI locale de gestion des patterns :

```sh
./start.sh
```

Ou ouvrir un fichier précis :

```sh
./start.sh gui tools/patterns.dat
```

Valider un fichier DAT :

```sh
./start.sh validate tools/patterns.dat
```

Lister les patterns :

```sh
./start.sh list tools/patterns.dat
```

Inspecter un DAT :

```sh
./start.sh inspect-dat tools/patterns.dat
```

Exporter une visualisation PNG d'un pattern :

```sh
./start.sh view tools/patterns.dat --pattern <id_pattern> --output pattern.png
```

La GUI utilise `tkinter`. L'export PNG nécessite `matplotlib`. La validation,
la liste et l'aperçu GUI utilisent uniquement la bibliothèque standard Python.

Tests Python :

```sh
python3 -m pytest
```

## Structure du projet

```text
platformio.ini   Configuration PlatformIO
src/main.h       Pins, limites mécaniques, timing et constantes
src/main.c       Bootstrap ESP8266 RTOS SDK et lancement des tâches
src/hardware.*   GPIO, laser, conversion coordonnées -> servos, PWM servo
src/game.*       Moteur de patterns et état ON/OFF du jouet
src/pattern.h    Modèle compact partagé des patterns
src/pattern_store.* Chargement/validation du pack binaire patterns.dat
src/default_patterns.* Pack compilé embarqué
src/web.*        WiFi AP/station, portail captif DNS, serveur HTTP
src/app_util.h   Helpers partagés temps/coordonnées
```

## Architecture logicielle

Le firmware lance plusieurs tâches FreeRTOS depuis `src/main.c` :

- `hardware_servo_pwm_task` : génère les impulsions servo à 50 Hz ;
- `game_movement_task` : choisit et exécute les patterns ;
- `web_http_server_task` : sert les pages Web ;
- `web_dns_server_task` : répond aux requêtes DNS captives ;
- `web_wifi_status_task` : trace les changements d'état WiFi station.

Les responsabilités sont séparées :

- `game.c` calcule les trajectoires et demande des positions ;
- `hardware.c` convertit les coordonnées en angles et pilote les GPIO ;
- `web.c` expose l'interface utilisateur et appelle seulement `game_set_enabled()` pour ON/OFF.

## Sécurité

- Ne jamais pointer le laser vers les yeux.
- Ne pas piloter le laser directement depuis un GPIO.
- Vérifier les limites servo avant de laisser le jouet tourner longtemps.
- Prévoir une alimentation 5 V suffisante pour les servos, séparée ou correctement dimensionnée.
