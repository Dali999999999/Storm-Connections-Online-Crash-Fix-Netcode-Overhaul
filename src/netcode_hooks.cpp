#include "netcode_internal.h"
#include "MinHook.h"
#include <algorithm>
#include <cstring>
#include <intrin.h>

// ============================================================================
// StormNetcode v2.9 Core Coordinator:
//   - Shared State & Function Pointer Definitions (`namespace StormInternal`)
//   - Module 4:  Battler Micro-Jitter Poll & Paced Dual-Shot FastNACK (`hk_ReceivePackets`)
//   - Module 4b: Spectator Micro-Jitter Poll, Initial-Load Guard & Paced NACK (`hk_ReceiveWatcherPackets`)
//   - Module 5:  BattleBase::CtrlBattleSync Governance & Live Anti-Jitter Shield (`hk_CtrlBattleSync`)
//   - .pdata-Verified MinHook Installation & Shutdown (`InitializeNetcodeHooks`, `ShutdownNetcodeHooks`)
// ============================================================================

NetcodeConfig    g_Config    = {};
NetcodeTelemetry g_Telemetry = {};

namespace StormInternal {

uintptr_t     g_BaseAddress = 0;
LARGE_INTEGER g_QpcFreq     = {};

// Per-match negotiation & per-frame lockstep pacing state
std::atomic<int32_t>  g_NegotiatedTargetDelay{-1};
std::atomic<int32_t>  g_PeerMinDelayFloor{0};
std::atomic<int32_t>  g_CapturedHostRawRttMs{0};
std::atomic<int32_t>  g_ClientTrueRawRttMs{0};
std::atomic<int32_t>  g_LastSentSyntheticRttMs{0};
std::atomic<int32_t>  g_AuthoritativeHostRttMs{0};
std::atomic<bool>     g_HostHandshakeObserved{false};
std::atomic<bool>     g_Battler0HasMod{false};
std::atomic<bool>     g_Battler1HasMod{false};
std::atomic<bool>     g_NegotiationPendingForMatch{false};
std::atomic<uint64_t> g_LastSyncInitTickMs{0};
std::atomic<uint64_t> g_LastNegotiatedPeerSteamId{0};
std::atomic<int32_t>  g_PeerDynamicJitterBoostFrames{0};

// Live Intra-Match Jitter Shield & Gentle Clock-Slip Tracking state
std::atomic<bool>     g_LiveAntiJitterActive{false};
std::atomic<int32_t>  g_LiveAntiJitterRedundancyDepth{3};
std::atomic<int32_t>  g_LiveAntiJitterSpinUs{1100};
std::atomic<int32_t>  g_FramePacerExtraUs{0};
std::atomic<uint64_t> g_LastActiveBattleTickMs{0};
bool                  s_EarlyFightEvaluated     = false;
int32_t               s_LastLiveShieldEvalFrame = 0;

uint64_t s_MatchStartStallCount       = 0;
int32_t  s_MatchPeakSyncFrame         = 0;
uint64_t s_MatchStallsAtPeakFrame     = 0;
uint64_t s_MatchNetStallsAtPeakFrame  = 0;
uint64_t s_MatchSlipStallsAtPeakFrame = 0;
bool     s_OpeningSettleCaptured      = false;
uint64_t s_OpeningSettleStalls        = 0;

uint64_t s_MatchNetworkStalls         = 0;
uint64_t s_MatchClockSlipStalls       = 0;
int32_t  s_LastSteadyStallFrame       = -100;
bool     s_PrevTickWasTentativeSlip   = false;
int32_t  s_ClockSyncWindowTicks       = 0;
int32_t  s_WindowSlipStalls           = 0;
int32_t  s_WindowNetStalls            = 0;
int32_t  s_ClockSyncCleanWindows      = 0;
bool     s_ClockSyncLoggedOnce        = false;

// Sliding 5-second window + calm-down hysteresis for Live Anti-Jitter Shield
struct LiveShieldSample {
    int32_t  frame     = 0;
    uint64_t netStalls = 0;
};
constexpr int32_t kLiveShieldRingCap               = 6; // 6 x 1s buckets = 5s sliding span
LiveShieldSample  s_LiveShieldRing[kLiveShieldRingCap] = {};
int32_t           s_LiveShieldRingCount            = 0;
int32_t           s_LiveShieldRingHead             = 0;
int32_t           s_LiveShieldLastSampleFrame      = 0;
uint64_t          s_LiveShieldPrevBucketNetStalls  = 0;
int32_t           s_LiveShieldCalmSeconds          = 0;
int32_t           s_LiveShieldEngageLogs           = 0;

int32_t s_LastPackedSendFrame          = -1;
int32_t s_SameFrameSendCount           = 0;
int32_t s_LastPackedSpectatorSendFrame = -1;
int32_t s_SameFrameSpectatorSendCount  = 0;
int32_t s_LastStallConfirmedFrame      = -2;
int32_t s_StalledTicksOnConfFrame      = 0;
int32_t s_InitialSpectatorWaitTicks    = 0;
int32_t s_RealModalStallFrames         = 0;
int32_t s_LastLoggedCoercion           = 0;
int32_t s_LastWatcherLoggedCoercion    = 0;

void ResetPerMatchPacingState() noexcept {
    s_LastPackedSendFrame           = -1;
    s_SameFrameSendCount            = 0;
    s_LastPackedSpectatorSendFrame  = -1;
    s_SameFrameSpectatorSendCount   = 0;
    s_LastStallConfirmedFrame       = -2;
    s_StalledTicksOnConfFrame       = 0;
    s_InitialSpectatorWaitTicks     = 0;
    s_RealModalStallFrames          = 0;
    s_LastLoggedCoercion            = 0;
    s_LastWatcherLoggedCoercion     = 0;
    ResetSpectatorRelayState();
    s_MatchStartStallCount          = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed);
    s_MatchPeakSyncFrame            = 0;
    s_MatchStallsAtPeakFrame        = 0;
    s_MatchNetStallsAtPeakFrame     = 0;
    s_MatchSlipStallsAtPeakFrame    = 0;
    s_OpeningSettleCaptured         = false;
    s_OpeningSettleStalls           = 0;
    s_MatchNetworkStalls            = 0;
    s_MatchClockSlipStalls          = 0;
    s_LastSteadyStallFrame          = -100;
    s_PrevTickWasTentativeSlip      = false;
    s_ClockSyncWindowTicks          = 0;
    s_WindowSlipStalls              = 0;
    s_WindowNetStalls               = 0;
    s_ClockSyncCleanWindows         = 0;
    s_ClockSyncLoggedOnce           = false;
    std::memset(s_LiveShieldRing, 0, sizeof(s_LiveShieldRing));
    s_LiveShieldRingCount           = 0;
    s_LiveShieldRingHead            = 0;
    s_LiveShieldLastSampleFrame     = 0;
    s_LiveShieldPrevBucketNetStalls = 0;
    s_LiveShieldCalmSeconds         = 0;
    s_LiveShieldEngageLogs          = 0;
    g_FramePacerExtraUs.store(0, std::memory_order_relaxed);
    g_LiveAntiJitterActive.store(false);
    g_LiveAntiJitterRedundancyDepth.store(std::clamp(g_Config.redundancy_frame_depth, 1, 5));
    g_LiveAntiJitterSpinUs.store(std::clamp(g_Config.micro_jitter_spin_us, 50, 2000));
    s_EarlyFightEvaluated           = false;
    s_LastLiveShieldEvalFrame       = 0;
}

// Game internal helper function pointers
fnInputBufReset_t            g_fnInputBufReset            = nullptr;
fnInputBufSeedFrames_t       g_fnInputBufSeedFrames       = nullptr;
fnInputBufGetOrAdvanceSlot_t g_fnInputBufGetOrAdvanceSlot = nullptr;
fnFreeSyncExtractFramePkt_t  g_fnFreeSyncExtractFramePkt  = nullptr;
fnFreeSyncSerializeOneFrm_t  g_fnFreeSyncSerializeOneFrm  = nullptr;
fnUdpSocketSend_t            g_fnUdpSocketSend            = nullptr;
fnSyncBaseIsHost_t           g_fnSyncBaseIsHost           = nullptr;
fnLocalInfoGetSlotArray_t    g_fnLocalInfoGetSlotArray    = nullptr;
fnNuccCsOp_t                 g_fnNuccCsLock               = nullptr;
fnNuccCsOp_t                 g_fnNuccCsUnlock             = nullptr;
fnVirtualKeyboardReset_t     g_fnVirtualKeyboardReset     = nullptr;
fnGetKeyboardDevice_t        g_fnGetKeyboardDevice        = nullptr;

// Original trampoline pointers populated by MinHook
fnCtrlBattleSync_t           oCtrlBattleSync           = nullptr;
fnCalcNetFrameByRtt_t        oCalcNetFrameByRtt        = nullptr;
fnSetupFrameSync_t           oSetupFrameSync           = nullptr;
fnUpdateMemberDelay_t        oUpdateMemberDelay        = nullptr;
fnBuildSendPacket_t          oBuildSendPacket          = nullptr;
fnSendSpectatorPacket_t      oSendSpectatorPacket      = nullptr;
fnSendWatcherRelayPacket_t   oSendWatcherRelayPacket   = nullptr;
fnReceivePackets_t           oReceivePackets           = nullptr;
fnReceiveWatcherPackets_t    oReceiveWatcherPackets    = nullptr;
fnSyncBaseInit_t             oSyncBaseInit             = nullptr;
fnSyncBaseRecvHostWorstRtt_t oSyncBaseRecvHostWorstRtt = nullptr;
fnSyncBaseRecvClientRtt_t    oSyncBaseRecvClientRtt    = nullptr;
fnNetReliableSendPacket_t    oNetReliableSendPacket    = nullptr;

fnSteamCopyFriendPersona32_t   oSteamCopyPersonaA          = nullptr;
fnSteamCopyFriendPersona32_t   oSteamCopyPersonaB          = nullptr;
fnSteamGetFriendPersonaWrap_t  oSteamGetPersonaWrapper     = nullptr;
fnKeyboardGetPressedChar_t     oKeyboardGetPressedChar     = nullptr;
fnKeyboardGetPressedKey_t      oKeyboardGetPressedKey      = nullptr;
fnNetIdSearchStateOp_t         oNetIdSearchDtor            = nullptr;
fnNetIdSearchStateOp_t         oNetIdSearchOnVkFinish      = nullptr;
fnNuccD3D11Present_t           oNuccD3D11Present           = nullptr;
fnNuccGetTimeUs_t              oNuccGetTimeUs              = nullptr;
fnNuccGetTimeMs_t              oNuccGetTimeMs              = nullptr;

fnSessionCommSendPkt_t         oSessionCommSendPkt         = nullptr;
fnSessionCommBroadcastPkt_t    oSessionCommBroadcastPkt    = nullptr;
fnSessionCommDestroySockets_t  oSessionCommDestroySockets  = nullptr;
fnSessionOnMemberLeft_t        oSessionOnMemberLeft        = nullptr;
fnSessionGetMySlot_t           oSessionGetMySlot           = nullptr;
fnSessionSortSlots_t           oSessionSortSlots           = nullptr;
fnSessionProcessMsgs_t         oSessionProcessMsgs         = nullptr;
fnSessionAddMember_t           oSessionAddMember           = nullptr;
fnSessionAssignNextUId_t       oSessionAssignNextUId       = nullptr;
fnSessionInvokeCallback_t      oSessionInvokeCallback      = nullptr;
fnSessionCheckNewMember_t      oSessionCheckNewMember      = nullptr;
fnSessionNewMemberTblAdd_t     oSessionNewMemberTblAdd     = nullptr;
fnCustomLobbyUpdateBattleTbl_t oCustomLobbyUpdateBattleTbl = nullptr;
fnCustomLobbyRecvBattleTbl_t   oCustomLobbyRecvBattleTbl   = nullptr;
fnCustomLobbyUiUpdateUsers_t   oCustomLobbyUiUpdateUsers   = nullptr;
fnTeamContainerGetByIndex_t    oTeamContainerGetByIndex    = nullptr;
fnTeamContainerFindByCharId_t  oTeamContainerFindByCharId  = nullptr;
fnTeamContainerFindByIndex_t   oTeamContainerFindByIndex   = nullptr;
fnChatTemplateClearDynamic_t   oChatTemplateClearDynamic   = nullptr;
fnSpSkillGetFloat_t            oSpSkillGetFloatParam1      = nullptr;
fnSpSkillGetFloat_t            oSpSkillGetFloatParam2      = nullptr;
fnSpSkillGetMsgIdPtr_t         oSpSkillGetMsgIdPtr         = nullptr;
fnSpSkillGetDword_t            oSpSkillGetDwordParam       = nullptr;
fnSpSkillGetMessageId_t        oSpSkillGetMessageId        = nullptr;
fnSaveDataSyncShadow_t         oSaveDataSyncShadow         = nullptr;
fnTaskExec_t                   oTaskExec                   = nullptr;
fnTaskMgrAddTask_t             oTaskMgrAddTask             = nullptr;
fnTaskMgrUpdateRange_t         oTaskMgrUpdateRange         = nullptr;
fnTaskMgrUpdateSinglePar_t     oTaskMgrUpdateSinglePar     = nullptr;
fnTaskMgrRemoveTask_t          oTaskMgrRemoveTask          = nullptr;
fnTaskMgrDeleteAllTasks_t      oTaskMgrDeleteAllTasks      = nullptr;

// ============================================================================
// 4. Gentle Micro-Jitter Poll & Paced Single-Shot FastNACK Recovery
//    Hook on ccNetFreeBattleSync::ReceivePackets (VA 0x1410F2740)
// ============================================================================

void __fastcall hk_ReceivePackets(ccNetFreeBattleSync* pSync) {
    oReceivePackets(pSync);

    if (!pSync || !pSync->m_pInputBuffer || !pSync->m_bIsStarted || pSync->m_coercionReason != 0) {
        return;
    }

    // Match finished / post-KO victory & rematch screen (verified @ CtrlFighting 0x1410F1BA8):
    // m_bSkipStall (+0x260) != 0 means frame sync is intentionally parked. Suppress both the
    // 1100us MicroJitterPoll spin and vanilla's 9-tick NACK loop (which otherwise sends ~6.7
    // useless NACKs/sec for confirmedFrame+1 until the rematch/lobby transition).
    if (pSync->m_bSkipStall != 0) {
        pSync->m_waitCounter      = 0;
        s_LastStallConfirmedFrame = -2;
        s_StalledTicksOnConfFrame = 0;
        return;
    }

    ccNetInputBuffer* pBuf = pSync->m_pInputBuffer;

    // Check if TryAdvanceSyncFrame (0x1410ED760) is about to succeed:
    //   When m_currentSyncFrame < 0, it needs m_confirmedAllPlayersFrame >= 0.
    //   When m_currentSyncFrame >= 0, it needs m_confirmedAllPlayersFrame > m_currentSyncFrame.
    auto canAdvanceNow = [&]() -> bool {
        int32_t cur  = pBuf->m_currentSyncFrame;
        int32_t conf = pBuf->m_confirmedAllPlayersFrame;
        return (cur < 0) ? (conf >= 0) : (conf > cur);
    };

    if (canAdvanceNow()) {
        s_LastStallConfirmedFrame = -2;
        s_StalledTicksOnConfFrame = 0;
        return;
    }

    // Gentle Dual-Edge Micro-Jitter Poll:
    // Run on tick 0 (up to 850us, or 1600us when Live Anti-Jitter Shield is active) and tick 1 (up to 450us/550us),
    // spaced by ~40us per poll, catching packets right across 16.67ms VSync boundaries without burning CPU during sustained stalls.
    if (g_Config.enable_micro_jitter_poll &&
        g_Config.micro_jitter_spin_us > 0 &&
        pSync->m_stallFrameCounter <= 1 &&
        g_QpcFreq.QuadPart > 0) {

        int64_t cfgSpinUs = std::clamp<int64_t>(g_Config.micro_jitter_spin_us, 50, 2000);
        if (g_LiveAntiJitterActive.load()) {
            cfgSpinUs = std::max(cfgSpinUs, static_cast<int64_t>(g_LiveAntiJitterSpinUs.load()));
        }

        const int64_t spinUs        = (pSync->m_stallFrameCounter == 0)
            ? cfgSpinUs
            : (g_LiveAntiJitterActive.load() ? 950 : std::min<int64_t>(cfgSpinUs, 650));
        const int64_t pollStepTicks = std::max<int64_t>(1, (g_QpcFreq.QuadPart * 25) / 1000000LL);
        LARGE_INTEGER tStart = {}, tNow = {};
        QueryPerformanceCounter(&tStart);
        const int64_t deadlineTicks = tStart.QuadPart + (g_QpcFreq.QuadPart * spinUs) / 1000000LL;
        int64_t nextPollTicks       = tStart.QuadPart + pollStepTicks;

        while (true) {
            QueryPerformanceCounter(&tNow);
            if (tNow.QuadPart >= deadlineTicks) {
                break;
            }
            if (tNow.QuadPart < nextPollTicks) {
                _mm_pause();
                continue;
            }
            nextPollTicks = tNow.QuadPart + pollStepTicks;
            oReceivePackets(pSync);
            if (canAdvanceNow()) {
                g_Telemetry.jitterPollsRecovered.fetch_add(1, std::memory_order_relaxed);
                s_LastStallConfirmedFrame = -2;
                s_StalledTicksOnConfFrame = 0;
                return;
            }
        }
    }

    // RTT-Aware & 30/60-FPS-Aware Paced FastNACK:
    // In vanilla CtrlFighting (0x1410F1B2C), m_waitCounter fires a NACK every 9 ticks unconditionally,
    // which on high-RTT links (e.g. 230ms) spams duplicate NACKs before the first NACK's reply can
    // physically return, exhausting an unmodded opponent's 16-slot m_resendNodes queue (1 pop/tick).
    // Skip post-KO / match-end idle frames where the engine is no longer stalling (m_bIsStalled == 0).
    if (g_Config.fast_nack_recovery && (pSync->m_bIsStalled != 0 || pBuf->m_currentSyncFrame < 0)) {
        const int32_t conf = pBuf->m_confirmedAllPlayersFrame;
        if (conf != s_LastStallConfirmedFrame) {
            s_LastStallConfirmedFrame = conf;
            s_StalledTicksOnConfFrame = 1;
        } else {
            ++s_StalledTicksOnConfFrame;
        }

        // If ReceivePackets (0x1410F2B2C) already queued a gap-NACK on this tick, don't double-push
        if (pSync->m_missingFramesBegin != pSync->m_missingFramesEnd) {
            pSync->m_waitCounter = 0;
            return;
        }

        const bool    is30Fps   = (GetEngineBattleFps() == 30);
        const int32_t frameMs   = is30Fps ? 33 : 16;
        const int32_t rawRtt    = std::max(0, g_Telemetry.lastRawRttMs.load());
        const int32_t rttTicks  = std::clamp((rawRtt + frameMs - 1) / frameMs, 2, 24);
        const int32_t nackTick1 = 2;
        const int32_t nackTick2 = nackTick1 + std::max(5, rttTicks + 2);
        const int32_t rawStep   = std::max(6, rttTicks + 3);
        const int32_t retryStep = (conf < std::max<int32_t>(1, pSync->m_netDelayFrames))
            ? std::clamp(rttTicks, 4, 10)
            : rawStep;

        const bool shouldFireNack =
            (s_StalledTicksOnConfFrame == nackTick1) ||
            (s_StalledTicksOnConfFrame == nackTick2) ||
            (s_StalledTicksOnConfFrame > nackTick2 && ((s_StalledTicksOnConfFrame - nackTick2) % retryStep) == 0);

        if (shouldFireNack && pSync->m_bDisconnectFlag == 0) {
            pSync->m_waitCounter = 8; // CtrlFighting increments to 9 > 8 -> queues single NACK for (conf + 1)
            if (s_StalledTicksOnConfFrame <= nackTick2) {
                g_Telemetry.fastNacksTriggered.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            pSync->m_waitCounter = 0; // Suppress premature duplicate NACKs while reply is in flight
        }
    }
}

// ============================================================================
// 4b. Spectator Micro-Jitter Poll, Initial-Load Guard & Paced NACK Recovery
//     Hook on ccNetFreeBattleSync::ReceiveWatcherPackets (VA 0x1410F2D80)
// ============================================================================

void __fastcall hk_ReceiveWatcherPackets(ccNetFreeBattleSync* pSync) {
    oReceiveWatcherPackets(pSync);

    if (!pSync || !pSync->m_pInputBuffer || !pSync->m_bIsStarted) {
        return;
    }

    if (pSync->m_coercionReason != 0) {
        if (s_LastWatcherLoggedCoercion != pSync->m_coercionReason) {
            s_LastWatcherLoggedCoercion = pSync->m_coercionReason;
            LogNetcode("[ReceiveWatcher] Match termination flagged: CoercionReason=%d (Frame=%d)",
                       pSync->m_coercionReason,
                       pSync->m_pInputBuffer ? pSync->m_pInputBuffer->m_currentSyncFrame : -1);
        }
        return;
    }

    // Post-KO / match-end parked state (verified @ CtrlWatching 0x1410F1E48):
    // Prevent bogus post-KO FastNACKs and wasted MicroJitterPoll spin on the victory screen.
    if (pSync->m_bSkipStall != 0) {
        pSync->m_waitCounter        = 0;
        s_LastStallConfirmedFrame   = -2;
        s_StalledTicksOnConfFrame   = 0;
        s_InitialSpectatorWaitTicks = 0;
        return;
    }

    ccNetInputBuffer* pBuf = pSync->m_pInputBuffer;

    auto canAdvanceNow = [&]() -> bool {
        int32_t cur  = pBuf->m_currentSyncFrame;
        int32_t conf = pBuf->m_confirmedAllPlayersFrame;
        return (cur < 0) ? (conf >= 0) : (conf > cur);
    };

    if (canAdvanceNow()) {
        s_LastStallConfirmedFrame   = -2;
        s_StalledTicksOnConfFrame   = 0;
        s_InitialSpectatorWaitTicks = 0;
        return;
    }

    // 1. Initial Pre-First-Packet Wait Guard (m_maxWatcherFrame == 0):
    //    When the Spectator finishes loading into the battle scene slightly before the two Battlers
    //    begin transmitting frame `m_netDelayFrames`, m_maxWatcherFrame (+0x04) is still 0.
    //    Keep m_stallFrameCounter = 0 during this initial handshake window (up to 600 ticks = 10s)
    //    so stage-load skew never eats into the 30-second disconnect timeout, and pace NACK probes
    //    to at most once every 30 ticks (0.5s) so we never overflow the Battlers' 8-slot m_spectatorDeque.
    if (pSync->m_maxWatcherFrame == 0) {
        ++s_InitialSpectatorWaitTicks;
        if (s_InitialSpectatorWaitTicks < 600) {
            pSync->m_stallFrameCounter = 0;
        }
        const bool sendInitialProbe = (s_InitialSpectatorWaitTicks >= 30) &&
                                      ((s_InitialSpectatorWaitTicks % 30) == 0) &&
                                      (pSync->m_bDisconnectFlag == 0);
        pSync->m_waitCounter = sendInitialProbe ? 8 : 0;
        return;
    }

    s_InitialSpectatorWaitTicks = 0;

    // 2. Dual-Edge Micro-Jitter Spin-Poll for Spectators:
    //    Active as soon as the Battlers have transmitted at least one frame (m_maxWatcherFrame > 0),
    //    including at Frame == (m_netDelayFrames - 1) (e.g. Frame=8, the first unseeded frame!).
    if (g_Config.enable_micro_jitter_poll &&
        g_Config.micro_jitter_spin_us > 0 &&
        pSync->m_stallFrameCounter <= 1 &&
        g_QpcFreq.QuadPart > 0) {

        const int64_t cfgSpinUs = std::clamp<int64_t>(g_Config.micro_jitter_spin_us, 50, 2000);
        const int64_t spinUs    = (pSync->m_stallFrameCounter == 0)
            ? cfgSpinUs
            : std::min<int64_t>(cfgSpinUs, 450);
        const int64_t pollStepTicks = std::max<int64_t>(1, (g_QpcFreq.QuadPart * 40) / 1000000LL);
        LARGE_INTEGER tStart = {}, tNow = {};
        QueryPerformanceCounter(&tStart);
        const int64_t deadlineTicks = tStart.QuadPart + (g_QpcFreq.QuadPart * spinUs) / 1000000LL;
        int64_t nextPollTicks       = tStart.QuadPart + pollStepTicks;

        while (true) {
            QueryPerformanceCounter(&tNow);
            if (tNow.QuadPart >= deadlineTicks) {
                break;
            }
            if (tNow.QuadPart < nextPollTicks) {
                _mm_pause();
                continue;
            }
            nextPollTicks = tNow.QuadPart + pollStepTicks;
            oReceiveWatcherPackets(pSync);
            if (canAdvanceNow()) {
                g_Telemetry.jitterPollsRecovered.fetch_add(1, std::memory_order_relaxed);
                s_LastStallConfirmedFrame = -2;
                s_StalledTicksOnConfFrame = 0;
                return;
            }
        }
    }

    // 3. RTT-Aware & 30/60-FPS-Aware Paced Spectator NACK Recovery:
    //    In vanilla CtrlWatcherInputAdvance (0x1410F43F0), CC2 forgot to reset m_waitCounter (+0x44)
    //    after sending a NACK (`cmp eax, 8; jbe`), causing spectators to spam 60 NACKs/sec on every
    //    tick >= 9 and overflow both Battlers' 8-slot m_spectatorDeque!
    //    Space retries by at least (RTT + 2) ticks so a 2nd NACK never fires before the 1st reply can return.
    const int32_t conf = pBuf->m_confirmedAllPlayersFrame;
    if (conf != s_LastStallConfirmedFrame) {
        s_LastStallConfirmedFrame = conf;
        s_StalledTicksOnConfFrame = 1;
    } else {
        ++s_StalledTicksOnConfFrame;
    }

    const bool    is30Fps   = (GetEngineBattleFps() == 30);
    const int32_t frameMs   = is30Fps ? 33 : 16;
    const int32_t rawRtt    = std::max(0, g_Telemetry.lastRawRttMs.load());
    const int32_t rttTicks  = std::clamp((rawRtt + frameMs - 1) / frameMs, 2, 24);
    const int32_t nackTick1 = 4;
    const int32_t nackTick2 = nackTick1 + std::max(6, rttTicks + 2);
    const int32_t retryStep = std::max(8, rttTicks + 4);

    bool shouldSendWatcherNack = false;
    if (g_Config.fast_nack_recovery) {
        shouldSendWatcherNack =
            (s_StalledTicksOnConfFrame == nackTick1) ||
            (s_StalledTicksOnConfFrame == nackTick2) ||
            (s_StalledTicksOnConfFrame > nackTick2 && ((s_StalledTicksOnConfFrame - nackTick2) % retryStep) == 0);
    } else {
        shouldSendWatcherNack = (s_StalledTicksOnConfFrame >= 9 && (s_StalledTicksOnConfFrame % 9) == 0);
    }

    if (shouldSendWatcherNack && pSync->m_bDisconnectFlag == 0) {
        pSync->m_waitCounter = 8; // CtrlWatcherInputAdvance increments to 9 > 8 and sends 1 clean NACK
        if (g_Config.fast_nack_recovery && s_StalledTicksOnConfFrame <= nackTick2) {
            g_Telemetry.fastNacksTriggered.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        pSync->m_waitCounter = 0; // CtrlWatcherInputAdvance increments to 1 <= 8 (no NACK spam)
    }
}

// ============================================================================
// 5. BattleBase::CtrlBattleSync Governance (VA 0x1408A1F30, RVA 0x8A1F30)
//    Preserves deterministic TaskPause::TRUE / FALSE state transitions while
//    governing the intrusive "Transmitting..." dark UI modal (+0x504).
// ============================================================================

void __fastcall hk_CtrlBattleSync(void* pBattleBase) {
    if (!pBattleBase) {
        oCtrlBattleSync(pBattleBase);
        return;
    }

    auto* pBytes = reinterpret_cast<uint8_t*>(pBattleBase);
    auto* pModalTimer = reinterpret_cast<int32_t*>(pBytes + 0x504);

    uintptr_t pSyncAddr = g_BaseAddress
        ? *reinterpret_cast<uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetFreeBattleSyncPtr)
        : 0;
    auto* pSync = reinterpret_cast<ccNetFreeBattleSync*>(pSyncAddr);
    const bool isSpectator = (pSync && pSync->m_myPlayerIdx == -1);

    if (pSync && pSync->m_coercionReason != 0) {
        if (s_LastLoggedCoercion != pSync->m_coercionReason) {
            s_LastLoggedCoercion = pSync->m_coercionReason;
            LogNetcode("[CtrlBattleSync] CoercionReason=%d detected! (1=Disconnect, 2=Timeout, 3=Desync)",
                       pSync->m_coercionReason);
        }
    }

    if (g_Config.suppress_short_overlay) {
        // Track true consecutive modal stall ticks independently of *pModalTimer (+0x504),
        // because clamping *pModalTimer to 28 previously prevented it from ever reaching `threshold`!
        const int32_t engineTimer = *pModalTimer;
        if (engineTimer <= 0) {
            s_RealModalStallFrames = 0;
        } else {
            ++s_RealModalStallFrames;
        }

        // For spectators: suppress overlay for at least 180 frames (3 seconds),
        // or up to overlay_threshold_frames if configured higher.
        const int32_t threshold = isSpectator
            ? std::max(180, g_Config.overlay_threshold_frames)
            : std::max(30, g_Config.overlay_threshold_frames);

        // Keep +0x504 below 0x1E (30) ONLY while true stall duration < threshold;
        // once threshold is reached, let +0x504 cross 30 naturally so real disconnects show the modal!
        if (s_RealModalStallFrames < threshold && engineTimer >= 28) {
            *pModalTimer = 28;
        }
    } else {
        s_RealModalStallFrames = 0;
    }

    oCtrlBattleSync(pBattleBase);

    // Telemetry periodic report (every 300 battle ticks = 5 seconds at 60 FPS)
    uint64_t totalTicks = g_Telemetry.totalBattleFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pSync) {
        const bool matchParked = (!pSync->m_bIsStarted || pSync->m_coercionReason != 0 || pSync->m_bSkipStall != 0);
        if (matchParked) {
            g_FramePacerExtraUs.store(0, std::memory_order_relaxed);
        }
        if (pSync->m_bIsStarted && pSync->m_bIsStalled) {
            g_Telemetry.lockstepStallFrames.fetch_add(1, std::memory_order_relaxed);
        }
        if (pSync->m_bIsStarted && pSync->m_pInputBuffer) {
            const int32_t curFrame = pSync->m_pInputBuffer->m_currentSyncFrame;
            if (!isSpectator && !matchParked) {
                g_LastActiveBattleTickMs.store(GetTickCount64(), std::memory_order_relaxed);
            }

            // Classify post-opening stalls: isolated 1-tick stalls spaced > 15 frames apart are
            // peer clock-slip (e.g. 58.8-59.94 Hz vs our exact 60.000 Hz), whereas multi-tick stalls
            // (m_stallFrameCounter >= 2) or bursts (<= 15 f apart) are genuine network jitter/loss.
            if (!matchParked && curFrame >= 45) {
                if (pSync->m_bIsStalled) {
                    if (pSync->m_stallFrameCounter >= 2) {
                        if (pSync->m_stallFrameCounter == 2 && s_PrevTickWasTentativeSlip) {
                            if (s_MatchClockSlipStalls > 0) --s_MatchClockSlipStalls;
                            ++s_MatchNetworkStalls;
                            if (s_WindowSlipStalls > 0) --s_WindowSlipStalls;
                            ++s_WindowNetStalls;
                        }
                        ++s_MatchNetworkStalls;
                        ++s_WindowNetStalls;
                        s_PrevTickWasTentativeSlip = false;
                    } else {
                        const bool isBurst = (curFrame - s_LastSteadyStallFrame) <= 15;
                        if (isBurst) {
                            ++s_MatchNetworkStalls;
                            ++s_WindowNetStalls;
                            s_PrevTickWasTentativeSlip = false;
                        } else {
                            ++s_MatchClockSlipStalls;
                            ++s_WindowSlipStalls;
                            s_PrevTickWasTentativeSlip = true;
                        }
                        s_LastSteadyStallFrame = curFrame;
                    }
                } else {
                    s_PrevTickWasTentativeSlip = false;
                }

                // Damped Clock-Slip Pacer Steering (active 60 FPS battlers only, bounded 0..350 us):
                // Fine +25us lock steps avoid overshooting 59.94Hz VSync peers into dual-pacer ping-pong,
                // while two-zone damped decay (resetting clean-window count on every step and ignoring
                // windows with network stalls) eliminates the 5-second sawtooth stall cycle.
                if (!isSpectator && curFrame >= 60 && GetEngineBattleFps() == 60) {
                    ++s_ClockSyncWindowTicks;
                    if (s_ClockSyncWindowTicks >= 60) {
                        const int32_t curExtra = g_FramePacerExtraUs.load(std::memory_order_relaxed);
                        if (s_WindowSlipStalls >= 1 && s_WindowNetStalls == 0) {
                            s_ClockSyncCleanWindows = 0;
                            const int32_t stepUp    = (s_WindowSlipStalls >= 2) ? 50 : 25;
                            const int32_t nextExtra = std::min<int32_t>(350, curExtra + stepUp);
                            if (nextExtra != curExtra) {
                                g_FramePacerExtraUs.store(nextExtra, std::memory_order_relaxed);
                                if (!s_ClockSyncLoggedOnce) {
                                    s_ClockSyncLoggedOnce = true;
                                    LogNetcode("[ClockSync] Peer running slightly below 60.00Hz (isolated clock-slip, 0 loss) -> FramePacer +%d us/frame (damped lockstep).",
                                               nextExtra);
                                }
                            }
                        } else if (s_WindowSlipStalls == 0 && s_WindowNetStalls == 0) {
                            ++s_ClockSyncCleanWindows;
                            const int32_t holdWindows = (curExtra > 75) ? 8 : 20;
                            const int32_t decayStep   = (curExtra > 75) ? 25 : 10;
                            if (s_ClockSyncCleanWindows >= holdWindows && curExtra > 0) {
                                s_ClockSyncCleanWindows = 0;
                                const int32_t nextExtra = std::max<int32_t>(0, curExtra - decayStep);
                                g_FramePacerExtraUs.store(nextExtra, std::memory_order_relaxed);
                            }
                        } else {
                            // Network jitter/burst window: preserve current clock lock without eroding it
                            s_ClockSyncCleanWindows = 0;
                        }
                        s_ClockSyncWindowTicks = 0;
                        s_WindowSlipStalls     = 0;
                        s_WindowNetStalls      = 0;
                    }
                }
            }

            if (curFrame > s_MatchPeakSyncFrame) {
                s_MatchPeakSyncFrame         = curFrame;
                s_MatchStallsAtPeakFrame     = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed) - s_MatchStartStallCount;
                s_MatchNetStallsAtPeakFrame  = s_MatchNetworkStalls;
                s_MatchSlipStallsAtPeakFrame = s_MatchClockSlipStalls;
                if (!s_OpeningSettleCaptured && curFrame >= 45) {
                    s_OpeningSettleCaptured = true;
                    s_OpeningSettleStalls   = s_MatchStallsAtPeakFrame;
                }
            }

            // Real-time intra-match live recalibration with hysteresis across the ENTIRE match (post-opening frame 45):
            // 1) Early fight (<= 15s): triggers on cumulative >= 2.5% net stalls (>= 5 stalls).
            // 2) Mid/late fight (any frame > 3s): 5-second FPS-aware sliding window catches sudden Wi-Fi/route
            //    jitter spikes (>= 4.0% over recent 2..5s span) without dilution.
            // 3) Calm-down hysteresis: after 18 consecutive 1-second buckets with 0 net stalls, releases the
            //    extra CPU spin & redundancy while remaining armed for any subsequent spike.
            if (!isSpectator && !matchParked && s_OpeningSettleCaptured && curFrame >= 45) {
                const bool    is30Fps    = (GetEngineBattleFps() == 30);
                const int32_t sampleStep = is30Fps ? 30 : 60; // 1.0s bucket in both 30 FPS and 60 FPS
                const int32_t evalMinFrm = is30Fps ? 90 : 180;
                const int32_t earlyLimit = is30Fps ? 450 : 900;

                if (s_LiveShieldRingCount == 0 || (curFrame - s_LiveShieldLastSampleFrame) >= sampleStep) {
                    s_LiveShieldLastSampleFrame = curFrame;
                    const int32_t writeIdx = (s_LiveShieldRingHead + s_LiveShieldRingCount) % kLiveShieldRingCap;
                    if (s_LiveShieldRingCount < kLiveShieldRingCap) {
                        ++s_LiveShieldRingCount;
                    } else {
                        s_LiveShieldRingHead = (s_LiveShieldRingHead + 1) % kLiveShieldRingCap;
                    }
                    s_LiveShieldRing[writeIdx] = { curFrame, s_MatchNetworkStalls };

                    if (g_LiveAntiJitterActive.load(std::memory_order_relaxed)) {
                        if (s_MatchNetworkStalls == s_LiveShieldPrevBucketNetStalls) {
                            ++s_LiveShieldCalmSeconds;
                            if (s_LiveShieldCalmSeconds >= 18) {
                                s_LiveShieldCalmSeconds = 0;
                                g_LiveAntiJitterActive.store(false);
                                g_LiveAntiJitterRedundancyDepth.store(std::clamp(g_Config.redundancy_frame_depth, 1, 5));
                                g_LiveAntiJitterSpinUs.store(std::clamp(g_Config.micro_jitter_spin_us, 50, 2000));
                                if (s_LiveShieldEngageLogs <= 2) {
                                    LogNetcode("[AutoTune-Live] Route stabilized (18s with 0 net stalls) -> Jitter Shield relaxed to baseline (armed).");
                                }
                            }
                        } else {
                            s_LiveShieldCalmSeconds = 0;
                        }
                    }
                    s_LiveShieldPrevBucketNetStalls = s_MatchNetworkStalls;
                }

                if (!g_LiveAntiJitterActive.load(std::memory_order_relaxed) &&
                    curFrame >= evalMinFrm &&
                    ((curFrame - s_LastLiveShieldEvalFrame) >= (sampleStep * 3 / 4) || pSync->m_stallFrameCounter == 12)) {
                    s_LastLiveShieldEvalFrame = curFrame;
                    bool     triggerShield    = false;
                    uint64_t triggerNetStalls = 0;
                    int32_t  triggerSpan      = 1;

                    if (!s_EarlyFightEvaluated && curFrame <= earlyLimit) {
                        const uint64_t netStalls = s_MatchNetStallsAtPeakFrame;
                        const int32_t  evalSpan  = std::max(1, curFrame - 45);
                        if (netStalls >= 5 && (static_cast<double>(netStalls) / static_cast<double>(evalSpan)) >= 0.025) {
                            s_EarlyFightEvaluated = true;
                            triggerShield         = true;
                            triggerNetStalls      = netStalls;
                            triggerSpan           = evalSpan;
                        } else if (curFrame == earlyLimit) {
                            s_EarlyFightEvaluated = true;
                        }
                    }

                    if (!triggerShield && s_LiveShieldRingCount >= 2) {
                        const LiveShieldSample& oldest = s_LiveShieldRing[s_LiveShieldRingHead];
                        const int32_t spanFrames       = curFrame - oldest.frame;
                        const uint64_t minWinStalls    = is30Fps ? 6ULL : 12ULL;
                        if (spanFrames >= (sampleStep * 2) && s_MatchNetworkStalls >= oldest.netStalls) {
                            const uint64_t winNetStalls = s_MatchNetworkStalls - oldest.netStalls;
                            if (winNetStalls >= minWinStalls &&
                                (static_cast<double>(winNetStalls) / static_cast<double>(spanFrames)) >= 0.040) {
                                triggerShield    = true;
                                triggerNetStalls = winNetStalls;
                                triggerSpan      = spanFrames;
                            }
                        }
                    }

                    if (triggerShield) {
                        s_LiveShieldCalmSeconds         = 0;
                        s_LiveShieldPrevBucketNetStalls = s_MatchNetworkStalls;
                        g_LiveAntiJitterActive.store(true);
                        const int32_t shieldDepth = (g_Battler0HasMod.load() && g_Battler1HasMod.load()) ? 4 : 3;
                        g_LiveAntiJitterRedundancyDepth.store(shieldDepth);
                        g_LiveAntiJitterSpinUs.store(1600);

                        if (++s_LiveShieldEngageLogs <= 2) {
                            LogNetcode("[AutoTune-Live] Mid-fight network jitter detected (%llu net stalls over %d f, %0.1f%%) -> Max Jitter Shield ENGAGED (Redundancy=%d, Spin=1600us, RTT-Paced FastNACK)!",
                                       static_cast<unsigned long long>(triggerNetStalls),
                                       triggerSpan,
                                       (static_cast<double>(triggerNetStalls) / static_cast<double>(triggerSpan)) * 100.0,
                                       shieldDepth);
                        }
                    }
                }
            }
        }
        if (pSync->m_bIsStarted && (totalTicks % 300 == 0)) {
            const uint64_t personaSaves  = g_Telemetry.personaPageBoundarySaves.load();
            const uint64_t lobbySaves    = g_Telemetry.lobbySlotNullDerefSaves.load();
            const uint64_t vkSaves       = g_Telemetry.virtualKeyboardNullSaves.load();
            const uint64_t sessDiscSaves = g_Telemetry.sessionDisconnectNullSaves.load();
            const uint64_t dupSlotSaves  = g_Telemetry.duplicateLobbySlotSaves.load();
            const uint64_t lobbyWaitSvs  = g_Telemetry.lobbyWaitDeadlockSaves.load();
            const uint64_t chatTplSvs    = g_Telemetry.chatTemplateCrashSaves.load();
            const uint64_t spSkillSvs    = g_Telemetry.spSkillOobSaves.load();
            const uint64_t taskMgrSvs    = g_Telemetry.taskManagerCrashSaves.load();
            const uint64_t vfxCoordSvs   = GetVfxCoordNullSaves();
            const uint64_t totalCrashSvs = personaSaves + lobbySaves + vkSaves + sessDiscSaves + dupSlotSaves + lobbyWaitSvs + chatTplSvs + spSkillSvs + taskMgrSvs + vfxCoordSvs;
            LogNetcode("[Telemetry] Frame=%d | RTT=%d ms | Delay=%d f | Pkts=%llu (Aux=%llu) | RedundantFrms=%llu | JitterSaved=%llu | FastNACKs=%llu | Stalls=%llu | CrashSaves=%llu (Persona=%llu, LobbySlot=%llu, VKeyboard=%llu, SessDisc=%llu, DupSlot=%llu, LobbyWait=%llu, ChatTpl=%llu, SpSkill=%llu, TaskMgr=%llu, VfxCoord=%llu)",
                       pSync->m_pInputBuffer ? pSync->m_pInputBuffer->m_currentSyncFrame : -1,
                       g_Telemetry.lastRawRttMs.load(),
                       pSync->m_netDelayFrames,
                       static_cast<unsigned long long>(g_Telemetry.packetsBuilt.load()),
                       static_cast<unsigned long long>(g_Telemetry.auxDatagramsSent.load()),
                       static_cast<unsigned long long>(g_Telemetry.redundantFramesSent.load()),
                       static_cast<unsigned long long>(g_Telemetry.jitterPollsRecovered.load()),
                       static_cast<unsigned long long>(g_Telemetry.fastNacksTriggered.load()),
                       static_cast<unsigned long long>(g_Telemetry.lockstepStallFrames.load()),
                       static_cast<unsigned long long>(totalCrashSvs),
                       static_cast<unsigned long long>(personaSaves),
                       static_cast<unsigned long long>(lobbySaves),
                       static_cast<unsigned long long>(vkSaves),
                       static_cast<unsigned long long>(sessDiscSaves),
                       static_cast<unsigned long long>(dupSlotSaves),
                       static_cast<unsigned long long>(lobbyWaitSvs),
                       static_cast<unsigned long long>(chatTplSvs),
                       static_cast<unsigned long long>(spSkillSvs),
                       static_cast<unsigned long long>(taskMgrSvs),
                       static_cast<unsigned long long>(vfxCoordSvs));
        }
    }
}

namespace {

struct HookDescriptor {
    const char*    name;
    uintptr_t      rva;
    const uint8_t* expectedPrologue;
    size_t         prologueLen;
    LPVOID         detourFn;
    LPVOID*        origFnOut;
};

bool VerifyAndInstallHook(const HookDescriptor& desc) {
    uintptr_t targetVa = g_BaseAddress + desc.rva;
    const auto* actualBytes = reinterpret_cast<const uint8_t*>(targetVa);

    if (std::memcmp(actualBytes, desc.expectedPrologue, desc.prologueLen) != 0) {
        LogNetcode("[ERROR] Prologue mismatch for %s at VA 0x%llX (RVA 0x%llX)! Refusing to hook.",
                   desc.name,
                   static_cast<unsigned long long>(targetVa),
                   static_cast<unsigned long long>(desc.rva));
        return false;
    }

    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(targetVa), desc.detourFn, desc.origFnOut);
    if (st != MH_OK) {
        LogNetcode("[ERROR] MH_CreateHook failed for %s at VA 0x%llX (status=%d)",
                   desc.name, static_cast<unsigned long long>(targetVa), static_cast<int>(st));
        return false;
    }

    st = MH_EnableHook(reinterpret_cast<LPVOID>(targetVa));
    if (st != MH_OK) {
        LogNetcode("[ERROR] MH_EnableHook failed for %s at VA 0x%llX (status=%d)",
                   desc.name, static_cast<unsigned long long>(targetVa), static_cast<int>(st));
        return false;
    }

    LogNetcode("[Hook] Installed %-42s @ VA 0x%llX (RVA 0x%07llX)",
               desc.name,
               static_cast<unsigned long long>(targetVa),
               static_cast<unsigned long long>(desc.rva));
    return true;
}

} // namespace

} // namespace StormInternal

void InitializeNetcodeHooks() {
    using namespace StormInternal;

    QueryPerformanceFrequency(&g_QpcFreq);
    g_BaseAddress = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LogNetcode("[Init] NSUNSC.exe ImageBase = 0x%llX", static_cast<unsigned long long>(g_BaseAddress));

    // Resolve internal engine helpers
    g_fnInputBufReset            = reinterpret_cast<fnInputBufReset_t>(g_BaseAddress + StormRva::kInputBuf_Reset);
    g_fnInputBufSeedFrames       = reinterpret_cast<fnInputBufSeedFrames_t>(g_BaseAddress + StormRva::kInputBuf_SeedInitialFrames);
    g_fnInputBufGetOrAdvanceSlot = reinterpret_cast<fnInputBufGetOrAdvanceSlot_t>(g_BaseAddress + StormRva::kInputBuf_GetOrAdvanceSlot);
    g_fnFreeSyncExtractFramePkt  = reinterpret_cast<fnFreeSyncExtractFramePkt_t>(g_BaseAddress + StormRva::kFreeSync_ExtractFramePkt);
    g_fnFreeSyncSerializeOneFrm  = reinterpret_cast<fnFreeSyncSerializeOneFrm_t>(g_BaseAddress + StormRva::kFreeSync_SerializeOneFrame);

    // Byte-verify ccNetUDPSocket::Send (RVA 0x11623B0) prologue before resolving pointer
    static constexpr uint8_t kPrologue_UdpSocketSend[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30
    };
    const auto* udpSendBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kUdpSocket_Send);
    if (std::memcmp(udpSendBytes, kPrologue_UdpSocketSend, sizeof(kPrologue_UdpSocketSend)) == 0) {
        g_fnUdpSocketSend = reinterpret_cast<fnUdpSocketSend_t>(g_BaseAddress + StormRva::kUdpSocket_Send);
    } else {
        LogNetcode("[WARN] ccNetUDPSocket::Send prologue mismatch @ RVA 0x%llX; auxiliary sends disabled.",
                   static_cast<unsigned long long>(StormRva::kUdpSocket_Send));
    }

    // Byte-verify ccSceneNetworkBattleSyncBase::IsHost (RVA 0x06BAB60) prologue before resolving pointer
    static constexpr uint8_t kPrologue_SyncBaseIsHost[] = {
        0x48, 0x8B, 0x05, 0x59, 0x7E, 0xB0, 0x01, 0x48, 0x8B, 0x88, 0x00, 0x01, 0x00, 0x00, 0x48, 0x85, 0xC9
    };
    const auto* isHostBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kSyncBase_IsHost);
    if (std::memcmp(isHostBytes, kPrologue_SyncBaseIsHost, sizeof(kPrologue_SyncBaseIsHost)) == 0) {
        g_fnSyncBaseIsHost = reinterpret_cast<fnSyncBaseIsHost_t>(g_BaseAddress + StormRva::kSyncBase_IsHost);
    } else {
        LogNetcode("[WARN] ccSceneNetworkBattleSyncBase::IsHost prologue mismatch @ RVA 0x%llX; using session member fallback.",
                   static_cast<unsigned long long>(StormRva::kSyncBase_IsHost));
    }

    // Byte-verify ccNetLocalInfo::GetSlotArray (RVA 0x061DCA0) prologue before resolving pointer
    static constexpr uint8_t kPrologue_LocalInfoGetSlotArray[] = {
        0x8B, 0x51, 0x24, 0xE9, 0x28, 0x12, 0x00, 0x00
    };
    const auto* slotArrBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kLocalInfo_GetSlotArray);
    if (std::memcmp(slotArrBytes, kPrologue_LocalInfoGetSlotArray, sizeof(kPrologue_LocalInfoGetSlotArray)) == 0) {
        g_fnLocalInfoGetSlotArray = reinterpret_cast<fnLocalInfoGetSlotArray_t>(g_BaseAddress + StormRva::kLocalInfo_GetSlotArray);
    } else {
        LogNetcode("[WARN] ccNetLocalInfo::GetSlotArray prologue mismatch @ RVA 0x%llX; slot role lookup disabled.",
                   static_cast<unsigned long long>(StormRva::kLocalInfo_GetSlotArray));
    }

    // Byte-verify nuccCriticalSection::Lock (+0x12A7920) & Unlock (+0x12A7990) for ccTaskManager locking
    static constexpr uint8_t kPrologue_NuccCsLock[]   = { 0x83, 0x79, 0x28, 0x00, 0x75, 0x0C, 0x48, 0x8D, 0x0D, 0xDB, 0xD1, 0x8E, 0x00, 0xE9, 0x4E, 0x46 };
    static constexpr uint8_t kPrologue_NuccCsUnlock[] = { 0x83, 0x79, 0x28, 0x00, 0x75, 0x0C, 0x48, 0x8D, 0x0D, 0xAB, 0xD1, 0x8E, 0x00, 0xE9, 0xDE, 0x45 };
    const auto* csLockBytes   = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kNuccCs_Lock);
    const auto* csUnlockBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kNuccCs_Unlock);
    if (std::memcmp(csLockBytes, kPrologue_NuccCsLock, sizeof(kPrologue_NuccCsLock)) == 0 &&
        std::memcmp(csUnlockBytes, kPrologue_NuccCsUnlock, sizeof(kPrologue_NuccCsUnlock)) == 0) {
        g_fnNuccCsLock   = reinterpret_cast<fnNuccCsOp_t>(g_BaseAddress + StormRva::kNuccCs_Lock);
        g_fnNuccCsUnlock = reinterpret_cast<fnNuccCsOp_t>(g_BaseAddress + StormRva::kNuccCs_Unlock);
    } else {
        LogNetcode("[WARN] nuccCriticalSection Lock/Unlock prologue mismatch; ccTaskManager CS lock disabled.");
    }
    InitTaskManagerTombstones();

    // Byte-verify ccVirtualKeyboard_Win64::Reset (+0x10C78A0) for safe close on ~ccNetIdSearchState
    static constexpr uint8_t kPrologue_VkWin64Reset[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x89, 0x4C, 0x24, 0x30, 0x48, 0x8B
    };
    const auto* vkResetBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kVirtualKeyboard_Win64Reset);
    if (std::memcmp(vkResetBytes, kPrologue_VkWin64Reset, sizeof(kPrologue_VkWin64Reset)) == 0) {
        g_fnVirtualKeyboardReset = reinterpret_cast<fnVirtualKeyboardReset_t>(g_BaseAddress + StormRva::kVirtualKeyboard_Win64Reset);
    } else {
        LogNetcode("[WARN] ccVirtualKeyboard_Win64::Reset prologue mismatch @ RVA 0x%llX; using direct state=-1 close fallback.",
                   static_cast<unsigned long long>(StormRva::kVirtualKeyboard_Win64Reset));
    }

    // Byte-verify GetKeyboardDevice (+0x120FE30) for automatic m_pKeyboard recovery in ccVirtualKeyboard_Win64
    static constexpr uint8_t kPrologue_GetKeyboardDevice[] = {
        0x48, 0x63, 0xC2, 0x45, 0x33, 0xC0, 0x48, 0x69, 0xD0, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x81
    };
    const auto* getKbdBytes = reinterpret_cast<const uint8_t*>(g_BaseAddress + StormRva::kInput_GetKeyboardDevice);
    if (std::memcmp(getKbdBytes, kPrologue_GetKeyboardDevice, sizeof(kPrologue_GetKeyboardDevice)) == 0) {
        g_fnGetKeyboardDevice = reinterpret_cast<fnGetKeyboardDevice_t>(g_BaseAddress + StormRva::kInput_GetKeyboardDevice);
    }

    // Apply byte-verified local engine bug fixes
    ApplyCrashAndNetcodeBytePatches(
        g_Config.fix_online_persona_crash || g_Config.enable_netcode_overhaul,
        g_Config.enable_netcode_overhaul);

    if (MH_Initialize() != MH_OK) {
        LogNetcode("[ERROR] MinHook initialization failed!");
        return;
    }

    // Exact .pdata-verified prologues from NSUNSC.exe
    static constexpr uint8_t kPrologue_CopyPersonaA[]         = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_CopyPersonaB[]         = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static constexpr uint8_t kPrologue_GetPersonaWrap[]       = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19 };
    static constexpr uint8_t kPrologue_KeyboardGetChar[]      = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_KeyboardGetKey[]       = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x05, 0xD3, 0x1E, 0x54, 0x08 };
    static constexpr uint8_t kPrologue_NetIdSearchDtor[]      = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83 };
    static constexpr uint8_t kPrologue_NetIdSearchOnVkFin[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48 };
    static constexpr uint8_t kPrologue_SessionSendPkt[]       = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48 };
    static constexpr uint8_t kPrologue_SessionBroadcastPkt[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_CommDestroySockets[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xB9, 0x01, 0x00, 0x00, 0x00, 0xE8, 0x2D };
    static constexpr uint8_t kPrologue_SessionOnMemberLeft[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83 };
    static constexpr uint8_t kPrologue_SessionGetMySlot[]     = { 0x48, 0x8B, 0x49, 0x20, 0x33, 0xD2, 0x48, 0x8D, 0x41, 0x18, 0x48, 0x85, 0xC0, 0x48, 0x0F, 0x44 };
    static constexpr uint8_t kPrologue_SessionSortSlots[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x99, 0x00, 0x05, 0x00 };
    static constexpr uint8_t kPrologue_SessionProcessMsgs[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55 };
    static constexpr uint8_t kPrologue_SessionAddMember[]     = { 0x40, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0xF9, 0x49, 0x8B, 0xE8 };
    static constexpr uint8_t kPrologue_SessionAssignUId[]     = { 0xFF, 0x81, 0x68, 0x01, 0x00, 0x00, 0x8B, 0x81, 0x68, 0x01, 0x00, 0x00, 0x89, 0x02, 0xC3 };
    static constexpr uint8_t kPrologue_SessionInvokeCb[]      = { 0x4C, 0x8B, 0x89, 0x58, 0x06, 0x00, 0x00, 0x49, 0x8B, 0xC9, 0x49, 0x8B, 0x41, 0x08, 0x80, 0x78 };
    static constexpr uint8_t kPrologue_SessionCheckNewMbr[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44 };
    static constexpr uint8_t kPrologue_SessionNewMbrAdd[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x48 };
    static constexpr uint8_t kPrologue_CustomUpdateBattle[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
    static constexpr uint8_t kPrologue_CustomRecvBattle[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41 };
    static constexpr uint8_t kPrologue_CustomUpdateUsers[]    = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
    static constexpr uint8_t kPrologue_TeamGetByIndex[]       = { 0x44, 0x8B, 0xC2, 0x4C, 0x39, 0x41, 0x50 };
    static constexpr uint8_t kPrologue_TeamFindByCharId[]     = { 0x48, 0x8B, 0x49, 0x48, 0x44, 0x8B, 0xCA };
    static constexpr uint8_t kPrologue_TeamFindByIndex[]      = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
    static constexpr uint8_t kPrologue_ChatTplClearDynamic[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
    static constexpr uint8_t kPrologue_SpSkillLeaf[]          = { 0x4C, 0x8B, 0x49, 0x30, 0x49, 0x8B, 0xC9, 0x49, 0x8B, 0x41, 0x08, 0x80, 0x78, 0x19, 0x00, 0x75 };
    static constexpr uint8_t kPrologue_SpSkillGetMsgId[]      = { 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x59, 0x30, 0x44, 0x8B, 0xD2, 0x49, 0x8B, 0xD3, 0x49, 0x8B };
    static constexpr uint8_t kPrologue_SaveDataSyncShadow[]   = { 0x48, 0x83, 0xEC, 0x28, 0x41, 0xB8, 0x18, 0xE3, 0x09, 0x00, 0x48, 0x8D, 0x15, 0x9F, 0xB2, 0x6E };
    static constexpr uint8_t kPrologue_TaskExec[]             = { 0x83, 0x79, 0x20, 0x00, 0x74, 0x0C, 0x85, 0x51, 0x1C, 0x75, 0x07, 0x48, 0x8B, 0x01, 0x48, 0xFF };
    static constexpr uint8_t kPrologue_TaskMgrAddTask[]       = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 };
    static constexpr uint8_t kPrologue_TaskMgrUpdateRange[]   = { 0x40, 0x56, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x41, 0x81, 0xF8, 0x80 };
    static constexpr uint8_t kPrologue_TaskMgrUpdateSingle[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xF2, 0x41, 0x8B };
    static constexpr uint8_t kPrologue_TaskMgrRemoveTask[]    = { 0x48, 0x85, 0xD2, 0x0F, 0x84, 0xEE, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89 };
    static constexpr uint8_t kPrologue_TaskMgrDeleteAll[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };

    static constexpr uint8_t kPrologue_CtrlBattleSync[]       = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
    static constexpr uint8_t kPrologue_CalcNetFrame[]         = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_SetupFrameSync[]       = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_UpdateMemberDly[]      = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x4C, 0x00 };
    static constexpr uint8_t kPrologue_BuildSendPacket[]      = { 0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60 };
    static constexpr uint8_t kPrologue_SendSpectatorPkt[]     = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x50 };
    static constexpr uint8_t kPrologue_SendWatcherRelayPkt[]  = { 0x40, 0x56, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00 };
    static constexpr uint8_t kPrologue_ReceivePackets[]       = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x55 };
    static constexpr uint8_t kPrologue_ReceiveWatcherPkts[]   = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x78, 0xF8, 0xFF, 0xFF };
    static constexpr uint8_t kPrologue_SyncBaseInit[]         = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_RecvHostWorstRtt[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_RecvClientRtt[]        = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
    static constexpr uint8_t kPrologue_ReliableSend[]         = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_NuccD3D11Present[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static constexpr uint8_t kPrologue_NuccGetTimeUs[]        = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x0D, 0xBB, 0x7E, 0x91 };
    static constexpr uint8_t kPrologue_NuccGetTimeMs[]        = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x0D, 0x9B, 0x7D, 0x91 };

    int installedCount = 0;
    int expectedCount  = 0;

    if (g_Config.enable_low_latency_render || g_Config.enable_scheduler_boost || g_Config.enable_precision_frame_pacer) {
        const HookDescriptor renderHook = {
            "nuccD3D11Device::Present (LowLatency)",
            StormRva::kNuccD3D11_Present,
            kPrologue_NuccD3D11Present,
            sizeof(kPrologue_NuccD3D11Present),
            reinterpret_cast<LPVOID>(&hk_NuccD3D11Present),
            reinterpret_cast<LPVOID*>(&oNuccD3D11Present)
        };
        ++expectedCount;
        if (VerifyAndInstallHook(renderHook)) {
            ++installedCount;
        }
    }

    if (g_Config.enable_scheduler_boost || g_Config.enable_precision_frame_pacer) {
        const HookDescriptor timerHooks[] = {
            { "nuccTimer::GetTimeUs (ZeroCore0Pin)", StormRva::kNuccTimer_GetTimeUs, kPrologue_NuccGetTimeUs, sizeof(kPrologue_NuccGetTimeUs), reinterpret_cast<LPVOID>(&hk_NuccGetTimeUs), reinterpret_cast<LPVOID*>(&oNuccGetTimeUs) },
            { "nuccTimer::GetTimeMs (ZeroCore0Pin)", StormRva::kNuccTimer_GetTimeMs, kPrologue_NuccGetTimeMs, sizeof(kPrologue_NuccGetTimeMs), reinterpret_cast<LPVOID>(&hk_NuccGetTimeMs), reinterpret_cast<LPVOID*>(&oNuccGetTimeMs) },
        };
        expectedCount += static_cast<int>(std::size(timerHooks));
        for (const auto& h : timerHooks) {
            if (VerifyAndInstallHook(h)) {
                ++installedCount;
            }
        }
    }

    if (g_Config.fix_online_persona_crash || g_Config.enable_netcode_overhaul) {
        const HookDescriptor crashFixHooks[] = {
            { "Steam::GetFriendPersonaName (SafeRing)",      StormRva::kSteam_GetFriendPersonaName,      kPrologue_GetPersonaWrap,      sizeof(kPrologue_GetPersonaWrap),      reinterpret_cast<LPVOID>(&hk_SteamGetFriendPersonaName),        reinterpret_cast<LPVOID*>(&oSteamGetPersonaWrapper) },
            { "Steam::CopyFriendPersonaNameA (+0x10BAD24)",  StormRva::kSteam_CopyFriendPersonaNameA,    kPrologue_CopyPersonaA,        sizeof(kPrologue_CopyPersonaA),        reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameA),     reinterpret_cast<LPVOID*>(&oSteamCopyPersonaA) },
            { "Steam::CopyFriendPersonaNameB (+0x0847A67)",  StormRva::kSteam_CopyFriendPersonaNameB,    kPrologue_CopyPersonaB,        sizeof(kPrologue_CopyPersonaB),        reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameB),     reinterpret_cast<LPVOID*>(&oSteamCopyPersonaB) },
            { "Input::KeyboardGetPressedKey (+0x120FFB5)",   StormRva::kInput_KeyboardGetPressedKey,     kPrologue_KeyboardGetKey,      sizeof(kPrologue_KeyboardGetKey),      reinterpret_cast<LPVOID>(&hk_KeyboardGetPressedKey),         reinterpret_cast<LPVOID*>(&oKeyboardGetPressedKey) },
            { "Input::KeyboardGetPressedChar (+0x120FEE0)",  StormRva::kInput_KeyboardGetPressedChar,    kPrologue_KeyboardGetChar,     sizeof(kPrologue_KeyboardGetChar),     reinterpret_cast<LPVOID>(&hk_KeyboardGetPressedChar),        reinterpret_cast<LPVOID*>(&oKeyboardGetPressedChar) },
            { "NetIdSearch::~ccNetIdSearchState (+0x16FBB0)",StormRva::kNetIdSearch_Dtor,                kPrologue_NetIdSearchDtor,     sizeof(kPrologue_NetIdSearchDtor),     reinterpret_cast<LPVOID>(&hk_NetIdSearchStateDtor),          reinterpret_cast<LPVOID*>(&oNetIdSearchDtor) },
            { "NetIdSearch::OnVkFinish (+0x170878)",         StormRva::kNetIdSearch_OnVkFinish,          kPrologue_NetIdSearchOnVkFin,  sizeof(kPrologue_NetIdSearchOnVkFin),  reinterpret_cast<LPVOID>(&hk_NetIdSearchStateOnVkFinish),    reinterpret_cast<LPVOID*>(&oNetIdSearchOnVkFinish) },
            { "SessionComm::SendPacket (+0x1169290)",        StormRva::kSessionComm_SendPacket,          kPrologue_SessionSendPkt,      sizeof(kPrologue_SessionSendPkt),      reinterpret_cast<LPVOID>(&hk_SessionCommSendPacket),         reinterpret_cast<LPVOID*>(&oSessionCommSendPkt) },
            { "SessionComm::BroadcastPacket (+0x1169310)",   StormRva::kSessionComm_BroadcastPacket,     kPrologue_SessionBroadcastPkt, sizeof(kPrologue_SessionBroadcastPkt), reinterpret_cast<LPVOID>(&hk_SessionCommBroadcastPacket),    reinterpret_cast<LPVOID*>(&oSessionCommBroadcastPkt) },
            { "SessionComm::DestroySockets (+0x117116C)",    StormRva::kSessionComm_DestroySockets,      kPrologue_CommDestroySockets,  sizeof(kPrologue_CommDestroySockets),  reinterpret_cast<LPVOID>(&hk_SessionCommDestroySockets),     reinterpret_cast<LPVOID*>(&oSessionCommDestroySockets) },
            { "Session::OnMemberLeft (+0x113D1E0)",          StormRva::kSession_OnMemberLeft,            kPrologue_SessionOnMemberLeft, sizeof(kPrologue_SessionOnMemberLeft), reinterpret_cast<LPVOID>(&hk_SessionOnMemberLeft),           reinterpret_cast<LPVOID*>(&oSessionOnMemberLeft) },
            { "Session::GetMySlot (+0x10B8E20)",             StormRva::kSession_GetMySlot,               kPrologue_SessionGetMySlot,    sizeof(kPrologue_SessionGetMySlot),    reinterpret_cast<LPVOID>(&hk_SessionGetMySlot),              reinterpret_cast<LPVOID*>(&oSessionGetMySlot) },
            { "Session::SortSlotsAndRefresh (+0x113FA50)",   StormRva::kSession_SortSlots,               kPrologue_SessionSortSlots,    sizeof(kPrologue_SessionSortSlots),    reinterpret_cast<LPVOID>(&hk_SessionSortSlotsAndRefreshMySlot), reinterpret_cast<LPVOID*>(&oSessionSortSlots) },
            { "Session::ProcessMessages (+0x116DAE0)",       StormRva::kSession_ProcessMessages,         kPrologue_SessionProcessMsgs,  sizeof(kPrologue_SessionProcessMsgs),  reinterpret_cast<LPVOID>(&hk_SessionProcessReceivedMessages), reinterpret_cast<LPVOID*>(&oSessionProcessMsgs) },
            { "Session::AddMemberToTable (+0x113AF60)",      StormRva::kSessionData_AddMember,           kPrologue_SessionAddMember,    sizeof(kPrologue_SessionAddMember),    reinterpret_cast<LPVOID>(&hk_SessionAddMemberToTable),       reinterpret_cast<LPVOID*>(&oSessionAddMember) },
            { "Session::AssignNextUId (+0x113B3C0)",         StormRva::kSession_AssignNextUId,           kPrologue_SessionAssignUId,    sizeof(kPrologue_SessionAssignUId),    reinterpret_cast<LPVOID>(&hk_SessionAssignNextUId),          reinterpret_cast<LPVOID*>(&oSessionAssignNextUId) },
            { "Session::InvokeCallback (+0x113D840)",        StormRva::kSession_InvokeCallback,          kPrologue_SessionInvokeCb,     sizeof(kPrologue_SessionInvokeCb),     reinterpret_cast<LPVOID>(&hk_SessionInvokeCallback),         reinterpret_cast<LPVOID*>(&oSessionInvokeCallback) },
            { "Session::CheckExistingNewMbr (+0x113C8B0)",   StormRva::kSession_CheckExistingNewMbr,     kPrologue_SessionCheckNewMbr,  sizeof(kPrologue_SessionCheckNewMbr),  reinterpret_cast<LPVOID>(&hk_SessionCheckExistingNewMember), reinterpret_cast<LPVOID*>(&oSessionCheckNewMember) },
            { "Session::NewMemberTblAdd (+0x1169E20)",       StormRva::kSession_NewMemberTblAdd,       kPrologue_SessionNewMbrAdd,    sizeof(kPrologue_SessionNewMbrAdd),    reinterpret_cast<LPVOID>(&hk_SessionNewMemberTblAdd),        reinterpret_cast<LPVOID*>(&oSessionNewMemberTblAdd) },
            { "CustomLobby::UpdateBattleTable (+0x00F4310)", StormRva::kCustomLobby_UpdateBattleTable,   kPrologue_CustomUpdateBattle,  sizeof(kPrologue_CustomUpdateBattle),  reinterpret_cast<LPVOID>(&hk_CustomLobbyUpdateBattleTable),  reinterpret_cast<LPVOID*>(&oCustomLobbyUpdateBattleTbl) },
            { "CustomLobby::RecvBattleTable (+0x00F6F30)",   StormRva::kCustomLobby_RecvBattleTable,     kPrologue_CustomRecvBattle,    sizeof(kPrologue_CustomRecvBattle),    reinterpret_cast<LPVOID>(&hk_CustomLobbyRecvBattleTable),    reinterpret_cast<LPVOID*>(&oCustomLobbyRecvBattleTbl) },
            { "CustomLobbyUi::UpdateUserList (+0x00FF810)",  StormRva::kCustomLobbyUi_UpdateUserList,    kPrologue_CustomUpdateUsers,   sizeof(kPrologue_CustomUpdateUsers),   reinterpret_cast<LPVOID>(&hk_CustomLobbyUiUpdateUserList),   reinterpret_cast<LPVOID*>(&oCustomLobbyUiUpdateUsers) },
            { "TeamContainer::GetByIndex (+0x094B3ED)",      StormRva::kTeamContainer_GetByIndex,        kPrologue_TeamGetByIndex,      sizeof(kPrologue_TeamGetByIndex),      reinterpret_cast<LPVOID>(&hk_TeamContainerGetByIndex),       reinterpret_cast<LPVOID*>(&oTeamContainerGetByIndex) },
            { "TeamContainer::FindByCharId (+0x094B837)",    StormRva::kTeamContainer_FindByCharId,      kPrologue_TeamFindByCharId,    sizeof(kPrologue_TeamFindByCharId),    reinterpret_cast<LPVOID>(&hk_TeamContainerFindByCharId),     reinterpret_cast<LPVOID*>(&oTeamContainerFindByCharId) },
            { "TeamContainer::FindByIndex (+0x094B8D8)",     StormRva::kTeamContainer_FindByIndex,       kPrologue_TeamFindByIndex,     sizeof(kPrologue_TeamFindByIndex),     reinterpret_cast<LPVOID>(&hk_TeamContainerFindByIndex),      reinterpret_cast<LPVOID*>(&oTeamContainerFindByIndex) },
            { "ChatTemplate::ClearDynamic (+0x0150C90)",     StormRva::kChatTemplate_ClearDynamic,      kPrologue_ChatTplClearDynamic, sizeof(kPrologue_ChatTplClearDynamic), reinterpret_cast<LPVOID>(&hk_ChatTemplateClearDynamic),      reinterpret_cast<LPVOID*>(&oChatTemplateClearDynamic) },
            { "SpSkill::GetFloatParam1 (+0x0702220)",        StormRva::kSpSkill_GetFloatParam1,         kPrologue_SpSkillLeaf,         sizeof(kPrologue_SpSkillLeaf),         reinterpret_cast<LPVOID>(&hk_SpSkillGetFloatParam1),         reinterpret_cast<LPVOID*>(&oSpSkillGetFloatParam1) },
            { "SpSkill::GetFloatParam2 (+0x0702270)",        StormRva::kSpSkill_GetFloatParam2,         kPrologue_SpSkillLeaf,         sizeof(kPrologue_SpSkillLeaf),         reinterpret_cast<LPVOID>(&hk_SpSkillGetFloatParam2),         reinterpret_cast<LPVOID*>(&oSpSkillGetFloatParam2) },
            { "SpSkill::GetMsgIdPtr (+0x07022C0)",           StormRva::kSpSkill_GetMsgIdPtr,            kPrologue_SpSkillLeaf,         sizeof(kPrologue_SpSkillLeaf),         reinterpret_cast<LPVOID>(&hk_SpSkillGetMsgIdPtr),            reinterpret_cast<LPVOID*>(&oSpSkillGetMsgIdPtr) },
            { "SpSkill::GetDwordParam (+0x0702310)",         StormRva::kSpSkill_GetDwordParam,          kPrologue_SpSkillLeaf,         sizeof(kPrologue_SpSkillLeaf),         reinterpret_cast<LPVOID>(&hk_SpSkillGetDwordParam),          reinterpret_cast<LPVOID*>(&oSpSkillGetDwordParam) },
            { "SpSkill::GetMessageId (+0x0702400)",          StormRva::kSpSkill_GetMessageId,           kPrologue_SpSkillGetMsgId,     sizeof(kPrologue_SpSkillGetMsgId),     reinterpret_cast<LPVOID>(&hk_SpSkillGetMessageId),           reinterpret_cast<LPVOID*>(&oSpSkillGetMessageId) },
            { "SaveData::SyncShadow (+0x0AEED10)",           StormRva::kSaveData_SyncShadow,            kPrologue_SaveDataSyncShadow,  sizeof(kPrologue_SaveDataSyncShadow),  reinterpret_cast<LPVOID>(&hk_SaveDataSyncShadow),            reinterpret_cast<LPVOID*>(&oSaveDataSyncShadow) },
            { "ccTask::Exec (+0x0109C8E0)",                  StormRva::kTask_Exec,                      kPrologue_TaskExec,            sizeof(kPrologue_TaskExec),            reinterpret_cast<LPVOID>(&hk_TaskExec),                      reinterpret_cast<LPVOID*>(&oTaskExec) },
            { "ccTaskManager::AddTask (+0x10D6200)",         StormRva::kTaskMgr_AddTask,                kPrologue_TaskMgrAddTask,      sizeof(kPrologue_TaskMgrAddTask),      reinterpret_cast<LPVOID>(&hk_TaskMgrAddTask),                reinterpret_cast<LPVOID*>(&oTaskMgrAddTask) },
            { "ccTaskManager::UpdateRange (+0x10D6740)",     StormRva::kTaskMgr_UpdateRange,            kPrologue_TaskMgrUpdateRange,  sizeof(kPrologue_TaskMgrUpdateRange),  reinterpret_cast<LPVOID>(&hk_TaskMgrUpdateRange),            reinterpret_cast<LPVOID*>(&oTaskMgrUpdateRange) },
            { "ccTaskManager::UpdateSinglePar (+0x10D6AA0)", StormRva::kTaskMgr_UpdateSingleParallel,   kPrologue_TaskMgrUpdateSingle, sizeof(kPrologue_TaskMgrUpdateSingle), reinterpret_cast<LPVOID>(&hk_TaskMgrUpdateSingleParallel),   reinterpret_cast<LPVOID*>(&oTaskMgrUpdateSinglePar) },
            { "ccTaskManager::RemoveTask (+0x10D6FE0)",      StormRva::kTaskMgr_RemoveTask,             kPrologue_TaskMgrRemoveTask,   sizeof(kPrologue_TaskMgrRemoveTask),   reinterpret_cast<LPVOID>(&hk_TaskMgrRemoveTask),             reinterpret_cast<LPVOID*>(&oTaskMgrRemoveTask) },
            { "ccTaskManager::DeleteAllTasks (+0x10D70E0)",  StormRva::kTaskMgr_DeleteAllTasks,         kPrologue_TaskMgrDeleteAll,    sizeof(kPrologue_TaskMgrDeleteAll),    reinterpret_cast<LPVOID>(&hk_TaskMgrDeleteAllTasks),         reinterpret_cast<LPVOID*>(&oTaskMgrDeleteAllTasks) },
        };
        expectedCount += static_cast<int>(std::size(crashFixHooks));
        for (const auto& h : crashFixHooks) {
            if (VerifyAndInstallHook(h)) {
                ++installedCount;
            }
        }
    }

    if (g_Config.enable_netcode_overhaul) {
        const HookDescriptor netcodeHooks[] = {
            { "BattleBase::CtrlBattleSync",                 StormRva::kBattleBase_CtrlBattleSync,       kPrologue_CtrlBattleSync,    sizeof(kPrologue_CtrlBattleSync),    reinterpret_cast<LPVOID>(&hk_CtrlBattleSync),            reinterpret_cast<LPVOID*>(&oCtrlBattleSync) },
            { "ccNetFreeBattleSync::CalcNetFrameByRtt",     StormRva::kFreeSync_CalcNetFrameByRtt,      kPrologue_CalcNetFrame,      sizeof(kPrologue_CalcNetFrame),      reinterpret_cast<LPVOID>(&hk_CalcNetFrameByRtt),         reinterpret_cast<LPVOID*>(&oCalcNetFrameByRtt) },
            { "ccNetFreeBattleSync::SetupFrameSync",        StormRva::kFreeSync_SetupFrameSync,         kPrologue_SetupFrameSync,    sizeof(kPrologue_SetupFrameSync),    reinterpret_cast<LPVOID>(&hk_SetupFrameSyncAndBuffers),  reinterpret_cast<LPVOID*>(&oSetupFrameSync) },
            { "ccNetFreeBattleSync::UpdateMemberDelay",     StormRva::kFreeSync_UpdateMemberDelay,      kPrologue_UpdateMemberDly,   sizeof(kPrologue_UpdateMemberDly),   reinterpret_cast<LPVOID>(&hk_UpdateMemberCountAndDelay), reinterpret_cast<LPVOID*>(&oUpdateMemberDelay) },
            { "ccNetFreeBattleSync::BuildSendPacket",       StormRva::kFreeSync_BuildSendPacket,        kPrologue_BuildSendPacket,   sizeof(kPrologue_BuildSendPacket),   reinterpret_cast<LPVOID>(&hk_BuildBattlerSendPacket),    reinterpret_cast<LPVOID*>(&oBuildSendPacket) },
            { "ccNetFreeBattleSync::SendSpectatorPacket",   StormRva::kFreeSync_SendSpectatorPacket,    kPrologue_SendSpectatorPkt,  sizeof(kPrologue_SendSpectatorPkt),  reinterpret_cast<LPVOID>(&hk_SendSpectatorPacket),       reinterpret_cast<LPVOID*>(&oSendSpectatorPacket) },
            { "ccNetFreeBattleSync::SendWatcherRelayPacket",StormRva::kFreeSync_SendWatcherRelayPacket, kPrologue_SendWatcherRelayPkt,sizeof(kPrologue_SendWatcherRelayPkt),reinterpret_cast<LPVOID>(&hk_SendWatcherRelayPacket),   reinterpret_cast<LPVOID*>(&oSendWatcherRelayPacket) },
            { "ccNetFreeBattleSync::ReceivePackets",        StormRva::kFreeSync_ReceivePackets,         kPrologue_ReceivePackets,    sizeof(kPrologue_ReceivePackets),    reinterpret_cast<LPVOID>(&hk_ReceivePackets),            reinterpret_cast<LPVOID*>(&oReceivePackets) },
            { "ccNetFreeBattleSync::ReceiveWatcherPackets", StormRva::kFreeSync_ReceiveWatcherPackets,  kPrologue_ReceiveWatcherPkts,sizeof(kPrologue_ReceiveWatcherPkts),reinterpret_cast<LPVOID>(&hk_ReceiveWatcherPackets),     reinterpret_cast<LPVOID*>(&oReceiveWatcherPackets) },
            { "ccSceneNetworkBattleSyncBase::Initialize",   StormRva::kSyncBase_Initialize,             kPrologue_SyncBaseInit,      sizeof(kPrologue_SyncBaseInit),      reinterpret_cast<LPVOID>(&hk_SyncBase_Initialize),       reinterpret_cast<LPVOID*>(&oSyncBaseInit) },
            { "ccSceneNetworkBattleSyncBase::RecvHostRTT",  StormRva::kSyncBase_RecvHostWorstRTT,       kPrologue_RecvHostWorstRtt,  sizeof(kPrologue_RecvHostWorstRtt),  reinterpret_cast<LPVOID>(&hk_SyncBase_RecvHostWorstRTT), reinterpret_cast<LPVOID*>(&oSyncBaseRecvHostWorstRtt) },
            { "ccSceneNetworkBattleSyncBase::RecvPeerRTT",  StormRva::kSyncBase_RecvClientRTT,          kPrologue_RecvClientRtt,     sizeof(kPrologue_RecvClientRtt),     reinterpret_cast<LPVOID>(&hk_SyncBase_RecvClientRTT),    reinterpret_cast<LPVOID*>(&oSyncBaseRecvClientRtt) },
            { "ccNetReliableSendPacket",                    StormRva::kNetReliableSendPacket,           kPrologue_ReliableSend,      sizeof(kPrologue_ReliableSend),      reinterpret_cast<LPVOID>(&hk_NetReliableSendPacket),     reinterpret_cast<LPVOID*>(&oNetReliableSendPacket) },
        };
        expectedCount += static_cast<int>(std::size(netcodeHooks));
        for (const auto& h : netcodeHooks) {
            if (VerifyAndInstallHook(h)) {
                ++installedCount;
            }
        }
    }

    InstallLobbyWaitDeadlockHooks(&installedCount, &expectedCount);
    LogNetcode("[Init] %d/%d .pdata-verified hooks active.", installedCount, expectedCount);
    ApplyProcessGamingBoost();
    ApplyWifiGamingLowLatencyMode(true);
}

void ShutdownNetcodeHooks(bool processTerminating) {
    using namespace StormInternal;

    ShutdownWifiGamingLowLatencyMode(processTerminating);
    ShutdownProcessGamingBoost(processTerminating);
    if (!processTerminating) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        RemoveVfxCoordNullFixBridges();
        RemoveLobbySlotNullFixBridge();
        RestoreVerifiedBytePatches();
    }
}
