#pragma once
#include "netcode_mod.h"

// ============================================================================
// Internal Shared Declarations for StormNetcode v2.9 Submodules
// ============================================================================

namespace StormInternal {

// Handshake magic placed in unused padding bytes [+0x05..+0x07] of 24-byte (0x18) pre-match RTT packets
constexpr uint8_t kHandshakeMagic0        = 0x4E; // 'N'
constexpr uint8_t kHandshakeMagic1        = 0x43; // 'C' (Active Battler with StormNetcode)
constexpr uint8_t kHandshakeWatcherMagic1 = 0x53; // 'S' (Spectator/Watcher with StormNetcode)
constexpr uint8_t kHandshakeRosterMagic1  = 0x4D; // 'M' (Host modded-battler bitmask on chan 4 subCmd 0x10)

bool IsValidSteamId(uint64_t steamId) noexcept;

// Maximum 12-byte NACK records allowed in a single outbound UDP packet so we NEVER overflow
// an unmodded opponent's 16-slot m_resendNodes pool (which crashes vanilla NSUNSC.exe @ +0x10F2928).
constexpr uint32_t kMaxOutboundNacksPerTick = 4;

// Base address & QPC frequency
extern uintptr_t     g_BaseAddress;
extern LARGE_INTEGER g_QpcFreq;

// Per-match negotiation & per-frame lockstep pacing state
extern std::atomic<int32_t>  g_NegotiatedTargetDelay;
extern std::atomic<int32_t>  g_PeerMinDelayFloor;
extern std::atomic<int32_t>  g_CapturedHostRawRttMs;
extern std::atomic<int32_t>  g_ClientTrueRawRttMs;
extern std::atomic<int32_t>  g_LastSentSyntheticRttMs;
extern std::atomic<int32_t>  g_AuthoritativeHostRttMs;
extern std::atomic<bool>     g_HostHandshakeObserved;
extern std::atomic<bool>     g_Battler0HasMod;
extern std::atomic<bool>     g_Battler1HasMod;
extern std::atomic<bool>     g_NegotiationPendingForMatch;
extern std::atomic<uint64_t> g_LastSyncInitTickMs;
extern std::atomic<uint64_t> g_LastNegotiatedPeerSteamId;
extern std::atomic<int32_t>  g_PeerDynamicJitterBoostFrames;

// Live Intra-Match Jitter Shield & Gentle Clock-Slip Tracking state
extern std::atomic<bool>     g_LiveAntiJitterActive;
extern std::atomic<int32_t>  g_LiveAntiJitterRedundancyDepth;
extern std::atomic<int32_t>  g_LiveAntiJitterSpinUs;
extern std::atomic<int32_t>  g_FramePacerExtraUs;
extern std::atomic<uint64_t> g_LastActiveBattleTickMs;
extern bool                  s_EarlyFightEvaluated;
extern int32_t               s_LastLiveShieldEvalFrame;

extern uint64_t s_MatchStartStallCount;
extern int32_t  s_MatchPeakSyncFrame;
extern uint64_t s_MatchStallsAtPeakFrame;
extern uint64_t s_MatchNetStallsAtPeakFrame;
extern uint64_t s_MatchSlipStallsAtPeakFrame;
extern bool     s_OpeningSettleCaptured;
extern uint64_t s_OpeningSettleStalls;

extern int32_t s_LastPackedSendFrame;
extern int32_t s_SameFrameSendCount;
extern int32_t s_LastPackedSpectatorSendFrame;
extern int32_t s_SameFrameSpectatorSendCount;
extern int32_t s_LastStallConfirmedFrame;
extern int32_t s_StalledTicksOnConfFrame;
extern int32_t s_InitialSpectatorWaitTicks;

void ResetPerMatchPacingState() noexcept;

// Game internal helper function typedefs
using fnInputBufReset_t            = void(__fastcall*)(ccNetInputBuffer* pInputBuf);
using fnInputBufSeedFrames_t       = void(__fastcall*)(ccNetInputBuffer* pInputBuf, int32_t frameCount);
using fnInputBufGetOrAdvanceSlot_t = ccNetFrameSlot*(__fastcall*)(ccNetInputBuffer* pInputBuf, int32_t frameNumber);
using fnFreeSyncExtractFramePkt_t  = void(__fastcall*)(ccNetFreeBattleSync* pSync, ccNetFrameInputPacket* outPkt, int32_t frameNumber);
using fnFreeSyncSerializeOneFrm_t  = uint8_t*(__fastcall*)(uint8_t* outRawBuf, const ccNetFrameInputPacket* pPkt);
using fnUdpSocketSend_t            = int32_t(__fastcall*)(void* pUdpSocket, const ccNetUdpEndpoint* pEndpoint, const uint8_t* pData, uint32_t dataSize);
using fnSyncBaseIsHost_t           = uint8_t(__fastcall*)(void* pSyncBase);
using fnLocalInfoGetSlotArray_t    = const int8_t*(__fastcall*)(const void* pLocalInfo);
using fnNuccCsOp_t                 = void(__fastcall*)(void* pNuccCs);
using fnVirtualKeyboardReset_t     = void(__fastcall*)(void* pVkImpl);
using fnGetKeyboardDevice_t        = void*(__fastcall*)(void* pInputMgr, int32_t deviceIdx);

extern fnInputBufReset_t            g_fnInputBufReset;
extern fnInputBufSeedFrames_t       g_fnInputBufSeedFrames;
extern fnInputBufGetOrAdvanceSlot_t g_fnInputBufGetOrAdvanceSlot;
extern fnFreeSyncExtractFramePkt_t  g_fnFreeSyncExtractFramePkt;
extern fnFreeSyncSerializeOneFrm_t  g_fnFreeSyncSerializeOneFrm;
extern fnUdpSocketSend_t            g_fnUdpSocketSend;
extern fnSyncBaseIsHost_t           g_fnSyncBaseIsHost;
extern fnLocalInfoGetSlotArray_t    g_fnLocalInfoGetSlotArray;
extern fnNuccCsOp_t                 g_fnNuccCsLock;
extern fnNuccCsOp_t                 g_fnNuccCsUnlock;
extern fnVirtualKeyboardReset_t     g_fnVirtualKeyboardReset;
extern fnGetKeyboardDevice_t        g_fnGetKeyboardDevice;

// Hooked function typedefs
using fnCtrlBattleSync_t           = void(__fastcall*)(void* pBattleBase);
using fnCalcNetFrameByRtt_t        = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t rttMs);
using fnSetupFrameSync_t           = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnUpdateMemberDelay_t        = void(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t memberCount);
using fnBuildSendPacket_t          = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSendSpectatorPacket_t      = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSendWatcherRelayPacket_t   = void(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t targetMemberId);
using fnReceivePackets_t           = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnReceiveWatcherPackets_t    = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSyncBaseInit_t             = void(__fastcall*)(void* pSyncBase);
using fnSyncBaseRecvHostWorstRtt_t = void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnSyncBaseRecvClientRtt_t    = void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnNetReliableSendPacket_t    = int32_t(__fastcall*)(void* pNetCtrl, uint8_t* pPacket, uint32_t packetSize, int32_t targetId);

// SteamFriends017 Persona Name, Input & D3D11 typedefs
using fnSteamContextInit_t          = void**(__fastcall*)(void* pContextInitData);
using fnSteamGetPersonaVtbl_t       = const char*(__fastcall*)(void* pSteamFriends, uint64_t steamId);
using fnSteamCopyFriendPersona32_t  = const char*(__fastcall*)(char* outName33, const uint64_t* pSteamId);
using fnSteamGetFriendPersonaWrap_t = const char*(__fastcall*)(const uint64_t* pSteamId);
using fnKeyboardGetPressedChar_t    = uint8_t(__fastcall*)(const void* pKeyMap);
using fnKeyboardGetPressedKey_t     = uint32_t(__fastcall*)(const void* pKeyMap);
using fnNetIdSearchStateOp_t        = void(__fastcall*)(void* pThis);
using fnNuccD3D11Present_t          = void(__fastcall*)(void* pNuccD3D11);
using fnNuccGetTimeUs_t             = uint64_t(__fastcall*)();
using fnNuccGetTimeMs_t             = uint32_t(__fastcall*)();

// Session Abrupt-Disconnect Null-Deref & Custom/Private Lobby Duplicate-Player typedefs
using fnSessionCommSendPkt_t        = int32_t(__fastcall*)(void* pComm, void* pPacket, uint32_t packetSize, int32_t targetUId);
using fnSessionCommBroadcastPkt_t   = int32_t(__fastcall*)(void* pComm, void* pPacket, uint32_t packetSize, int32_t excludeUId);
using fnSessionCommDestroySockets_t = void(__fastcall*)(void* pCommInner);
using fnSessionOnMemberLeft_t       = void(__fastcall*)(void* pSession, uint64_t leavingSteamId);
using fnSessionGetMySlot_t          = void*(__fastcall*)(void* pSessionHolder);
using fnSessionSortSlots_t          = void(__fastcall*)(void* pSession);
using fnSessionProcessMsgs_t        = void(__fastcall*)(void* pSession);
using fnSessionAddMember_t          = void(__fastcall*)(void* pSessionData, const uint64_t* pMemberSteamId, const uint64_t* pLobbySteamId);
using fnSessionAssignNextUId_t      = void(__fastcall*)(void* pSession, int32_t* pSlotUId);
using fnSessionInvokeCallback_t     = void(__fastcall*)(void* pSession, int32_t callbackId, void* pSlot);
using fnSessionCheckNewMember_t     = int32_t(__fastcall*)(void* pSession, void* pNewMemberEntry);
using fnSessionNewMemberTblAdd_t    = uint8_t(__fastcall*)(void* pNewMemberContainer, uint64_t memberSteamId, const void* pMeta, int32_t uId);
using fnCustomLobbyUpdateBattleTbl_t = void(__fastcall*)(void* pLobbyState);
using fnCustomLobbyRecvBattleTbl_t  = void(__fastcall*)(void* pLobbyState, void* pArg2, const void* pPacket);
using fnCustomLobbyUiUpdateUsers_t  = void(__fastcall*)(void* pLobbyUi, void* pSessionData, void* pBattleTable, void* pArg4, uint64_t selectedIdx, void* pArg6);
using fnTeamContainerGetByIndex_t   = void*(__fastcall*)(void* pContainer, uint32_t index);
using fnTeamContainerFindByCharId_t = void*(__fastcall*)(void* pContainer, uint32_t charId);
using fnTeamContainerFindByIndex_t  = void*(__fastcall*)(void* pContainer, uint32_t index);
using fnChatTemplateClearDynamic_t  = void(__fastcall*)(void* pManager);
using fnSpSkillGetFloat_t           = float(__fastcall*)(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
using fnSpSkillGetMsgIdPtr_t        = const char*(__fastcall*)(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
using fnSpSkillGetDword_t           = uint32_t(__fastcall*)(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
using fnSpSkillGetMessageId_t       = int32_t(__fastcall*)(void* pParamMgr, int32_t charCode, int32_t spAtkIndex, void* pOutNuccString);
using fnSaveDataSyncShadow_t        = void*(__fastcall*)();
using fnTaskExec_t                  = void(__fastcall*)(void* pTask, uint32_t pauseMask);
using fnTaskMgrAddTask_t            = uint32_t(__fastcall*)(void* pMgr, void* pTask);
using fnTaskMgrUpdateRange_t        = void(__fastcall*)(void* pMgr, uint32_t startPrio, uint32_t endPrio, uint32_t pauseFlag);
using fnTaskMgrUpdateSinglePar_t    = void(__fastcall*)(void* pMgr, uint32_t priority, uint32_t pauseFlag);
using fnTaskMgrRemoveTask_t         = void(__fastcall*)(void* pMgr, void* pTask);
using fnTaskMgrDeleteAllTasks_t     = void(__fastcall*)(void* pMgr);

extern fnCtrlBattleSync_t           oCtrlBattleSync;
extern fnCalcNetFrameByRtt_t        oCalcNetFrameByRtt;
extern fnSetupFrameSync_t           oSetupFrameSync;
extern fnUpdateMemberDelay_t        oUpdateMemberDelay;
extern fnBuildSendPacket_t          oBuildSendPacket;
extern fnSendSpectatorPacket_t      oSendSpectatorPacket;
extern fnSendWatcherRelayPacket_t   oSendWatcherRelayPacket;
extern fnReceivePackets_t           oReceivePackets;
extern fnReceiveWatcherPackets_t    oReceiveWatcherPackets;
extern fnSyncBaseInit_t             oSyncBaseInit;
extern fnSyncBaseRecvHostWorstRtt_t oSyncBaseRecvHostWorstRtt;
extern fnSyncBaseRecvClientRtt_t    oSyncBaseRecvClientRtt;
extern fnNetReliableSendPacket_t    oNetReliableSendPacket;

extern fnSteamCopyFriendPersona32_t  oSteamCopyPersonaA;
extern fnSteamCopyFriendPersona32_t  oSteamCopyPersonaB;
extern fnSteamGetFriendPersonaWrap_t oSteamGetPersonaWrapper;
extern fnKeyboardGetPressedChar_t    oKeyboardGetPressedChar;
extern fnKeyboardGetPressedKey_t     oKeyboardGetPressedKey;
extern fnNetIdSearchStateOp_t        oNetIdSearchDtor;
extern fnNetIdSearchStateOp_t        oNetIdSearchOnVkFinish;
extern fnNuccD3D11Present_t          oNuccD3D11Present;
extern fnNuccGetTimeUs_t             oNuccGetTimeUs;
extern fnNuccGetTimeMs_t             oNuccGetTimeMs;

extern fnSessionCommSendPkt_t        oSessionCommSendPkt;
extern fnSessionCommBroadcastPkt_t   oSessionCommBroadcastPkt;
extern fnSessionCommDestroySockets_t oSessionCommDestroySockets;
extern fnSessionOnMemberLeft_t       oSessionOnMemberLeft;
extern fnSessionGetMySlot_t          oSessionGetMySlot;
extern fnSessionSortSlots_t          oSessionSortSlots;
extern fnSessionProcessMsgs_t        oSessionProcessMsgs;
extern fnSessionAddMember_t          oSessionAddMember;
extern fnSessionAssignNextUId_t      oSessionAssignNextUId;
extern fnSessionInvokeCallback_t     oSessionInvokeCallback;
extern fnSessionCheckNewMember_t     oSessionCheckNewMember;
extern fnSessionNewMemberTblAdd_t    oSessionNewMemberTblAdd;
extern fnCustomLobbyUpdateBattleTbl_t oCustomLobbyUpdateBattleTbl;
extern fnCustomLobbyRecvBattleTbl_t  oCustomLobbyRecvBattleTbl;
extern fnCustomLobbyUiUpdateUsers_t  oCustomLobbyUiUpdateUsers;
extern fnTeamContainerGetByIndex_t   oTeamContainerGetByIndex;
extern fnTeamContainerFindByCharId_t oTeamContainerFindByCharId;
extern fnTeamContainerFindByIndex_t  oTeamContainerFindByIndex;
extern fnChatTemplateClearDynamic_t  oChatTemplateClearDynamic;
extern fnSpSkillGetFloat_t           oSpSkillGetFloatParam1;
extern fnSpSkillGetFloat_t           oSpSkillGetFloatParam2;
extern fnSpSkillGetMsgIdPtr_t        oSpSkillGetMsgIdPtr;
extern fnSpSkillGetDword_t           oSpSkillGetDwordParam;
extern fnSpSkillGetMessageId_t       oSpSkillGetMessageId;
extern fnSaveDataSyncShadow_t        oSaveDataSyncShadow;
extern fnTaskExec_t                  oTaskExec;
extern fnTaskMgrAddTask_t            oTaskMgrAddTask;
extern fnTaskMgrUpdateRange_t        oTaskMgrUpdateRange;
extern fnTaskMgrUpdateSinglePar_t    oTaskMgrUpdateSinglePar;
extern fnTaskMgrRemoveTask_t         oTaskMgrRemoveTask;
extern fnTaskMgrDeleteAllTasks_t     oTaskMgrDeleteAllTasks;

// Module 0: Crash fixes & Byte-verified Engine Bug Patches (crash_fixes.cpp)
const char* __fastcall hk_SteamGetFriendPersonaName(const uint64_t* pSteamId);
const char* __fastcall hk_SteamCopyFriendPersonaNameA(char* outName33, const uint64_t* pSteamId);
const char* __fastcall hk_SteamCopyFriendPersonaNameB(char* outName33, const uint64_t* pSteamId);
uint8_t     __fastcall hk_KeyboardGetPressedChar(const void* pKeyMap);
uint32_t    __fastcall hk_KeyboardGetPressedKey(const void* pKeyMap);
void        __fastcall hk_NetIdSearchStateDtor(void* pThis);
void        __fastcall hk_NetIdSearchStateOnVkFinish(void* pThis);
int32_t     __fastcall hk_SessionCommSendPacket(void* pComm, void* pPacket, uint32_t packetSize, int32_t targetUId);
int32_t     __fastcall hk_SessionCommBroadcastPacket(void* pComm, void* pPacket, uint32_t packetSize, int32_t excludeUId);
void        __fastcall hk_SessionCommDestroySockets(void* pCommInner);
void        __fastcall hk_SessionOnMemberLeft(void* pSession, uint64_t leavingSteamId);
void*       __fastcall hk_SessionGetMySlot(void* pSessionHolder);
void        __fastcall hk_SessionSortSlotsAndRefreshMySlot(void* pSession);
void        __fastcall hk_SessionProcessReceivedMessages(void* pSession);
void        __fastcall hk_SessionAddMemberToTable(void* pSessionData, const uint64_t* pMemberSteamId, const uint64_t* pLobbySteamId);
void        __fastcall hk_SessionAssignNextUId(void* pSession, int32_t* pSlotUId);
void        __fastcall hk_SessionInvokeCallback(void* pSession, int32_t callbackId, void* pSlot);
int32_t     __fastcall hk_SessionCheckExistingNewMember(void* pSession, void* pNewMemberEntry);
uint8_t     __fastcall hk_SessionNewMemberTblAdd(void* pNewMemberContainer, uint64_t memberSteamId, const void* pMeta, int32_t uId);
void        __fastcall hk_CustomLobbyUpdateBattleTable(void* pLobbyState);
void        __fastcall hk_CustomLobbyRecvBattleTable(void* pLobbyState, void* pArg2, const void* pPacket);
void        __fastcall hk_CustomLobbyUiUpdateUserList(void* pLobbyUi, void* pSessionData, void* pBattleTable, void* pArg4, uint64_t selectedIdx, void* pArg6);
void*       __fastcall hk_TeamContainerGetByIndex(void* pContainer, uint32_t index);
void*       __fastcall hk_TeamContainerFindByCharId(void* pContainer, uint32_t charId);
void*       __fastcall hk_TeamContainerFindByIndex(void* pContainer, uint32_t index);
void        __fastcall hk_ChatTemplateClearDynamic(void* pManager);
float       __fastcall hk_SpSkillGetFloatParam1(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
float       __fastcall hk_SpSkillGetFloatParam2(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
const char* __fastcall hk_SpSkillGetMsgIdPtr(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
uint32_t    __fastcall hk_SpSkillGetDwordParam(void* pParamMgr, int32_t charCode, int32_t spAtkIndex);
int32_t     __fastcall hk_SpSkillGetMessageId(void* pParamMgr, int32_t charCode, int32_t spAtkIndex, void* pOutNuccString);
void*       __fastcall hk_SaveDataSyncShadow();
void        InitTaskManagerTombstones() noexcept;
void        __fastcall hk_TaskExec(void* pTask, uint32_t pauseMask);
uint32_t    __fastcall hk_TaskMgrAddTask(void* pMgr, void* pTask);
void        __fastcall hk_TaskMgrUpdateRange(void* pMgr, uint32_t startPrio, uint32_t endPrio, uint32_t pauseFlag);
void        __fastcall hk_TaskMgrUpdateSingleParallel(void* pMgr, uint32_t priority, uint32_t pauseFlag);
void        __fastcall hk_TaskMgrRemoveTask(void* pMgr, void* pTask);
void        __fastcall hk_TaskMgrDeleteAllTasks(void* pMgr);
void ApplyCrashAndNetcodeBytePatches(bool applyCrashPatches, bool applyNetcodePatches) noexcept;
void RestoreVerifiedBytePatches() noexcept;
bool InstallLobbySlotNullFixBridge();
void RemoveLobbySlotNullFixBridge() noexcept;
bool InstallVfxCoordNullFixBridges();
void RemoveVfxCoordNullFixBridges() noexcept;
uint64_t GetVfxCoordNullSaves() noexcept;

// Module 0b & 0c: System, D3D11, MMCSS & WLAN Low-Latency Tuning (system_tuning.cpp)
void ApplyProcessGamingBoost() noexcept;
void ShutdownProcessGamingBoost(bool processTerminating = false) noexcept;
void     __fastcall hk_NuccD3D11Present(void* pNuccD3D11);
uint64_t __fastcall hk_NuccGetTimeUs();
uint32_t __fastcall hk_NuccGetTimeMs();
void ApplyWifiGamingLowLatencyMode(bool verboseLog) noexcept;
void ShutdownWifiGamingLowLatencyMode(bool processTerminating = false) noexcept;

// Module 1 & 2: Delay Math, RTT Negotiation & Buffer Seeding (rtt_negotiation.cpp)
int32_t GetEngineBattleFps() noexcept;
double  GetEngineTrueFramePeriodMs() noexcept;
int32_t GetSyntheticRttForFrames(int32_t targetFrames) noexcept;
int32_t ComputeVanillaDelayFrames(int32_t rttMs, int32_t extraOffset);
int32_t ComputeOptimalOneWayDelayFrames(int32_t rttMs, int32_t minFloor, int32_t maxClamp);
int32_t ComputeHostAuthoritativeTargetDelay(int32_t rttMs, int32_t minFloor, int32_t maxClamp, bool peerModded);
void    __fastcall hk_SyncBase_Initialize(void* pSyncBase);
void    __fastcall hk_SyncBase_RecvClientRTT(void* pSyncBase, void* pArg2, uint8_t* pPacket);
void    __fastcall hk_SyncBase_RecvHostWorstRTT(void* pSyncBase, void* pArg2, uint8_t* pPacket);
int32_t __fastcall hk_NetReliableSendPacket(void* pNetCtrl, uint8_t* pPacket, uint32_t packetSize, int32_t targetId);
int32_t __fastcall hk_CalcNetFrameByRtt(ccNetFreeBattleSync* pSync, int32_t rttMs);
void    __fastcall hk_SetupFrameSyncAndBuffers(ccNetFreeBattleSync* pSync);
void    __fastcall hk_UpdateMemberCountAndDelay(ccNetFreeBattleSync* pSync, int32_t memberCount);

// Module 3 & 3b: Multi-Frame UDP Redundancy & Spectator Stream (packet_redundancy.cpp)
void    PreWarmSessionPeersOnChannel6(bool bypassThrottle = false) noexcept;
void    PreWarmBattleUdpChannel(ccNetFreeBattleSync* pSync) noexcept;
int32_t __fastcall hk_BuildBattlerSendPacket(ccNetFreeBattleSync* pSync);
void    __fastcall hk_SendSpectatorPacket(ccNetFreeBattleSync* pSync);
void    __fastcall hk_SendWatcherRelayPacket(ccNetFreeBattleSync* pSync, int32_t targetMemberId);
void    ResetSpectatorRelayState() noexcept;

// Module 4, 4b & 5: Micro-Jitter Poll, Paced FastNACK & BattleSync Governance (jitter_governance.cpp)
void __fastcall hk_ReceivePackets(ccNetFreeBattleSync* pSync);
void __fastcall hk_ReceiveWatcherPackets(ccNetFreeBattleSync* pSync);
void __fastcall hk_CtrlBattleSync(void* pBattleBase);

// Module 6: Endless / Custom Match Lobby WaitDialog Deadlock Fix (crash_fixes.cpp)
void InstallLobbyWaitDeadlockHooks(int* pInstalledCount, int* pExpectedCount) noexcept;
void ResetLobbyWaitDeadlockState() noexcept;

} // namespace StormInternal
