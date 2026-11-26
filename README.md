# Storm Connections — Online Crash Fix & Netcode Overhaul (v2.9)

Correctif de crash en ligne et refonte du netcode pour **NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS** (PC / Steam).

> [!IMPORTANT]
> **Version du jeu requise : 1.71** (exécutable `NSUNSC.exe` Steam, Windows x64).
> Le mod cible des adresses précises de l'exécutable. Sur toute autre version, les correctifs dont le code ne correspond plus sont automatiquement désactivés (voir [Précautions](#️-précautions--limites)).

> [!NOTE]
> Ce mod **n'est pas du rollback**. Le jeu reste en *lockstep* (les deux machines avancent image par image en attendant les entrées de l'adversaire). Le mod corrige les défauts de ce lockstep : délai d'entrée surévalué, absence de redondance des paquets, gels inutiles, crashs.

---

## Sommaire

- [Nouveautés de la v2.9](#-nouveautés-de-la-v29)
- [Fonctionnalités](#-fonctionnalités)
- [Comment ça fonctionne](#-comment-ça-fonctionne)
- [Hôte, client et compatibilité](#-hôte-client-et-compatibilité)
- [Installation](#-installation)
- [Vérifier que le mod est actif](#-vérifier-que-le-mod-est-actif)
- [Configuration (`netcode_config.ini`)](#️-configuration-netcode_configini)
- [Précautions & limites](#️-précautions--limites)
- [Dépannage](#-dépannage)
- [Désinstallation](#-désinstallation)
- [Compiler depuis les sources](#-compiler-depuis-les-sources)
- [Licence & avertissement](#-licence--avertissement)

---

## 🆕 Nouveautés de la v2.9

Mise à jour majeure axée sur la **cadence 60,000 Hz exacte** (avec rattrapage de dérive d'horloge `ClockSync` et suppression du judder VSync sur écrans 144/165 Hz), le **bouclier anti-gigue dynamique en plein match**, l'**élimination de nouvelles familles de crashs en ligne** (`ccTaskManager`, VFX `m_pCoord`, presets `SpSkill`, recherche d'ID, `DestroySockets`) et le **durcissement complet des sessions personnalisées**. Le détail complet est dans le [`CHANGELOG`](CHANGELOG.md).

- **Cadence 60,000 Hz exacte, `ClockSync` & écrans 144/165 Hz** : nouveau `WaitableTimer` haute résolution (`EnablePrecisionFramePacer = 1`), déverrouillage du Cœur CPU 0, synchronisation fine d'horloge entre les deux PC, et contournement automatique du battement VSync en fenêtré / sans bordure sur les moniteurs hors multiples de 60 Hz (`BypassNon60HzVsyncJudder = 1`).
- **Bouclier anti-gigue en direct & filtre RTT à froid** : passage automatique en protection maximale (`Redundancy = 4`, `Spin = 1600 µs`) lors d'une rafale Wi-Fi en plein round puis retour au calme après 18 s stables, filtrage des pics de ping initiaux du relais Steam SDR, suppression du gel lockstep `checksum == 0`, et propagation `'NM'` du statut `2/2` moddé aux combattants clients en lobby spectateur.
- **Nouvelle armure anti-crash (`59/59 hooks` + 7 ponts VFX W^X)** : protection contre l'invalidation d'itérateur `ccTaskManager` (`RemoveTask` en cours de parcours), les 7 sites de crash VFX `m_pCoord == NULL`, les presets d'équipe `SpSkill` hors bornes, le mismatch `DestroySockets`, et le crash de fermeture de la recherche d'ID (+ saisie directe des chiffres sur clavier **AZERTY**).
- **Sessions & « Merci de patienter » durcis** : chien de garde continu sur l'interface de lobby et `WaitDlgClose` (même sous `BattleState > 0`), fermeture automatique des lobbies « zombies » tombés à 0 membre, purge des cartes fantômes `BattleTable` et compactage strict des 8 slots de session.

---

## ✨ Fonctionnalités

| # | Fonctionnalité | Effet |
| :-- | :--- | :--- |
| 1 | **Correctifs des crashs & blocages en ligne** (`0xC0000005` / `MSG_WaitMoment`) | Supprime les crashs liés aux pseudos Steam (`NSUNSC.exe+0x10BAD24`, `+0x0847A67`), au pool de renvoi (`+0x10F1455`, `+0x10F2928`), au lobby personnalisé (`+0x113D7F2`, dédoublement de slots, déconnexion brutale), à la fenêtre de chat rapide (`+0x0150CC8`) et débloque la fenêtre *« Merci de patienter »* figée en fin de match (`+0x06C1CF0`). |
| 2 | **Délai d'entrée « aller simple » (OWD)** | Remplace la formule du jeu (`1,25 × ping` complet) par la latence réelle aller simple (`ping / 2`) + une petite marge anti-gigue. Jusqu'à **1 frame (16,7 ms)** entre deux joueurs équipés du mod. |
| 3 | **Négociation automatique (handshake `NC`)** | Les deux jeux se reconnaissent entre eux et se mettent d'accord sur le même délai. Reste compatible avec les joueurs sans mod. |
| 4 | **Redondance des paquets UDP** | Chaque paquet contient aussi les 3 frames précédentes : si un paquet se perd, le suivant comble le trou sans gel. |
| 5 | **Récupération rapide des pertes (Fast NACK)** | Redemande les frames manquantes après 2 frames de blocage (33 ms), avec relance espacée selon le vrai ping pour ne jamais saturer l'adversaire. |
| 6 | **Rendu DirectX 11 sans file d'attente** | Force `SetMaximumFrameLatency(1)` : environ **-33 ms** (2 frames) de latence d'affichage locale, en ligne comme hors ligne. |
| 7 | **Boost du planificateur Windows & Pacer 60,000 Hz** | Désactive EcoQoS / Timer-Coalescing (Windows 11), timer noyau à 0,5 ms, `WaitableTimer` haute résolution à 60,000 Hz et priorité MMCSS « Games » pour un rythme d'images ultra-stable (y compris sur écrans 144/165 Hz). |
| 8 | **Mode Wi-Fi gaming** | Bloque le scan Wi-Fi de fond de Windows (toutes les ~65 s, source de pics de ping) pendant que le jeu tourne. Restauré automatiquement à la fermeture. |
| 9 | **Auto-ajustement en revanche** | Si le match précédent contre le même adversaire a eu trop de micro-gels (≥ 1,8 % / ≥ 12 %), ajoute +1 / +2 frames de marge au match suivant (+1 max en 30 FPS). |
| 10 | **Masquage de la fenêtre « Communication en cours... »** | La fenêtre sombre n'apparaît plus sur les micro-coupures (< 3 s). |

---

## 🔧 Comment ça fonctionne

### Chargement (proxy DLL)

Le jeu charge au démarrage la bibliothèque Microsoft `d3dcompiler_47.dll`. Le mod se présente sous le même nom :

1. `NSUNSC.exe` charge **notre** `d3dcompiler_47.dll` depuis son dossier.
2. Notre DLL charge la **vraie** `d3dcompiler_47.dll` et lui relaie les 29 fonctions d'origine : le rendu du jeu n'est pas modifié. Ordre de recherche :
   1. `d3dcompiler_47_original.dll` dans le dossier du jeu,
   2. `d3dcompiler_47_o.dll` dans le dossier du jeu,
   3. `C:\Windows\System32\d3dcompiler_47.dll` (présente sur tout Windows 10/11).
3. Elle lit `netcode_config.ini` (le recrée avec les valeurs par défaut s'il est absent), puis installe ses correctifs en mémoire avec [MinHook](https://github.com/TsudaKageyu/minhook).

**Aucun fichier du jeu n'est modifié sur le disque.** Tout se passe en mémoire, pendant que le jeu tourne.

### Sécurité de l'installation des correctifs

Avant de toucher une fonction du jeu, le mod compare ses premiers octets avec ceux attendus pour la version 1.71 :

- **59 hooks** (1 rendu + 2 timer + 38 crash-fix/session/tâches/presets + 13 netcode + 5 wait-dialog), **9 patchs d'octets**, **1 pont de tri/slot lobby** et **7 ponts VFX W^X**, vérifiés individuellement.
- Si les octets ne correspondent pas (autre version du jeu), ce correctif est **ignoré** au lieu de risquer un crash.

### Le problème du netcode d'origine

Le jeu calcule le délai d'entrée à partir du ping complet multiplié par 1,25, alors qu'une entrée n'a besoin que d'un aller simple pour arriver chez l'adversaire.

| Ping (aller-retour) | Délai du jeu d'origine | Délai avec le mod |
| :---: | :---: | :---: |
| 35 ms | 4 frames (67 ms) | 2 frames (33 ms) |
| 65 ms | 7 frames (117 ms) | 3 frames (50 ms) |
| 95 ms | 9 frames (150 ms) | 4 frames (67 ms) |
| 115 ms | 10 frames (167 ms) | 5 frames (83 ms) |
| 143 ms | 13 frames (217 ms) | 6 frames (100 ms) |

*(1 frame = 16,67 ms à 60 FPS. Plancher : 1 frame entre deux joueurs moddés, 2 frames contre un joueur sans mod.)*

### Négociation du délai

En lockstep, **les deux machines doivent utiliser exactement le même délai**, sinon le combat gèle dès les premières frames. Avant chaque combat, le jeu échange des mesures de ping, et c'est **l'hôte** qui diffuse la valeur finale :

1. Le mod ajoute une signature `NC` dans des octets inutilisés de ces paquets.
2. **Si l'hôte a le mod**, il calcule le délai optimal et l'envoie sous forme d'un « ping synthétique » choisi pour qu'un client **sans mod**, avec sa formule d'origine, retombe exactement sur le même nombre de frames.
3. **Si les deux ont le mod**, le plancher de 1 frame est déverrouillé.

---

## 🤝 Hôte, client et compatibilité

C'est le jeu (via les lobbies Steam) qui décide qui est l'hôte : en général, **celui qui a créé le lobby** (ou qui attendait en entraînement pendant la recherche). Le mod ne peut pas forcer ce rôle.

| Vous | Adversaire | Délai appliqué | Autres améliorations |
| :--- | :--- | :--- | :--- |
| Moddé (hôte **ou** client) | **Moddé** | ✅ Optimal, jusqu'à 1 frame | ✅ Toutes |
| Moddé, **hôte** | Sans mod | ✅ Optimal, minimum 2 frames | ✅ Toutes |
| Moddé, **hôte spectateur** | 2 joueurs, moddés ou non | ✅ Délai optimal imposé aux deux combattants (minimum 3 frames si aucun n'est moddé) | ✅ Toutes |
| Moddé, **client** | Sans mod, hôte | ⚠️ Délai d'origine du jeu (imposé par l'hôte) | ✅ Crash-fix, redondance, rendu, Wi-Fi |

> [!TIP]
> Pour profiter pleinement du mod, **partagez-le avec vos partenaires de jeu** : entre deux joueurs moddés, le délai optimal s'applique quel que soit l'hôte.

---

## 📥 Installation

### Prérequis

- Windows 10 ou 11 (64 bits).
- NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS **version 1.71** sur Steam.
- Rien d'autre : la DLL utilise les mêmes runtimes Visual C++ que le jeu.

### Étapes

1. Dans **Steam**, clic droit sur *NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS* → **Gérer** → **Parcourir les fichiers locaux**.
2. Si un fichier `d3dcompiler_47.dll` existe déjà dans ce dossier, **renommez-le** en :
   ```
   d3dcompiler_47_original.dll
   ```
   Le mod le chargera en priorité (sinon il utilise celui de Windows, ce qui fonctionne aussi).
3. Copiez ces **2 fichiers** dans ce dossier, **à côté de `NSUNSC.exe`** :
   - `d3dcompiler_47.dll`
   - `netcode_config.ini`
4. Lancez le jeu normalement depuis Steam.

Arborescence attendue :

```
NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS\
├── NSUNSC.exe
├── steam_api64.dll
├── d3dcompiler_47.dll            ← le mod
├── d3dcompiler_47_original.dll   ← l'ancienne DLL renommée (si elle existait)
└── netcode_config.ini            ← la configuration
```

### Steam Deck / Linux (Proton)

Proton ignore par défaut les DLL placées à côté du jeu. Ajoutez dans **Propriétés → Options de lancement** :

```
WINEDLLOVERRIDES="d3dcompiler_47=n,b" %command%
```

---

## ✅ Vérifier que le mod est actif

Au lancement, une **console noire** s'ouvre à côté du jeu (désactivable, voir plus bas). Les messages s'écrivent aussi dans `storm_netcode.log`, dans le dossier du jeu.

| Message | Signification |
| :--- | :--- |
| `[Init] 59/59 .pdata-verified hooks active.` | Tout est actif. **Moins de 59** = version du jeu différente de 1.71 (ou option désactivée dans le `.ini`). |
| `[LowLatency] DXGI MaxFrameLatency: 3 -> 1 f ...` | Rendu sans file d'attente actif (s'affiche au premier rendu). |
| `[Scheduler] Gaming Boost active: ...` | Boost du planificateur Windows actif. |
| `[WiFi-AntiLag] Active on "..."` | Mode Wi-Fi gaming actif (uniquement si vous êtes en Wi-Fi). |
| `[Handshake] Remote client has StormNetcode mod` | Vous êtes l'hôte et l'adversaire a le mod. |
| `[Handshake] Host has StormNetcode mod! ... Agreed Delay=N frames` | Vous êtes client et l'hôte a le mod. |
| `[Handshake] Host is unmodded` | Vous êtes client face à un hôte sans mod (délai d'origine). |
| `[RTT-Negotiation] Host authoritative: ... TargetDelay=N f` | Vous êtes l'hôte et imposez le délai optimal. |
| `[CalcNetFrameByRtt] ... vanilla=X frames \| active=Y frames` | Délai du jeu d'origine comparé au délai réellement appliqué. |

---

## ⚙️ Configuration (`netcode_config.ini`)

Ouvrez le fichier avec le Bloc-notes. `1` = activé, `0` = désactivé. **Les valeurs par défaut sont recommandées.** Si le fichier est supprimé, il est recréé avec ces valeurs au prochain lancement.

### `[General]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `EnableConsole` | `1` | Affiche la console de diagnostic. Mettez `0` pour la cacher. |
| `EnableFileLog` | `1` | Écrit le journal `storm_netcode.log`. |
| `EnableNetcodeOverhaul` | `1` | Active toute la refonte du netcode (délai, redondance, NACK...). |

### `[CrashFix]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `FixOnlinePersonaCrash` | `1` | Correctifs des crashs et blocages en ligne. **À laisser activé.** |
| `FixLobbyWaitDeadlock` | `1` *(optionnel)* | Déblocage automatique de la fenêtre *« Merci de patienter »* en session (`MSG_WaitMoment`). |

### `[InputDelay]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `EnableCustomDelay` | `1` | Active le calcul de délai du mod. |
| `DelayMode` | `1` | `1` = négocié et compatible avec tous (**recommandé**). `2` = forcé, uniquement si **les deux joueurs** ont le mod **et des réglages identiques**. `0` = délai d'origine du jeu (les autres correctifs restent actifs). |
| `ForcedDelayFrames` | `-1` | `-1` = adaptatif selon le ping. `1` à `16` = délai fixe. |
| `MinDelayFrames` | `1` | Délai minimal (1 frame = 16,67 ms). Contre un joueur sans mod, le plancher reste 2. |
| `MaxDelayClamp` | `16` | Délai maximal autorisé. |
| `JitterMarginMs` | `2` | Marge de sécurité anti-gigue (en ms), plus garde de route (`+3 ms + RTT/13` à 60 FPS) automatiquement. |

### `[LowLatencyRender]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `EnableLowLatencyRender` | `1` | Rendu DirectX 11 sans file d'attente. |
| `MaxFrameLatency` | `1` | Taille de la file d'images (1 à 3, Windows utilise 3 par défaut). |
| `EnableSchedulerBoost` | `1` | Timer noyau 0,5 ms, MMCSS « Games », désactivation d'EcoQoS. |
| `EnablePrecisionFramePacer` | `1` | Cadence exacte à 60,000 Hz (`WaitableTimer` haute résolution + micro-spin QPC + pilotage de dérive d'horloge). |
| `BypassNon60HzVsyncJudder` | `1` | Désactive l'attente VBlank DXGI en fenêtré / sans bordure sur les écrans hors multiples de 60 Hz (75, 100, 144, 165 Hz) lorsque le pacer 60 Hz est actif. |

### `[PacketRedundancy]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `EnablePacketRedundancy` | `1` | Envoie les frames précédentes dans chaque paquet. |
| `RedundancyFrameDepth` | `3` | Nombre de frames incluses par paquet (1 à 4 côté combattant, 1 à 5 côté spectateur). |
| `MaxResendBatch` | `3` | Frames renvoyées par lot après une perte (1 à 4). |

### `[JitterAndStallGovernance]`

| Clé | Défaut | Description |
| :--- | :---: | :--- |
| `EnableWifiGamingMode` | `1` | Bloque le scan Wi-Fi de fond pendant le jeu. |
| `EnableDynamicPeerAutoTune` | `1` | Ajoute +1 / +2 frames en revanche si le match précédent a eu trop de gels. |
| `EnableMicroJitterPoll` | `1` | Attente fine des paquets à chaque frame pour absorber la micro-gigue. |
| `MicroJitterSpinUs` | `1100` | Durée de cette attente (µs, 0 à 2000). |
| `FastNackRecovery` | `1` | Redemande rapide des frames perdues. |
| `SuppressShortOverlay` | `1` | Cache la fenêtre « Communication en cours... » sur les coupures courtes. |
| `OverlayThresholdFrames` | `180` | Durée avant d'afficher cette fenêtre (180 frames = 3 s). |

---

## ⚠️ Précautions & limites

> [!WARNING]
> **Mises à jour du jeu.** Le mod est conçu pour la **version 1.71**. Après un patch officiel, les correctifs dont le code a changé sont désactivés automatiquement (la console affiche moins de `59/59`). Le jeu reste jouable, mais le mod ne fonctionne plus qu'en partie. Il faudra attendre une nouvelle version du mod.

- **Steam peut retirer le mod.** « Vérifier l'intégrité des fichiers du jeu » ou une mise à jour peut remplacer `d3dcompiler_47.dll` par l'original. Recopiez alors la DLL du mod.
- **Antivirus.** La DLL n'est pas signée et modifie le comportement du jeu en mémoire : certains antivirus (dont Windows Defender) peuvent la signaler. Si vous avez un doute, compilez-la vous-même depuis les sources.
- **Source fiable uniquement.** Ne téléchargez la DLL que depuis la [page Releases de ce dépôt](https://github.com/Dali999999999/Storm-Connections-Online-Crash-Fix-Netcode-Overhaul/releases). Chaque release est compilée automatiquement par GitHub Actions à partir du code public et accompagnée de son empreinte SHA-256. Une DLL modifiée par un tiers peut être malveillante.
- **Ce n'est pas du rollback.** Une connexion instable (Wi-Fi saturé, pertes importantes, très longue distance) provoquera toujours des ralentissements ; le mod les réduit, il ne les supprime pas.
- **Le rôle d'hôte n'est pas contrôlable.** Contre un hôte sans mod, vous subissez le délai d'origine du jeu (voir [Hôte, client et compatibilité](#-hôte-client-et-compatibilité)).
- **`DelayMode = 2`** : à n'utiliser qu'entre deux joueurs moddés avec des réglages identiques. Sinon, risque de gel en début de combat.
- **Réglages système temporaires.** Le mode Wi-Fi, le timer 0,5 ms et les priorités sont appliqués **uniquement pendant que le jeu tourne** et restaurés à sa fermeture. En cas de crash du jeu, un redémarrage de Windows remet tout par défaut.
- **Usage en ligne.** Le mod ne modifie aucune donnée de jeu (personnages, dégâts, progression) et ne donne aucun avantage de gameplay. Mais il modifie un jeu en ligne : **utilisation à vos propres risques.**

---

## 🩺 Dépannage

| Symptôme | Solution |
| :--- | :--- |
| Aucune console au lancement | Vérifiez que `d3dcompiler_47.dll` est bien **à côté de `NSUNSC.exe`** et que `EnableConsole = 1`. Sous Proton, ajoutez l'option de lancement `WINEDLLOVERRIDES`. |
| `X/59 hooks active` avec X < 59 | La version du jeu n'est pas la 1.71 (ou une option de correctif a été désactivée dans `netcode_config.ini`). |
| Le jeu ne démarre plus | Supprimez `d3dcompiler_47.dll` et renommez `d3dcompiler_47_original.dll` en `d3dcompiler_47.dll` (ou vérifiez l'intégrité des fichiers sur Steam), puis signalez le problème avec votre `storm_netcode.log`. |
| Gel au tout début du combat | Remettez `DelayMode = 1` et les valeurs par défaut (supprimez le `.ini` pour qu'il soit régénéré). |
| Saccades fréquentes en combat | Augmentez `JitterMarginMs` (par ex. `12` ou `16`), ou passez en câble Ethernet. |
| La DLL disparaît | Steam l'a remplacée (mise à jour ou vérification) : recopiez-la. |

Pour signaler un bug, joignez le fichier **`storm_netcode.log`** (dossier du jeu) à votre ticket.

---

## 🗑️ Désinstallation

1. Supprimez du dossier du jeu : `d3dcompiler_47.dll`, `netcode_config.ini` et `storm_netcode.log`.
2. Si vous aviez renommé l'ancienne DLL, renommez `d3dcompiler_47_original.dll` en `d3dcompiler_47.dll`.

Ou simplement : Steam → clic droit sur le jeu → **Propriétés → Fichiers installés → Vérifier l'intégrité des fichiers du jeu**.

---

## 🛠️ Compiler depuis les sources

Prérequis : **Visual Studio 2022 ou plus récent** (outils C++ « Développement Desktop en C++ »), **CMake ≥ 3.20** et **Git**.

```powershell
git clone --recurse-submodules https://github.com/Dali999999999/Storm-Connections-Online-Crash-Fix-Netcode-Overhaul.git
cd Storm-Connections-Online-Crash-Fix-Netcode-Overhaul
cmake -B build -A x64
cmake --build build --config Release
```

> [!NOTE]
> Si vous avez cloné sans `--recurse-submodules`, récupérez MinHook avec `git submodule update --init`.

La DLL est générée dans `build\Release\d3dcompiler_47.dll`. Copiez-la avec `netcode_config.ini` dans le dossier du jeu.

Structure du dépôt :

| Fichier | Rôle |
| :--- | :--- |
| `CMakeLists.txt` | Script de compilation (DLL x64 `d3dcompiler_47.dll`). |
| `d3dcompiler_47.def` | Table des 29 exports (`@1` à `@29`). |
| `netcode_config.ini` | Configuration par défaut livrée avec la DLL. |
| `include/netcode_mod.h` | Adresses (RVA) du jeu 1.71, structures internes et configuration partagée. |
| `src/proxy_d3dcompiler.cpp` | Proxy : charge la vraie `d3dcompiler_47.dll` et relaie ses 29 exports. |
| `src/dllmain.cpp` | Point d'entrée, lecture/génération du `.ini`, console et journal. |
| `include/netcode_internal.h` | Déclarations partagées entre les modules. |
| `src/crash_fixes.cpp` | Correctifs de crash (pseudos Steam, pool de renvoi, lobby, déconnexions brutales) et patchs d'octets vérifiés. |
| `src/lobby_wait_fix.cpp` | Déblocage de la barrière post-match en session (`CtrlState_MoveLoser` / *« Merci de patienter »*). |
| `src/system_tuning.cpp` | Rendu DirectX 11, planificateur Windows, mode Wi-Fi gaming. |
| `src/rtt_negotiation.cpp` | Calcul du délai, négociation hôte/client, mise en place de la synchronisation. |
| `src/packet_redundancy.cpp` | Redondance UDP, flux spectateur, pré-chauffage du canal de combat. |
| `src/netcode_hooks.cpp` | Attente fine des paquets, Fast NACK, gouvernance du combat, installation des hooks. |
| `minhook/` | Bibliothèque de hooking [MinHook](https://github.com/TsudaKageyu/minhook) (sous-module Git, licence BSD-2). |

---

## 📜 Licence & avertissement

Ce projet est distribué sous licence **[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0)** (voir [`LICENSE`](LICENSE)), avec une politique d'attribution complémentaire (voir [`NOTICE.md`](NOTICE.md)). MinHook reste sous sa propre licence BSD-2 (`minhook/LICENSE.txt`).

| Situation | Autorisé ? |
| :--- | :---: |
| Télécharger et utiliser gratuitement | ✅ |
| Modifier le code | ✅ |
| Publier un fork sur son propre GitHub | ✅ |
| Donner un autre nom à son fork | ✅ |
| Améliorer et redistribuer gratuitement | ✅ |
| Conserver ses propres modifications | ✅ |
| Vendre le logiciel ou un fork | ❌ |
| Commercialiser les améliorations | ❌ |
| Supprimer les crédits du projet original | ❌ |
| Faire croire qu'on est l'auteur original | ❌ |

Projet communautaire non officiel, **sans aucun lien avec Bandai Namco Entertainment ni CyberConnect2**. *NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS* et toutes les marques associées appartiennent à leurs propriétaires respectifs.

Ce logiciel est fourni « tel quel », sans garantie d'aucune sorte. Les auteurs ne sont pas responsables des éventuels dommages, pertes de données ou sanctions liés à son utilisation.
