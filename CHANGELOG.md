# Changelog

Toutes les évolutions notables de ce projet sont documentées ici.

## [2.8] - 2026-11-15

**Jeu requis : NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS — version 1.71 (Steam, Windows x64).**

### Déblocage de la fenêtre « Merci de patienter » en session (`MSG_WaitMoment`)
- **Correctif du deadlock `CtrlState_MoveLoser` (`+0x06C1CF0`) & `Signal_Receive` (`+0x00F9520`)** *(désactivable via `FixLobbyWaitDeadlock`)* :
  - Correction d'un bug d'origine où, en fin de match en session / partie personnalisée, l'écran *« Merci de patienter »* restait figé tant que tous les autres joueurs ne s'étaient pas déconnectés (l'hôte affichait `Timeout` après 60 s sans jamais relâcher la barrière `IsAllSignalWait`, et le client n'avait aucun timeout).
  - **Côté hôte** : au bout de 10 s si un pair n'a pas acquitté `Signal(1)`, exécution propre de la séquence de libération (`Signal(2)` + rotation `BattleTable`), débloquant simultanément l'hôte et les clients (y compris **sans mod**).
  - **Côté client** : résolution automatique sous 1,5 s du blocage de désordre `Signal(2)` / `BattleTable` (`+0x768=1, +0x764=0`), préservation d'un `Signal(2)` reçu juste avant `STEP_BEGIN`, et sortie sécurisée à 14 s (22 s si migration d'hôte Steam active) avec filtre anti-boucle contre les relances `Signal(0)` d'un hôte sans mod, plus fermeture de toute fuite de refcount du dialogue en `STEP_END`.

### Mode spectateur 60 FPS complet (fin du ralenti 30 FPS)
- **Relais spectateur principal débridé (`SendWatcherRelayPacket` à `+0x10F3D70`)** : le spectateur de tête ne dépend plus de son propre `m_currentSyncFrame` (bridé à 30 FPS s'il joue en 30 FPS), mais relaie immédiatement les frames confirmées (`m_confirmedAllPlayersFrame`) avec rafales de rattrapage (jusqu'à 6 frames/tick).
- **Relais direct des combattants vers les spectateurs secondaires (`w >= 1`)** : un combattant moddé envoie désormais directement des enregistrements relais `0x08000000 | frame` (`desyncHash = 0`, zéro risque d'`OnDesync()`) sur le canal 6 aux spectateurs secondaires — supprimant le ralenti 30 FPS même lorsque le spectateur principal est **non moddé et en 30 FPS**.

### Suppression du gel de démarrage Client ↔ Client (hôte spectateur)
- **Pré-chauffage proactif Steam P2P canal 6 pendant `SyncBase`** (`PreWarmSessionPeersOnChannel6`) : ouvre la session P2P entre tous les pairs du lobby (en particulier Client 1 ↔ Client 2 et les spectateurs secondaires) dès la négociation RTT pré-match (~3,5–5 s avant `StartFrameSync`) via un paquet No-Op natif `{0,1,1}` sur frame 0.
- **Pré-chauffage de tous les spectateurs** dans `PreWarmBattleUdpChannel`.
- **Récupération au démarrage (`BuildBattlerSendPacket` & `ReceivePackets`)** : tant que la première frame réelle n'est pas confirmée (`confirmed < m_netDelayFrames`), maintien d'un keepalive doux et rotation de 2 frames initiales non confirmées pour absorber le délai de traversée NAT/ICE, avec réessai NACK initial accéléré (`rttTicks` capé à `4..10`).

### Sessions privées & personnalisées (correction du bug de duplication de soi-même)
- Correction du bug où un joueur se voyait lui-même plusieurs fois dans la liste des membres d'une session privée, personnalisée, tournoi ou sans fin :
  - **Cause #1 corrigée (`NSUNSC.exe+0x113D736` & `+0x116EAC4`)** : `State1_Update` et `ProcessReceivedSessionMessages` conservaient un pointeur vers le slot d'un membre dans un registre (`rdi` / `r15`), appelaient `qsort` sur le tableau `m_slots` (ce qui déplaçait le slot du joueur local à l'ancienne adresse pointée par `rdi`/`r15`), puis déclenchaient le callback `OnMemberJoined` avec ce pointeur périmé — ajoutant le joueur local une nouvelle fois à la table du lobby à chaque arrivée ou resynchronisation d'un pair.
  - **Cause #2 corrigée (`AddMemberToTable` à `+0x113AF60`)** : l'insertion s'arrêtait dès le premier slot vide (`steamId == 0`) sans vérifier si le `SteamID` existait déjà dans un slot supérieur après le départ d'un joueur.
  - **Cause #3 corrigée (`UpdateBattleTableFromChangeQueue` à `+0x00F4310` & `UpdateUserList` à `+0x00FF810`)** : déduplication stricte par `uId` et `SteamID` lors des événements `status == 6` et avant le rendu de l'interface du lobby.
  - **Cause #4 corrigée (`AssignNextUId` à `+0x113B3C0` & `InvokeSessionCallback` à `+0x113D840`)** : préservation de l'`uId` déjà attribué sur les `JoinRequest` retransmis et résolution du vrai slot avant tout callback de session.

### Crashs de déconnexion brutale, équipes, clavier & fermeture (`0xC0000005` / `0xC0000374`)
- Protection contre les déconnexions brutales d'hôte ou d'adversaire pendant une transition de session :
  - Garde sur `ccNetSessionCommunicator::SendPacket` (`+0x1169290`, crash à `+0x11692A3`) et `BroadcastPacket` (`+0x1169310`, crash à `+0x1169320`) lorsque `[session + 0x518]` (`m_pCommunicator`) est mis à `NULL` par un thread réseau concurrent.
  - Garde et auto-réparation de `m_pMySlot` (`[session + 0x500]`) sur `GetMySlot` (`+0x10B8E20`), `SortSlotsAndRefreshMySlot` (`+0x113FA50`) et `ProcessReceivedSessionMessages` (`+0x116DAE0`, 9 points de déréférencement).
- **Conteneurs d'équipe (`TeamContainer` à `+0x094B3E0`, `+0x094B830` et `+0x094B8C0`)** : validation de la vtable d'allocateur et du nœud sentinelle `_Myhead` (`0xC0000005` à `+0x094B3ED`, `+0x094B837`, `+0x094B8D8`) après destruction/libération du conteneur.
- **Fenêtre de chat rapide (`ccStateChatWindow` / `ccChatTemplateParamManager`, `0xC0000005` à `+0x0150CC8`)** :
  - Correction de la cause première sur les trois chemins de sortie/destructeur de `ccStateChatWindow` (`+0x016A795`, `+0x016A8CB`, `+0x016AAE1`) qui déchargeaient `data/network/chatTemplateParam.bin.xfbin` *avant* d'appeler `DestroySingleton` (`+0x150950`), laissant `ClearDynamic` (`+0x150C90`) déréférencer des pointeurs statiques libérés.
  - Garde complémentaire sur `ccChatTemplateParamManager::ClearDynamic` (`+0x150C90`) contre un `pManager == NULL` (appelant `+0x19BE1E`), un `NULL` ou des pointeurs déjà libérés dans `m_ptrVec` / `m_dynVec`, avec filet SEH.
- **Clavier virtuel Win64 (`ccVirtualKeyboard` à `+0x120FEB0` et `+0x120FF90`)** : garde contre un `m_pKeyboard` nul (`0xC0000005` à `+0x120FEE0` et `+0x120FFB5`) tout en conservant le fonctionnement manette/souris.
- Correction du crash d'allocation à la fermeture du jeu (`0xC0000374` `STATUS_HEAP_CORRUPTION` dans `nuccFree` à `+0x12732D8`).
- **Lancement direct de `NSUNSC.exe` & fermeture propre (`DLL_PROCESS_DETACH`)** : attente passive du contexte `SteamAppId` avant d'ouvrir la console, d'écrire `storm_netcode.log` ou d'installer MinHook (évite la 2e console figée lors d'un double-clic direct hors Steam avec `SteamAPI_RestartAppIfNecessary`), et arrêt sûr sous `lpReserved != nullptr` sans deadlock de spinlock ni `FreeLibrary` sous Loader Lock.
- **35 hooks et 10 patchs d'octets (+1 pont de lobby)** vérifiés individuellement au démarrage.

### Latence minimale, 30/60 FPS & AutoTune affinés
- Résolution d'horloge noyau Windows abaissée à **0,500 ms** (`NtSetTimerResolution(5000)`) au lieu de 1,0 ms pour des réveils de boucle de jeu deux fois plus précis.
- Plage **1 frame (16,67 ms)** entre deux joueurs moddés étendue jusqu'à **26 ms de ping** à 60 FPS (40 ms à 30 FPS), et marge de phase par défaut affinée à `JitterMarginMs = 2` (plage `0..10`), avec garde anti-gigue à 60 FPS lissée (`+3 ms + RTT/13` si marge > 0).
- **Détection anticipée des lobbies mixtes 30 / 60 FPS** dès la négociation `SyncBase` via `[pNetManager + 0x183128]` (`GetEngineBattleFps()`), garantissant l'accord dès la Phase 1.
- **Suppression du double comptage `PeerMinDelayFloor`** après calcul hôte et calibration d'`AutoTune` sur le régime établi (exclusion des frames `0..45` de chargement initial + relâchement progressif du coussin dynamique sur route stable `< 0,5 %` de stalls).

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

### Licence
- Passage de la licence MIT à **PolyForm Noncommercial 1.0.0**, avec politique d'attribution complémentaire (`LICENSE`, `NOTICE.md`) : usage, modification et fork libres et gratuits ; vente et commercialisation interdites ; crédits de l'auteur original obligatoires.

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
