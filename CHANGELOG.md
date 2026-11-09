# Changelog

Toutes les évolutions notables de ce projet sont documentées ici.

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
