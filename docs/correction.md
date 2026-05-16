# correction

## Objectif

Stabiliser l’application côté comportement embarqué, moteur de jeu, cohérence runtime/documentation et robustesse produit. Les sujets sécurité, OTA, authentification, exposition réseau et accès aux pages/endpoints sont volontairement exclus : usage maison assumé.

---

## P0 — Corrections prioritaires

### 1. Ajouter une limite de session + cooldown

Problème : le jeu peut tourner indéfiniment.

Risque : surstimulation du chat, frustration accrue, comportement obsessionnel autour du point rouge.

Correction attendue :

- définir une durée maximale de session ;
- couper automatiquement le laser et les servos à la fin ;
- imposer un cooldown avant relance ;
- exposer un état runtime clair : `idle`, `running`, `cooldown`, `debug`.

Paramètres recommandés :

```c
#define GAME_SESSION_MAX_MS   (10 * 60 * 1000)  // 10 min
#define GAME_COOLDOWN_MS      (5  * 60 * 1000)  // 5 min
```

Comportement cible :

- démarrage d’une session bornée ;
- fin de session → laser OFF + servo repos + cooldown ;
- arrêt manuel → coupure immédiate du jeu et du laser ;
- relance bloquée pendant cooldown.

---

### 2. Corriger l’arrêt servo

Problème : à l’arrêt, le code envoie la position de repos puis désactive immédiatement les servos. Le servo n’a probablement pas le temps de revenir physiquement au centre.

Correction attendue :

- envoyer la consigne de repos ;
- garder les pulses servo actifs pendant un court délai ;
- couper les servos après stabilisation.

Comportement cible :

```c
hardware_reset_position();
vTaskDelay(pdMS_TO_TICKS(500));
hardware_servo_set_enabled(false);
```

À intégrer proprement sans bloquer une tâche critique si la logique actuelle ne le permet pas.

---

### 3. Encadrer le mode laser manuel

Problème : le laser peut être placé dans un état manuel continu.

Risque : laser laissé allumé trop longtemps, comportement non maîtrisé du jouet, échauffement inutile ou stimulation excessive.

Correction attendue :

- interdire l’état “laser ON permanent” côté logique métier ;
- remplacer par une impulsion courte ;
- extinction forcée après durée fixe ;
- laser OFF systématique à chaque changement d’état critique.

Comportement cible :

```c
laser_manual_pulse(duration_ms);
// extinction forcée après duration_ms
```

Durée indicative : 500 ms à 3000 ms selon usage debug/calibration.

---

## P1 — Corrections importantes

### 4. Corriger l’incohérence AP / documentation

Problème : la documentation indique que le firmware reste en `STATIONAP_MODE`, alors que le code désactive l’AP après connexion station.

Correction possible A : conserver réellement l’AP.

- rester en `STATIONAP_MODE` ;
- documenter que le portail reste accessible ;
- assumer la présence permanente de l’AP.

Correction possible B : assumer l’arrêt de l’AP.

- conserver le comportement actuel ;
- corriger la documentation ;
- indiquer clairement que l’AP disparaît une fois connecté au WiFi.

Choix recommandé : B, plus simple et plus propre pour un objet domestique.

---

### 5. Restreindre le DNS captif au mode AP

Problème : le DNS captif écoute globalement. S’il reste actif alors que l’AP est désactivé, il peut répondre dans un contexte où il n’a plus de rôle utile.

Correction attendue :

- ne lancer le DNS captif que lorsque l’AP est actif ;
- stopper le DNS quand l’AP est désactivé ;
- ignorer les requêtes DNS si l’AP n’est plus actif.

Comportement cible :

```c
if (!ap_active) {
    return; // pas de réponse DNS captive hors mode AP
}
```

---

### 6. Unifier le pattern `capture`

Problème : avec le pack par défaut, `find_capture_pattern()` retourne un ancien pattern codé en dur au lieu d’un pattern issu du JSON compilé.

Conséquence : le JSON ne décrit pas exactement ce que le firmware peut jouer.

Correction attendue :

- supprimer le pattern legacy codé en dur ;
- définir explicitement le pattern de capture dans `default_patterns.json` ;
- le générer dans `default_patterns.c` ;
- faire pointer `find_capture_pattern()` uniquement vers les patterns du pack actif.

Comportement cible :

```c
return pattern_pack_find_by_id(pack, "capture");
```

Fallback acceptable : si aucun pattern `capture` n’existe, utiliser le dernier pattern du pack ou désactiver la phase capture.

---

### 7. Ajouter des timeouts socket HTTP

Problème : serveur HTTP minimal, client unique, `recv()` potentiellement bloquant.

Risque : une connexion lente ou incomplète peut bloquer le serveur.

Correction attendue :

- ajouter `SO_RCVTIMEO` ;
- ajouter `SO_SNDTIMEO` ;
- fermer proprement les connexions incomplètes ;
- limiter la durée de traitement par client.

Exemple logique :

```c
struct timeval timeout = {
    .tv_sec = 3,
    .tv_usec = 0,
};
setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
```

Ce point est conservé uniquement comme robustesse runtime, pas comme sujet sécurité.

---

## P2 — Nettoyage et dette technique

### 8. Étendre le validateur de patterns

Ajouter des règles comportementales côté Python :

- durée totale maximale ;
- ratio laser ON/OFF ;
- distance maximale par étape ;
- vitesse angulaire maximale approximative ;
- répétitions trop longues ;
- patterns trop brusques ;
- nombre de points conforme à la cible ;
- poids dans une plage valide ;
- ID trop longs ou tronqués côté firmware.

But : détecter les mauvais patterns avant compilation.

---

### 9. Ajouter une CI PlatformIO minimale

Problème : les tests Python passent, mais la compilation firmware n’est pas garantie automatiquement.

Correction attendue :

- job Python : validation JSON + tests pytest ;
- job PlatformIO : build firmware ;
- génération contrôlée de `default_patterns.c` ;
- échec CI si le fichier généré n’est pas à jour.

---

### 10. Corriger les casts `ARRAY_SIZE` vers `uint8_t`

Problème potentiel : si le nombre de patterns ou de points dépasse 255, cast silencieux.

Correction attendue :

- utiliser `size_t` ou `uint16_t` côté structures ;
- ajouter assertions compile-time ;
- faire échouer la génération si dépassement.

---

### 11. Nettoyer les patterns legacy

Problème : présence de patterns codés en dur qui contredisent le principe JSON → C.

Correction attendue :

- supprimer les anciens patterns internes ;
- centraliser tous les patterns dans `default_patterns.json` ;
- garder uniquement des fallbacks très courts et explicites si nécessaire.

---

### 12. Séparer mode normal et mode debug hardware

Problème : les fonctions de calibration ou de test matériel sont mélangées au comportement normal.

Correction attendue :

- ajouter un flag de compilation `DEBUG_HARDWARE_ENABLED` ;
- isoler les fonctions servo test, laser pulse et calibration ;
- limiter les actions debug par timeout ;
- empêcher tout état laser permanent, même en debug.

Exemple :

```c
#if DEBUG_HARDWARE_ENABLED
// servo test, laser pulse, calibration
#endif
```

---

## Ordre d’exécution recommandé

1. Ajouter limite de session + cooldown.
2. Corriger l’arrêt servo.
3. Encadrer le mode laser manuel.
4. Corriger AP/doc + DNS captif.
5. Unifier le pattern `capture`.
6. Ajouter timeouts HTTP.
7. Étendre validation patterns.
8. Ajouter build PlatformIO en CI.
9. Corriger les casts et nettoyer les patterns legacy.
10. Séparer mode normal et debug hardware.

---

## Critères de validation

### Test session

- Une session démarre correctement.
- La session s’arrête seule à la durée maximale.
- Le cooldown empêche une relance immédiate.
- L’arrêt manuel coupe immédiatement le laser et le jeu.

### Test laser manuel

- Le laser ne peut pas rester actif indéfiniment.
- Le mode manuel fonctionne uniquement sous forme d’impulsion bornée.
- Le laser est forcé OFF lors des transitions d’état.

### Test servo

- À l’arrêt, les servos reçoivent bien la consigne de repos.
- Les pulses restent actifs assez longtemps pour atteindre le repos.
- Les servos sont ensuite désactivés.

### Test WiFi / AP

- Le comportement AP réel correspond à la documentation.
- L’AP disparaît ou reste actif selon le choix retenu.

### Test DNS

- DNS captif actif uniquement en mode AP.
- Aucune réponse captive hors contexte AP.

### Test patterns

- Le pattern `capture` vient du JSON compilé.
- Le comportement runtime correspond aux patterns générés.
- Le validateur rejette les patterns trop brusques ou trop longs.

### Test robustesse HTTP

- Une connexion lente ne bloque pas indéfiniment le serveur.
- Les timeouts ferment proprement les clients incomplets.

---

## Résultat attendu

Après ces corrections, l’application reste simple et adaptée à un usage maison, mais gagne en robustesse produit :

- pas d’état laser permanent ;
- session bornée ;
- arrêt matériel cohérent ;
- DNS captif propre ;
- patterns cohérents entre JSON, firmware et runtime ;
- serveur moins fragile face aux connexions incomplètes.

