# Cahier fonctionnel — CatChat ESP

## 1. Objectif

CatChat ESP est un système ESP8266 pilotant un jouet laser pour chat à partir de patterns décrits en JSON.

Le JSON devient le format pivot du projet :

- format de transit entre l’interface web, l’ESP et les outils externes ;
- format éditable par l’utilisateur ;
- format visualisable par un outil Python ;
- éventuellement format directement exécuté par l’ESP, si les contraintes mémoire restent acceptables.

Le système doit permettre de créer, importer, valider, stocker, visualiser et jouer des patterns de déplacement laser.

## 2. Périmètre fonctionnel

Le périmètre couvre trois blocs :

1. Firmware ESP8266.
2. Interface web embarquée.
3. Outil Python de visualisation des trajectoires.

Le firmware reste responsable de l’exécution temps réel : servos, laser, interpolation, timing, sécurité de mouvement.

L’interface web sert à envoyer, consulter et gérer les fichiers JSON.

Le logiciel Python sert à vérifier visuellement les patterns avant transfert vers l’ESP.

## 3. Rôle du JSON

Le JSON décrit des patterns de mouvement sous forme de scènes.

Un pattern contient :

- un identifiant unique ;
- un nom lisible ;
- une catégorie ;
- un poids de sélection ;
- un niveau d’intensité ;
- une description ;
- une liste ordonnée d’étapes.

Une étape décrit :

- un type d’action ;
- l’état du laser ;
- une position cible `x/y` si nécessaire ;
- une durée ;
- éventuellement une amplitude pour les micro-mouvements.

Format de coordonnées retenu :

```text
x = -1.0 gauche  →  +1.0 droite
y = -1.0 bas     →  +1.0 haut
```

Sur ESP, ces coordonnées peuvent être converties en entier interne `-1000 → +1000` pour éviter les flottants.

## 4. Types d’actions JSON

Types minimaux à prendre en charge :

| Type | Rôle |
|---|---|
| `hold` | Maintient une position visible |
| `move` | Déplace le laser vers une position cible |
| `jitter` | Génère des micro-mouvements autour d’un point |
| `off_hold` | Laser éteint, sans déplacement visible |
| `off_move` | Déplacement invisible vers une nouvelle position |

Le comportement doit rester déterministe à l’exécution : une étape JSON correspond à un comportement moteur clair.

## 5. Contraintes de mouvement

Le but n’est pas de dessiner des formes parfaites. Le système doit simuler une proie.

Règles générales :

- mouvements lents majoritaires ;
- pauses visibles longues ;
- accélérations rares et courtes ;
- disparitions brèves ;
- réapparitions proches ou logiques ;
- pas de changement de direction permanent ;
- pas de patterns trop compacts ;
- pas de jitter trop rapide.

Valeurs fonctionnelles recommandées :

| Élément | Plage cible |
|---|---:|
| Mouvement lent | 4000 à 9000 ms |
| Fuite courte | 1500 à 2800 ms |
| Pause visible | 2000 à 6000 ms |
| Jitter | 1500 à 3500 ms |
| Intervalle jitter | 400 à 800 ms |
| Pause invisible | 800 à 2500 ms |
| Transition invisible | 1500 à 3500 ms |

Le firmware doit pouvoir refuser ou corriger un pattern trop rapide.

## 6. Firmware ESP8266

### 6.1 Fonctions attendues

Le firmware doit :

- piloter deux servos ;
- piloter le laser via GPIO/MOSFET ;
- charger un fichier JSON depuis le stockage interne ;
- valider le JSON avant activation ;
- convertir les coordonnées JSON vers les angles servo ;
- exécuter les patterns dans l’ordre ou par tirage pondéré ;
- gérer les transitions invisibles entre patterns ;
- assurer un état laser OFF au démarrage logiciel ;
- exposer une interface web de configuration.

### 6.2 Modes d’utilisation JSON

Deux stratégies sont possibles.

#### Mode A — JSON utilisé directement

L’ESP lit le JSON stocké et l’interprète à l’exécution.

Avantages :

- souple ;
- modification facile depuis l’interface web ;
- pas besoin de recompiler ;
- cohérent avec un éditeur externe Python.

Inconvénients :

- parsing JSON plus coûteux ;
- plus de RAM consommée ;
- validation obligatoire ;
- risque de latence si mal conçu.

Ce mode est acceptable si le JSON reste compact et si le parsing est fait au chargement, pas pendant les mouvements temps réel.

#### Mode B — JSON transformé en structure interne

L’ESP reçoit un JSON, le valide, puis le transforme en format interne compact.

Exemples possibles :

- structure binaire ;
- tableau compact en mémoire ;
- fichier précompilé interne ;
- normalisation JSON réécrite.

Avantages :

- plus robuste ;
- plus rapide à exécuter ;
- moins de risque pendant le temps réel ;
- permet de rejeter/corriger les durées trop faibles.

Inconvénients :

- plus de logique firmware ;
- moins lisible si le format interne remplace le JSON source.

Décision fonctionnelle recommandée :

```text
Le JSON reste le format utilisateur.
L’ESP le valide puis le transforme en structure interne compacte au chargement.
Le JSON original peut être conservé comme source.
```

## 7. Stockage ESP

Le firmware doit stocker au minimum :

- le JSON actif ;
- éventuellement plusieurs JSON nommés ;
- un fichier de configuration courant ;
- un état de sélection du pattern actif ou du pack actif.

Organisation fonctionnelle proposée :

```text
/patterns/active.json
/patterns/uploaded/<nom>.json
/config/runtime.json
```

Si le stockage est limité, la version minimale se limite à :

```text
/patterns.json
/config.json
```

## 8. Interface web embarquée

### 8.1 Fonctions minimales

L’interface web doit permettre :

- afficher le JSON actif ;
- uploader un nouveau JSON via formulaire ;
- valider le JSON avant enregistrement ;
- enregistrer le JSON comme actif ;
- redémarrer proprement le moteur de patterns ;
- afficher les erreurs de validation ;
- télécharger le JSON actif.

### 8.2 Fonctions optionnelles

Fonctions utiles mais non prioritaires :

- liste des fichiers JSON stockés ;
- activation d’un fichier existant ;
- suppression d’un fichier ;
- renommage ;
- test d’un pattern précis ;
- pause/play du moteur ;
- réglage de vitesse globale ;
- réglage d’amplitude globale ;
- mode diagnostic servo ;
- affichage du pattern en cours.

## 9. Validation JSON côté ESP

Le firmware doit refuser un JSON invalide.

Contrôles minimaux :

- JSON bien formé ;
- présence d’un champ `schema` ;
- présence d’un tableau `patterns` ;
- chaque pattern possède un `id` unique ;
- chaque pattern possède au moins une étape ;
- chaque step possède un `type` valide ;
- `duration_ms` présent et supérieur au minimum ;
- `x/y` présents pour `move`, `hold`, `jitter`, `off_move` ;
- `x/y` dans la plage `-1.0 → +1.0` ;
- `amplitude` présent pour `jitter` ;
- pas de pattern trop long si limite mémoire ou timing.

Contrôles comportementaux recommandés :

- refuser les mouvements visibles inférieurs à 1000 ms ;
- refuser les pauses visibles inférieures à 1000 ms sauf exception ;
- refuser les jitter trop rapides ;
- limiter les disparitions trop longues ;
- limiter les sauts de coordonnées trop importants si la durée est faible.

## 10. Moteur d’exécution ESP

Le moteur d’exécution doit fonctionner selon cette logique :

```text
chargement JSON
validation
conversion en structure interne
sélection pattern
transition invisible vers début pattern
exécution ordonnée des steps
pause respiratoire
sélection pattern suivant
```

Le moteur doit éviter :

- parsing JSON pendant les mouvements ;
- allocation mémoire dynamique répétée ;
- changement de pattern en plein step ;
- laser ON pendant une transition non prévue ;
- servo jump visible entre deux patterns.

## 11. Sélection des patterns

Modes prévus :

| Mode | Description |
|---|---|
| `weighted_random` | tirage pondéré par `weight` |
| `sequence` | lecture dans un ordre défini |
| `single` | boucle sur un pattern précis |
| `test` | exécution d’un pattern demandé depuis l’interface web |

Mode par défaut recommandé :

```text
weighted_random + capture régulière
```

Le système doit éviter de rejouer trop souvent le même pattern.

Règles possibles :

- pas deux fois le même pattern d’affilée ;
- forcer un pattern `capture` tous les N patterns ;
- réduire temporairement le poids des patterns nerveux après utilisation.

## 12. Sécurité comportementale

Le laser doit être éteint :

- au boot logiciel ;
- en cas de JSON invalide ;
- en cas d’erreur moteur ;
- pendant les transitions invisibles ;
- en pause système ;
- en fin de session.

Le firmware doit prévoir un timeout général de session.

Exemple :

```text
Arrêt automatique après 5 à 10 minutes.
```

Le système ne doit jamais forcer un laser ON permanent sans mouvement ou sans limite de temps.

## 13. Outil Python de visualisation

### 13.1 Objectif

Le logiciel Python sert à visualiser le chemin du laser avant envoi sur l’ESP.

Il doit permettre de vérifier :

- la trajectoire ;
- les pauses ;
- les disparitions ;
- les réapparitions ;
- la densité temporelle ;
- la vitesse apparente ;
- les patterns trop nerveux.

### 13.2 Fonctions minimales

Le logiciel Python doit :

- ouvrir un fichier JSON ;
- parser les patterns ;
- lister les patterns disponibles ;
- afficher la trajectoire 2D d’un pattern ;
- différencier les segments laser ON et OFF ;
- représenter les pauses ;
- afficher la durée totale du pattern ;
- signaler les steps trop rapides ;
- signaler les coordonnées hors plage.

### 13.3 Fonctions visuelles recommandées

Représentation :

- segment visible : ligne continue ;
- segment invisible : ligne pointillée ;
- pause visible : point ou cercle ;
- jitter : zone circulaire autour du point ;
- début : marqueur spécifique ;
- fin : marqueur spécifique.

L’outil peut aussi afficher :

- durée de chaque step ;
- vitesse relative ;
- heatmap des zones les plus utilisées ;
- timeline laser ON/OFF.

### 13.4 Fonctions optionnelles

Fonctions utiles ensuite :

- lecture animée du pattern ;
- export PNG ;
- export GIF ou MP4 ;
- édition simple des points ;
- ralentissement global ;
- normalisation automatique ;
- upload direct vers l’ESP via HTTP.

## 14. Flux complet cible

Flux utilisateur complet :

```text
création / modification JSON
visualisation Python
correction si nécessaire
upload via interface web ESP
validation côté ESP
enregistrement
activation
exécution par le moteur de patterns
```

Flux minimal viable :

```text
JSON local
upload formulaire web
validation ESP
enregistrement active.json
redémarrage moteur
lecture pattern
```

## 15. Format JSON cible

Structure fonctionnelle recommandée :

```json
{
  "schema": "laser_cat_patterns.v1",
  "meta": {
    "name": "default_pack",
    "version": 1,
    "author": "local",
    "description": "Pack de patterns pour CatChat ESP"
  },
  "coordinate_system": {
    "type": "normalized",
    "x_min": -1.0,
    "x_max": 1.0,
    "y_min": -1.0,
    "y_max": 1.0
  },
  "runtime": {
    "selection_mode": "weighted_random",
    "capture_every": 5,
    "transition_ms_min": 1500,
    "transition_ms_max": 3500,
    "pause_between_patterns_ms_min": 3000,
    "pause_between_patterns_ms_max": 8000,
    "jitter_interval_ms": 600,
    "speed_scale": 1.0
  },
  "patterns": [
    {
      "id": "mouse_cautious",
      "name": "Souris prudente",
      "category": "prey_simulation",
      "weight": 45,
      "intensity": "low",
      "steps": [
        {
          "type": "hold",
          "laser": true,
          "x": -0.25,
          "y": -0.15,
          "duration_ms": 3000
        },
        {
          "type": "move",
          "laser": true,
          "x": -0.05,
          "y": -0.12,
          "duration_ms": 6000
        }
      ]
    }
  ]
}
```

## 16. Décisions fonctionnelles recommandées

Décisions à figer pour stabiliser le projet :

| Sujet | Décision recommandée |
|---|---|
| Format utilisateur | JSON |
| Format runtime ESP | structure interne compacte générée depuis JSON |
| Parsing JSON | au chargement seulement |
| Visualisation | outil Python externe |
| Upload | formulaire web HTTP |
| Stockage | fichier actif + fichiers uploadés optionnels |
| Mouvement | patterns lents, aérés, orientés proie |
| Sécurité laser | OFF par défaut, OFF sur erreur |
| Premier objectif | MVP upload + validation + exécution active.json |

## 16.1 Décision d'implémentation actuelle

L'implémentation firmware actuelle utilise une stratégie hybride :

- `tools/examples/default_patterns.json` reste la source utilisateur du pack par défaut ;
- `tools/generate_default_patterns_c.py` convertit ce JSON en `src/default_patterns.c` ;
- l'ESP démarre sur ce pack compilé compact ;
- `/patterns` permet de choisir un pattern, régler la vitesse et télécharger le pack actif ;
- l'upload JSON firmware est désactivé par défaut pour préserver la stabilité du serveur web ;
- aucun filesystem persistant n'est encore monté côté ESP8266.

Conséquence : le flux fiable actuel est édition/validation avec l'outil Python,
puis régénération du pack C compilé qui reste le fallback sûr au boot.

## 17. MVP

Le MVP doit livrer uniquement :

1. Un format JSON stable.
2. Un firmware capable de charger un JSON actif.
3. Une interface web simple avec upload.
4. Une validation JSON minimale.
5. Un moteur capable d’exécuter `hold`, `move`, `off_hold`, `off_move`.
6. Un visualiseur Python capable d’afficher les trajectoires ON/OFF.

Le `jitter`, les packs multiples, l’éditeur web et l’upload Python direct peuvent venir après.

## 18. Hors périmètre initial

Non inclus dans la première version :

- Alexa ;
- cloud ;
- application mobile ;
- éditeur graphique web complet ;
- authentification avancée ;
- OTA ;
- génération automatique de patterns par IA ;
- calibration automatique caméra.

Ces fonctions peuvent être ajoutées ensuite, mais ne doivent pas polluer la base fonctionnelle JSON + moteur + visualisation.
