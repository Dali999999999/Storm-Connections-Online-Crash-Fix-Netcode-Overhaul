# Changelog

Toutes les évolutions notables de ce projet sont documentées ici.

## [2.7] - 2026-11-12

**Jeu requis : NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS — version 1.71 (Steam, Windows x64).**

### Mode spectateur
- Fin des messages « connexion à l'adversaire perdue » et des micro-gels pendant le visionnage d'un match.
- Garde de chargement initial du spectateur (la phase d'attente du premier paquet n'entame plus le délai de déconnexion).
- Demandes de renvoi (NACK) cadencées et dédupliquées, redondance du flux Combattant → Spectateur.
- Correction de deux bugs du jeu d'origine côté spectateur (débordement non signé du compteur d'attente, compteur de blocage écrasé).

### Compatibilité universelle
- Toutes les combinaisons validées : moddé / non-moddé × hôte / client × 30 / 60 FPS, en matchmaking, partie personnalisée, tournoi et ligue.
- Un hôte moddé qui **spectate deux autres joueurs** (même non-moddés) calcule et impose désormais le délai optimal aux deux combattants.
- Détection correcte des rotations de combattants (roi de la colline / tournoi) via la paire de SteamID.

### Premier match & revanche
- Suppression des gels au premier match entre deux joueurs moddés (plancher de phase VSync, protection contre les renvois tardifs du ping client).

### Netcode & latence
- Courbe de délai à 3 paliers (2/2, 1/2, 0/2 combattants moddés) sans double pénalité de distance : par exemple 3 frames à 65 ms de ping contre 7 sans le mod.
- Mode 30 FPS : marge de phase dédiée (6 frames à 230 ms de ping au lieu de 5) et auto-ajustement plafonné à +1 frame.
- Anti-flood UDP : plus de paquets dupliqués pendant les gels (ceux-ci doublaient le débit sur Wi-Fi).
- Fast NACK cadencé sur le vrai ping : relance espacée d'un aller-retour, sans saturer la file de renvoi (16 emplacements) d'un adversaire sans mod.
- Pré-chauffage du canal Steam P2P de combat (canal 6) avant la frame 0 : plus de latence d'ouverture au premier paquet.
- Attente fine des paquets portée à 1100 µs (pas de 25 µs) pour absorber la gigue au passage du VSync.

### Stabilité
- Nouveau correctif de crash `0xC0000005` dans le lobby de partie personnalisée (`NSUNSC.exe+0x113D7F2`, spectateur ou joueur sans slot).
- 16 hooks et 5 patchs d'octets vérifiés individuellement au démarrage.

### Code source
- Refactorisation en 9 fichiers de 700 lignes maximum, sans changement de comportement.

### Installation
1. Renommez le `d3dcompiler_47.dll` existant du dossier du jeu en `d3dcompiler_47_original.dll` (s'il existe).
2. Copiez `d3dcompiler_47.dll` et `netcode_config.ini` à côté de `NSUNSC.exe`.
3. Lancez le jeu : la console doit afficher `[Init] 16/16 .pdata-verified hooks active.`

Si vous aviez déjà un `netcode_config.ini` de la v2.6, **remplacez-le** par celui de cette version (nouvelles valeurs par défaut).

## [2.6] - 2026-11-09

**Jeu requis : NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS — version 1.71 (Steam, Windows x64).**

### Correctifs de stabilité
- Suppression des crashs en ligne `0xC0000005` liés à la lecture des pseudos Steam (`NSUNSC.exe+0x10BAD24`, `+0x0847A67`).
- Suppression des crashs du pool de renvoi de paquets (`+0x10F1455`, `+0x10F2928`).

### Netcode
- Délai d'entrée calculé sur la latence aller simple (`ping / 2`) + marge anti-gigue, au lieu de `1,25 × ping` : jusqu'à **1 frame (16,7 ms)** entre deux joueurs équipés du mod.
- Négociation automatique `NC` entre joueurs moddés, avec ping synthétique pour rester compatible avec les joueurs sans mod quand vous êtes l'hôte.
- Redondance UDP (3 frames par paquet) et récupération rapide des pertes (Fast NACK).
- Auto-ajustement de la marge en revanche selon les micro-gels du match précédent.
- Masquage de la fenêtre « Communication en cours... » sur les coupures de moins de 3 s.

### Latence locale & système
- Rendu DirectX 11 sans file d'attente (`SetMaximumFrameLatency(1)`) : environ -33 ms d'input lag.
- Boost du planificateur Windows (timer 1 ms, MMCSS « Games », désactivation d'EcoQoS).
- Mode Wi-Fi gaming : blocage du scan Wi-Fi de fond pendant le jeu, restauré à la fermeture.

### Installation
1. Renommez le `d3dcompiler_47.dll` existant du dossier du jeu en `d3dcompiler_47_original.dll` (s'il existe).
2. Copiez `d3dcompiler_47.dll` et `netcode_config.ini` à côté de `NSUNSC.exe`.
3. Lancez le jeu : la console doit afficher `[Init] 14/14 .pdata-verified hooks active.`

Guide complet, configuration et précautions : voir le [README](https://github.com/Dali999999999/Storm-Connections-Online-Crash-Fix-Netcode-Overhaul#readme).
