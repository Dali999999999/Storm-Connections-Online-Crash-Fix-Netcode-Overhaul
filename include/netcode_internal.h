#pragma once
#include "netcode_mod.h"

// ============================================================================
// Internal Shared Declarations for StormNetcode v2.7 Submodules
// ============================================================================

namespace StormInternal {

// Handshake magic placed in unused padding bytes [+0x05..+0x07] of 24-byte (0x18) pre-match RTT packets
constexpr uint8_t kHandshakeMagic0        = 0x4E; // 'N'
constexpr uint8_t kHandshakeMagic1        = 0x43; // 'C' (Active Battler with StormNetcode)
constexpr uint8_t kHandshakeWatcherMagic1 = 0x53; // 'S' (Spectator/Watcher with StormNetcode)

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

// Live Intra-Match Jitter Shield state (rapid in-fight recalibration)
extern std::atomic<bool>     g_LiveAntiJitterActive;
extern std::atomic<int32_t>  g_LiveAntiJitterRedundancyDepth;
extern std::atomic<int32_t>  g_LiveAntiJitterSpinUs;
extern bool                  s_EarlyFightEvaluated;
extern int32_t               s_LastLiveShieldEvalFrame;

extern uint64_t s_MatchStartStallCount;
extern int32_t  s_MatchPeakSyncFrame;
extern uint64_t s_MatchStallsAtPeakFrame;

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

extern fnInputBufReset_t            g_fnInputBufReset;
extern fnInputBufSeedFrames_t       g_fnInputBufSeedFrames;
extern fnInputBufGetOrAdvanceSlot_t g_fnInputBufGetOrAdvanceSlot;
extern fnFreeSyncExtractFramePkt_t  g_fnFreeSyncExtractFramePkt;
extern fnFreeSyncSerializeOneFrm_t  g_fnFreeSyncSerializeOneFrm;
extern fnUdpSocketSend_t            g_fnUdpSocketSend;
extern fnSyncBaseIsHost_t           g_fnSyncBaseIsHost;
extern fnLocalInfoGetSlotArray_t    g_fnLocalInfoGetSlotArray;

// Hooked function typedefs
using fnCtrlBattleSync_t           = void(__fastcall*)(void* pBattleBase);
using fnCalcNetFrameByRtt_t        = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t rttMs);
using fnSetupFrameSync_t           = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnUpdateMemberDelay_t        = void(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t memberCount);
using fnBuildSendPacket_t          = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSendSpectatorPacket_t      = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnReceivePackets_t           = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnReceiveWatcherPackets_t    = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSyncBaseInit_t             = void(__fastcall*)(void* pSyncBase);
using fnSyncBaseRecvHostWorstRtt_t = void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnSyncBaseRecvClientRtt_t    = void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnNetReliableSendPacket_t    = int32_t(__fastcall*)(void* pNetCtrl, uint8_t* pPacket, uint32_t packetSize, int32_t targetId);

// SteamFriends017 Persona Name & D3D11 typedefs
using fnSteamContextInit_t          = void**(__fastcall*)(void* pContextInitData);
using fnSteamGetPersonaVtbl_t       = const char*(__fastcall*)(void* pSteamFriends, uint64_t steamId);
using fnSteamCopyFriendPersona32_t  = const char*(__fastcall*)(char* outName33, const uint64_t* pSteamId);
using fnSteamGetFriendPersonaWrap_t = const char*(__fastcall*)(const uint64_t* pSteamId);
using fnNuccD3D11Present_t          = void(__fastcall*)(void* pNuccD3D11);

extern fnCtrlBattleSync_t           oCtrlBattleSync;
extern fnCalcNetFrameByRtt_t        oCalcNetFrameByRtt;
extern fnSetupFrameSync_t           oSetupFrameSync;
extern fnUpdateMemberDelay_t        oUpdateMemberDelay;
extern fnBuildSendPacket_t          oBuildSendPacket;
extern fnSendSpectatorPacket_t      oSendSpectatorPacket;
extern fnReceivePackets_t           oReceivePackets;
extern fnReceiveWatcherPackets_t    oReceiveWatcherPackets;
extern fnSyncBaseInit_t             oSyncBaseInit;
extern fnSyncBaseRecvHostWorstRtt_t oSyncBaseRecvHostWorstRtt;
extern fnSyncBaseRecvClientRtt_t    oSyncBaseRecvClientRtt;
extern fnNetReliableSendPacket_t    oNetReliableSendPacket;

extern fnSteamCopyFriendPersona32_t  oSteamCopyPersonaA;
extern fnSteamCopyFriendPersona32_t  oSteamCopyPersonaB;
extern fnSteamGetFriendPersonaWrap_t oSteamGetPersonaWrapper;
extern fnNuccD3D11Present_t          oNuccD3D11Present;

// Module 0: Crash fixes & Byte-verified Engine Bug Patches (crash_fixes.cpp)
const char* __fastcall hk_SteamGetFriendPersonaName(const uint64_t* pSteamId);
const char* __fastcall hk_SteamCopyFriendPersonaNameA(char* outName33, const uint64_t* pSteamId);
const char* __fastcall hk_SteamCopyFriendPersonaNameB(char* outName33, const uint64_t* pSteamId);
void ApplyCrashAndNetcodeBytePatches(bool applyCrashPatches, bool applyNetcodePatches) noexcept;
void RestoreVerifiedBytePatches() noexcept;
bool InstallLobbySlotNullFixBridge();
void RemoveLobbySlotNullFixBridge() noexcept;

// Module 0b & 0c: System, D3D11, MMCSS & WLAN Low-Latency Tuning (system_tuning.cpp)
void ApplyProcessGamingBoost() noexcept;
void ShutdownProcessGamingBoost() noexcept;
void __fastcall hk_NuccD3D11Present(void* pNuccD3D11);
void ApplyWifiGamingLowLatencyMode(bool verboseLog) noexcept;
void ShutdownWifiGamingLowLatencyMode() noexcept;

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
void    PreWarmBattleUdpChannel(ccNetFreeBattleSync* pSync) noexcept;
int32_t __fastcall hk_BuildBattlerSendPacket(ccNetFreeBattleSync* pSync);
void    __fastcall hk_SendSpectatorPacket(ccNetFreeBattleSync* pSync);

// Module 4, 4b & 5: Micro-Jitter Poll, Paced FastNACK & BattleSync Governance (jitter_governance.cpp)
void __fastcall hk_ReceivePackets(ccNetFreeBattleSync* pSync);
void __fastcall hk_ReceiveWatcherPackets(ccNetFreeBattleSync* pSync);
void __fastcall hk_CtrlBattleSync(void* pBattleBase);

} // namespace StormInternal
