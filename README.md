# Laser Cat Toy ESP8266

Jouet laser automatique pour chat basé sur un Wemos D1 mini Pro / ESP8266 avec deux servos et un module laser 5 V piloté par MOSFET.

Le firmware utilise `esp8266-rtos-sdk` via PlatformIO. Il ne s'agit pas d'un sketch Arduino.

## Fonctionnement

Le laser ne suit plus des mouvements aléatoires indépendants. Le code utilise un moteur de patterns coordonnés :

- les deux servos bougent ensemble dans un espace de coordonnées interne ;
- les transitions sont interpolées avec une courbe `smoothstep` pour éviter les mouvements brusques ;
- le laser peut s'éteindre pendant certains déplacements invisibles ;
- plusieurs patterns simulent des comportements de proie : souris prudente, insecte nerveux, fuite vers un bord, cache-cache, patrouille, ellipse cassée, etc. ;
- tous les 5 patterns, un pattern `capture` plus long est joué.

Les patterns sont définis dans `src/game.c` sous forme de tableaux de `pattern_step_t`.

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

## Laser

Le laser est uniquement ON/OFF. Il n'y a pas de PWM laser.

Le pilotage est fait par :

```c
laser_set(true);
laser_set(false);
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

Le firmware reste en mode `STATIONAP_MODE` :

- le point d'acces `LaserCatToy` reste disponible pour configurer l'appareil ;
- la partie station peut se connecter au WiFi choisi via la page Web.

### Page principale

```text
http://192.168.4.1/
```

Cette page permet :

- de voir l'etat du jouet : `ON` ou `OFF` ;
- d'activer le jouet avec `/on` ;
- de couper le jouet avec `/off` ;
- d'aller vers la configuration WiFi.

Quand le jouet est coupe :

- le laser est eteint ;
- les servos reviennent vers la position de repos ;
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

## Structure du projet

```text
platformio.ini   Configuration PlatformIO
src/main.h       Pins, limites mécaniques, timing et constantes
src/main.c       Bootstrap ESP8266 RTOS SDK et lancement des tâches
src/hardware.*   GPIO, laser, conversion coordonnées -> servos, PWM servo
src/game.*       Moteur de patterns et état ON/OFF du jouet
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
