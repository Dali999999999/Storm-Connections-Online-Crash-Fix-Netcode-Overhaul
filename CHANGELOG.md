# Changelog

Toutes les évolutions notables de ce projet sont documentées ici.

## [2.10] - 2026-11-27

**Jeu requis : NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS — version 1.71 (Steam, Windows x64).**

Mise à jour ciblée corrigeant un nouveau crash en ligne du jeu d'origine (`0xC0000005` à `NSUNSC.exe+0x00B6530B`) découvert grâce aux retours de la communauté, ainsi que toute sa famille de callbacks réseau asynchrones dans le module d'événements en ligne (`NetFes`). **100 % rétrocompatible sur le réseau avec la v2.9.**

### Nouveau correctif de crash en ligne : sous-système `NetFes` (`NSUNSC.exe+0x00B6530B` & 16 callbacks jumeaux)
- **Cause du bug d'origine identifiée (`NetFes\ccNetEvergreenFesParamManager.cpp` & `ccNetFesParamManager.cpp`)** :
  - Lors de la connexion au mode en ligne ou de la synchronisation des événements/statistiques, le jeu envoie des requêtes HTTP asynchrones vers les serveurs Bandai Namco (`+0x0BB7BF0`) en enregistrant **17 callbacks `std::_Func_impl_no_alloc`** qui capturent uniquement un pointeur `this` brut (`[pFuncImpl + 0x08]`), sans `weak_ptr` ni annulation à la destruction.
  - Si une réponse tardive ou un timeout réseau (`"networkmode_089"` / `"networkmode_1073"`) survient alors que le mode réseau a été quitté/réinitialisé (`ccDataSet` `+0x05B61AF` / `+0x05B61CC`) ou que la tâche `ccTaskCertainExe` (`"ccNetFesParamManager::Update"`) a été purgée par `ccTaskManager::DeleteAllTasks` (`+0x10D70E0`), le jeu déréférence un pointeur libéré sur `call qword ptr [rax + 0x20]` (`NSUNSC.exe+0x00B6530B`) et provoque un crash immédiat au bureau (`0xC0000005`).
- **Armure complète en 3 niveaux (`64/64 hooks` + 17 gardes de vtables `.rdata` W^X)** :
  - **Suivi du cycle de vie (`+0x0B64080` / `+0x0B64C40` et `+0x0B6DE00` / `+0x0B6E420`)** : registre thread-safe (`SRWLOCK`) des instances vivantes de `ccNetEvergreenFesParamManager` et `ccNetFesParamManager` + anneau d'exclusion des gestionnaires détruits, avec assainissement préventif de `m_pUpdateTask` dans le destructeur pour éliminer tout risque de double-free (`+0x0B64C7B` / `+0x0B6E45B`).
  - **Protection W^X sur les 17 vtables `.rdata` (`+0x19987E0..+0x19989A0` et `+0x19995B8..+0x1999740`)** : interception individuelle de `_Do_call` (`slot[2]`) sur les 9 lambdas `EvergreenFes` et les 8 lambdas `NetFes` afin de vérifier la vtable RTTI vivante du gestionnaire et d'ignorer proprement tout callback HTTP orphelin.
  - **Auto-réparation immédiate de `m_pUpdateTask` (`ccTaskCertainExe::sdtor` à `+0x00B57B0`)** : vérification croisée du `SimpleCommand` propriétaire (`[pTask + 0x28] -> [pCmd + 0x08] == pMgr`, insensible à la réutilisation d'adresse mémoire sur le tas `0x30`) et remplacement instantané de `[mgr + 0xB8]` / `[mgr + 0x88]` par une tâche sentinelle inerte (`s_InertFesTask`) en cas de suppression externe — protégeant aussi toutes les méthodes synchrones du module `NetFes`.
- **Télémétrie & intégrité binaire** :
  - Ajout du compteur `NetFes` dans le rapport périodique `[Telemetry]` (`storm_netcode.log`).
  - **64/64 hooks vérifiés via `.pdata`**, **17 gardes de vtables `NetFes`**, **9 patchs d'octets**, **1 pont de lobby** et **7 ponts VFX W^X**, compilés sans aucun avertissement sous MSVC `/W4 /permissive-`.

### Mise à jour depuis la v2.9 (ou antérieure)
1. Remplacez `d3dcompiler_47.dll` (et `netcode_config.ini` si vous souhaitez les commentaires à jour) dans le dossier du jeu à côté de `NSUNSC.exe`.
2. Au lancement, la console doit afficher **`v2.10`** et **`[Init] 64/64 .pdata-verified hooks active.`**

## [2.9] - 2026-11-26

**Jeu requis : NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS — version 1.71 (Steam, Windows x64).**

### Cadence 60,000 Hz exacte, `ClockSync` & écrans haute fréquence (75 / 100 / 144 / 165 Hz)
- **Precision Frame Pacer (`EnablePrecisionFramePacer`)** :
  - Remplacement de la boucle `Sleep(0)` / `Sleep(1)` imprécise du moteur par un `WaitableTimer` haute résolution Windows (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) couplé à un micro-spin QPC final — supprimant la famine CPU `Sleep(0)` ainsi que les chutes accidentelles à 30 FPS lors d'une micro-surcharge frame (> 16,67 ms).
  - **Déverrouillage du Cœur CPU 0** (`nuccTimer::GetTimeUs` à `+0x13D7490` et `GetTimeMs` à `+0x13D75B0`) : le thread principal n'est plus confiné artificiellement sur le cœur 0.
- **Synchronisation d'horloge amortie entre pairs (`ClockSync`)** :
  - Asservissement fin (±25 à ±120 µs/frame max) calé sur l'écart de file lockstep lorsque le PC adverse tourne très légèrement sous ou au-dessus de `60,000 Hz`, éliminant les micro-gels périodiques dus à la dérive quartz entre deux machines.
- **Suppression du judder VSync fenêtré sur écrans hors multiples de 60 Hz (`BypassNon60HzVsyncJudder`)** :
  - Détection automatique des modes fenêtré / sans bordure sur les moniteurs `75, 100, 144, 165 Hz` (`hz > 62 && hz % 60 != 0`) et neutralisation ciblée de l'attente VBlank DXGI pendant `Present` lorsque le pacer `60,000 Hz` est actif — supprimant le battement de cadence 144 Hz ↔ 60 Hz sans modifier le plein écran exclusif ni les écrans 60 / 120 / 180 / 240 Hz.

### Gouvernance anti-gigue en plein match, redondance UDP & négociation RTT
- **Bouclier anti-gigue dynamique en plein combat (`Live Anti-Jitter Shield`) avec hystérésis de retour au calme** :
  - Détection temps réel des rafales de stalls réseau (fenêtre glissante de 5 s adaptée aux modes 30 / 60 FPS) : passage automatique à `Redundancy = 4`, `Spin = 1600 µs` et Fast NACK accéléré pour absorber la dégradation Wi-Fi/route, suivi d'un **retour automatique au régime nominal après 18 s consécutives sans stall** (tout en restant armé pour la suite du match).
- **Suppression du gel lockstep sur `checksum == 0x00000000` (`SyncCheckWriter::WriteChecksum` à `+0x06155D0`)** :
  - Correction d'un faux négatif où un checksum de position légitimement nul (avant l'apparition des personnages ou périodiquement tous les 256 ticks lorsque `popcnt == 0`) bloquait l'envoi/renvoi redondant d'une frame prête (`bReceived != 0`).
- **Filtre de ping à froid (`RTT-Filter`) & séparation `NetStalls` / `ClockSlip` dans `AutoTune`** :
  - Filtrage des pics RTT initiaux gonflés par l'ouverture du relais Steam SDR lors de `SyncBase`, afin de ne plus surévaluer le délai négocié au premier match.
  - Distinction stricte entre vraies pertes/gigues réseau (`NetStalls`) et simples glissements d'horloge (`ClockSlip`) dans l'auto-ajustement en revanche (`AutoTune`), et verrouillage de `m_netDelayFrames` dès le début effectif du combat (`m_bIsStarted != 0`).
- **Synchronisation `2/2` combattants moddés côté Client en lobby spectateur (`'NM'`)** :
  - Propagation rétrocompatible du masque des joueurs moddés dans l'annonce `chan=4, subCmd=0x10` (ignorée par le jeu d'origine et par les versions `v2.6..v2.8`), permettant aux Clients moddés d'appliquer les garde-fous optimaux `2/2` même lorsque l'hôte est spectateur, plus détection d'un hôte moddé obsolète en match 30 FPS.

### Nouveaux correctifs de crashs en ligne (`0xC0000005` — 59 hooks & 7 ponts VFX W^X)
- **Armure complète `ccTaskManager` & `ccTask::Exec` (`+0x109C8E0`, `+0x10D6200..+0x10D70E0`)** :
  - Synchronisation par `CRITICAL_SECTION` réentrante du moteur, protection contre les pointeurs de tâche nuls ou déjà libérés, et mise en quarantaine (tombstone à vtable neutre + barrière mémoire `seq_cst`) des tâches retirées (`RemoveTask`) pendant le parcours actif de la liste — éliminant une source majeure de crashs Use-After-Free en combat et lors des transitions d'écran.
- **7 ponts assembleur W^X contre les crashs VFX `m_pCoord == NULL` (`+0x132C99B` et 6 sites jumeaux)** :
  - Interception atomique en registre (zéro fenêtre TOCTOU, page `PAGE_EXECUTE_READ`) sur les 7 déréférencements non protégés de coordonnées d'effets/particules.
- **Presets d'équipe & `ccSpSkillCustomizeParam` (`+0x0702220..+0x0702400`, `SaveData::SyncShadow` à `+0x0AEED10`)** :
  - Blocage des lectures hors bornes sur un `spAtkIndex` corrompu et assainissement automatique des presets d'équipe avant leur synchronisation réseau.
- **Recherche d'ID de session (`~ccNetIdSearchState` à `+0x016FBB0`, `OnVkFinish` à `+0x0170840`) & clavier AZERTY** :
  - Suppression du crash Use-After-Free lorsque la fenêtre de recherche d'ID est détruite avant la fermeture du clavier virtuel, et saisie directe des chiffres sur la rangée haute des claviers **AZERTY** (`&é"'(-è_çà`) sans maintenir Majuscule.
- **Destruction des sockets UDP (`Comm::DestroySockets` à `+0x1168E20` → `+0x117116C`)** :
  - Correction du crash d'allocateur `operator new(0x28)` libéré via `delete[]` à la fermeture d'une session réseau.

### Sessions personnalisées, lobbies 8 joueurs & fenêtre « Merci de patienter »
- **Renforcement du déblocage « Merci de patienter » (`MSG_WaitMoment`)** :
  - Contournement du verrou `BattleState > 0` sur `WaitDlgClose1` (`+0x0103E70`) et `WaitDlgClose2` (`+0x0104290`), et ajout d'un chien de garde continu dans `EndlessLobby::Update` (`+0x06C3DE0`) qui déverrouille l'interface du lobby si les entrées restent bloquées ou si une migration d'hôte (`ChangeOwner`) se fige.
- **Fin des lobbies « zombies » à 0 membre (`OnMemberLeft` à `+0x113D1E0`)** :
  - Fermeture propre de la session lorsqu'elle tombe à 0 membre valide (au lieu de laisser le joueur bloqué devant 4 cartes grises vides).
- **Purge des cartes fantômes & compactage strict des 8 slots de session (`ccNetSessionData::m_slots[8]`)** :
  - Nettoyage des entrées `BattleTable` orphelines (`RecvBattleTable` à `+0x00F6F30`), déduplication `CheckExistingNewMbr` (`+0x113C8B0`) / `NewMemberTbl::Add` (`+0x1169E20`), compactage strict `0..7` avec `uId = 0` (sans interférer avec le tri `qsort` `+0x11407B0`), rejet atomique d'un `pSlot == nullptr` en lobby plein `8/8`, et alignement de pile ABI x64 + W^X strict sur `LobbySlotNullFixBridge` (`+0x113D7E6`).
- **Journalisation asynchrone & proxy `d3dcompiler_47.dll` 16 arguments** :
  - Écriture de `storm_netcode.log` déportée sur un thread dédié sans bloquer la simulation, couverture complète des 14 arguments de `D3DCompile2`, et compilation sans aucun avertissement sous MSVC `/W4 /permissive-` (**59/59 hooks vérifiés via `.pdata`**).

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
