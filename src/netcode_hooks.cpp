#include "netcode_internal.h"
#include "MinHook.h"
#include <algorithm>
#include <cstring>
#include <intrin.h>

// ============================================================================
// StormNetcode v2.7 Core Coordinator:
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
std::atomic<int32_t>  g_PeerMinDelayFloor{2};
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

// Live Intra-Match Jitter Shield state (rapid in-fight recalibration)
std::atomic<bool>     g_LiveAntiJitterActive{false};
std::atomic<int32_t>  g_LiveAntiJitterRedundancyDepth{3};
std::atomic<int32_t>  g_LiveAntiJitterSpinUs{1100};
bool                  s_EarlyFightEvaluated     = false;
int32_t               s_LastLiveShieldEvalFrame = 0;

uint64_t s_MatchStartStallCount   = 0;
int32_t  s_MatchPeakSyncFrame     = 0;
uint64_t s_MatchStallsAtPeakFrame = 0;

int32_t s_LastPackedSendFrame          = -1;
int32_t s_SameFrameSendCount           = 0;
int32_t s_LastPackedSpectatorSendFrame = -1;
int32_t s_SameFrameSpectatorSendCount  = 0;
int32_t s_LastStallConfirmedFrame      = -2;
int32_t s_StalledTicksOnConfFrame      = 0;
int32_t s_InitialSpectatorWaitTicks    = 0;

void ResetPerMatchPacingState() noexcept {
    s_LastPackedSendFrame           = -1;
    s_SameFrameSendCount            = 0;
    s_LastPackedSpectatorSendFrame  = -1;
    s_SameFrameSpectatorSendCount   = 0;
    s_LastStallConfirmedFrame       = -2;
    s_StalledTicksOnConfFrame       = 0;
    s_InitialSpectatorWaitTicks     = 0;
    s_MatchStartStallCount          = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed);
    s_MatchPeakSyncFrame            = 0;
    s_MatchStallsAtPeakFrame        = 0;
    g_LiveAntiJitterActive.store(false);
    g_LiveAntiJitterRedundancyDepth.store(std::clamp(g_Config.redundancy_frame_depth, 1, 6));
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

// Original trampoline pointers populated by MinHook
fnCtrlBattleSync_t           oCtrlBattleSync           = nullptr;
fnCalcNetFrameByRtt_t        oCalcNetFrameByRtt        = nullptr;
fnSetupFrameSync_t           oSetupFrameSync           = nullptr;
fnUpdateMemberDelay_t        oUpdateMemberDelay        = nullptr;
fnBuildSendPacket_t          oBuildSendPacket          = nullptr;
fnSendSpectatorPacket_t      oSendSpectatorPacket      = nullptr;
fnReceivePackets_t           oReceivePackets           = nullptr;
fnReceiveWatcherPackets_t    oReceiveWatcherPackets    = nullptr;
fnSyncBaseInit_t             oSyncBaseInit             = nullptr;
fnSyncBaseRecvHostWorstRtt_t oSyncBaseRecvHostWorstRtt = nullptr;
fnSyncBaseRecvClientRtt_t    oSyncBaseRecvClientRtt    = nullptr;
fnNetReliableSendPacket_t    oNetReliableSendPacket    = nullptr;

fnSteamCopyFriendPersona32_t  oSteamCopyPersonaA      = nullptr;
fnSteamCopyFriendPersona32_t  oSteamCopyPersonaB      = nullptr;
fnSteamGetFriendPersonaWrap_t oSteamGetPersonaWrapper = nullptr;
fnNuccD3D11Present_t          oNuccD3D11Present       = nullptr;

// ============================================================================
// 4. Gentle Micro-Jitter Poll & Paced Single-Shot FastNACK Recovery
//    Hook on ccNetFreeBattleSync::ReceivePackets (VA 0x1410F2740)
// ============================================================================

void __fastcall hk_ReceivePackets(ccNetFreeBattleSync* pSync) {
    oReceivePackets(pSync);

    if (!pSync || !pSync->m_pInputBuffer || !pSync->m_bIsStarted || pSync->m_coercionReason != 0) {
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
    if (g_Config.fast_nack_recovery) {
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
        const int32_t nackTick1 = is30Fps ? 2 : (g_LiveAntiJitterActive.load() ? 3 : 2);
        const int32_t nackTick2 = nackTick1 + std::max(5, rttTicks + 2);
        const int32_t retryStep = std::max(6, rttTicks + 3);

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
        static int32_t s_LastWatcherLoggedCoercion = 0;
        if (s_LastWatcherLoggedCoercion != pSync->m_coercionReason) {
            s_LastWatcherLoggedCoercion = pSync->m_coercionReason;
            LogNetcode("[ReceiveWatcher] Match termination flagged: CoercionReason=%d (Frame=%d)",
                       pSync->m_coercionReason,
                       pSync->m_pInputBuffer ? pSync->m_pInputBuffer->m_currentSyncFrame : -1);
        }
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

    // 3. Paced Spectator NACK Recovery:
    //    In vanilla CtrlWatcherInputAdvance (0x1410F43F0), CC2 forgot to reset m_waitCounter (+0x44)
    //    after sending a NACK (`cmp eax, 8; jbe`), causing spectators to spam 60 NACKs/sec on every
    //    tick >= 9 and overflow both Battlers' 8-slot m_spectatorDeque!
    //    We govern m_waitCounter so CtrlWatcherInputAdvance only sends a NACK on paced ticks
    //    (tick 4, 12, 24, and every 20 ticks thereafter) and only when m_bDisconnectFlag == 0.
    const int32_t conf = pBuf->m_confirmedAllPlayersFrame;
    if (conf != s_LastStallConfirmedFrame) {
        s_LastStallConfirmedFrame = conf;
        s_StalledTicksOnConfFrame = 1;
    } else {
        ++s_StalledTicksOnConfFrame;
    }

    bool shouldSendWatcherNack = false;
    if (g_Config.fast_nack_recovery) {
        shouldSendWatcherNack =
            (s_StalledTicksOnConfFrame == 4)  ||
            (s_StalledTicksOnConfFrame == 12) ||
            (s_StalledTicksOnConfFrame == 24) ||
            (s_StalledTicksOnConfFrame > 24 && ((s_StalledTicksOnConfFrame - 24) % 20) == 0);
    } else {
        shouldSendWatcherNack = (s_StalledTicksOnConfFrame >= 9 && (s_StalledTicksOnConfFrame % 9) == 0);
    }

    if (shouldSendWatcherNack && pSync->m_bDisconnectFlag == 0) {
        pSync->m_waitCounter = 8; // CtrlWatcherInputAdvance increments to 9 > 8 and sends 1 clean NACK
        if (g_Config.fast_nack_recovery && s_StalledTicksOnConfFrame <= 12) {
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
        static int32_t s_LastLoggedCoercion = 0;
        if (s_LastLoggedCoercion != pSync->m_coercionReason) {
            s_LastLoggedCoercion = pSync->m_coercionReason;
            LogNetcode("[CtrlBattleSync] CoercionReason=%d detected! (1=Disconnect, 2=Timeout, 3=Desync)",
                       pSync->m_coercionReason);
        }
    }

    if (g_Config.suppress_short_overlay) {
        // For spectators: suppress overlay for at least 180 frames (3 seconds),
        // or up to overlay_threshold_frames if configured higher.
        int32_t threshold = isSpectator
            ? std::max(180, g_Config.overlay_threshold_frames)
            : std::max(30, g_Config.overlay_threshold_frames);
        int32_t trueStallTimer = *pModalTimer;
        // Keep +0x504 below 0x1E (30) until trueStallTimer reaches threshold
        if (trueStallTimer < threshold && trueStallTimer >= 28) {
            *pModalTimer = 28;
        }
    }

    oCtrlBattleSync(pBattleBase);

    // Telemetry periodic report (every 300 battle ticks = 5 seconds at 60 FPS)
    uint64_t totalTicks = g_Telemetry.totalBattleFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pSync) {
        if (pSync->m_bIsStarted && pSync->m_bIsStalled) {
            g_Telemetry.lockstepStallFrames.fetch_add(1, std::memory_order_relaxed);
        }
        if (pSync->m_bIsStarted && pSync->m_pInputBuffer) {
            const int32_t curFrame = pSync->m_pInputBuffer->m_currentSyncFrame;
            if (curFrame > s_MatchPeakSyncFrame) {
                s_MatchPeakSyncFrame     = curFrame;
                s_MatchStallsAtPeakFrame = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed) - s_MatchStartStallCount;
            }

            // Real-time intra-match live recalibration (frames 90 .. 720 = 1.5s .. 12s)
            // ONLY for active battlers (spectators do not transmit inputs or negotiate delay)
            if (!isSpectator && !s_EarlyFightEvaluated && curFrame >= 90 && curFrame <= 720 && (curFrame - s_LastLiveShieldEvalFrame >= 45)) {
                s_LastLiveShieldEvalFrame = curFrame;
                const uint64_t currentStalls = s_MatchStallsAtPeakFrame;
                // If >= 3 stalls or >= 2.0% stall ratio during the first seconds:
                if (currentStalls >= 3 && (static_cast<double>(currentStalls) / static_cast<double>(curFrame)) >= 0.020) {
                    s_EarlyFightEvaluated = true;
                    g_LiveAntiJitterActive.store(true);
                    const int32_t shieldDepth = (g_Battler0HasMod.load() && g_Battler1HasMod.load()) ? 4 : 3;
                    g_LiveAntiJitterRedundancyDepth.store(shieldDepth);
                    g_LiveAntiJitterSpinUs.store(1600);

                    // Pre-arm rematch cushion so rematch starts optimal
                    if (g_PeerDynamicJitterBoostFrames.load() == 0) {
                        g_PeerDynamicJitterBoostFrames.store(1);
                        g_Telemetry.dynamicJitterBoostFrames.store(1);
                    }

                    LogNetcode("[AutoTune-Live] Early-fight jitter detected (%llu stalls at Frame %d, %0.1f%%) -> Max Jitter Shield ENGAGED (Redundancy=%d, Spin=1600us, RTT-Paced FastNACK)!",
                               static_cast<unsigned long long>(currentStalls),
                               curFrame,
                               (static_cast<double>(currentStalls) / static_cast<double>(curFrame)) * 100.0,
                               shieldDepth);
                }
            }
        }
        if (pSync->m_bIsStarted && (totalTicks % 300 == 0)) {
            const uint64_t personaSaves  = g_Telemetry.personaPageBoundarySaves.load();
            const uint64_t lobbySaves    = g_Telemetry.lobbySlotNullDerefSaves.load();
            const uint64_t totalCrashSvs = personaSaves + lobbySaves;
            LogNetcode("[Telemetry] Frame=%d | RTT=%d ms | Delay=%d f | Pkts=%llu (Aux=%llu) | RedundantFrms=%llu | JitterSaved=%llu | FastNACKs=%llu | Stalls=%llu | CrashSaves=%llu (Persona=%llu, LobbySlot=%llu)",
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
                       static_cast<unsigned long long>(lobbySaves));
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

    // Apply byte-verified local engine bug fixes
    ApplyCrashAndNetcodeBytePatches(
        g_Config.fix_online_persona_crash || g_Config.enable_netcode_overhaul,
        g_Config.enable_netcode_overhaul);

    if (MH_Initialize() != MH_OK) {
        LogNetcode("[ERROR] MinHook initialization failed!");
        return;
    }

    // Exact .pdata-verified prologues from NSUNSC.exe
    static constexpr uint8_t kPrologue_CopyPersonaA[]       = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_CopyPersonaB[]       = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static constexpr uint8_t kPrologue_GetPersonaWrap[]     = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19 };

    static constexpr uint8_t kPrologue_CtrlBattleSync[]     = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
    static constexpr uint8_t kPrologue_CalcNetFrame[]       = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_SetupFrameSync[]     = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_UpdateMemberDly[]    = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x4C, 0x00 };
    static constexpr uint8_t kPrologue_BuildSendPacket[]    = { 0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60 };
    static constexpr uint8_t kPrologue_SendSpectatorPkt[]   = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x50 };
    static constexpr uint8_t kPrologue_ReceivePackets[]     = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x55 };
    static constexpr uint8_t kPrologue_ReceiveWatcherPkts[] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x78, 0xF8, 0xFF, 0xFF };
    static constexpr uint8_t kPrologue_SyncBaseInit[]       = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_RecvHostWorstRtt[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_RecvClientRtt[]      = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
    static constexpr uint8_t kPrologue_ReliableSend[]       = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_NuccD3D11Present[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };

    int installedCount = 0;
    int expectedCount  = 0;

    if (g_Config.enable_low_latency_render) {
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

    if (g_Config.fix_online_persona_crash) {
        const HookDescriptor crashFixHooks[] = {
            { "Steam::GetFriendPersonaName (SafeRing)",    StormRva::kSteam_GetFriendPersonaName,   kPrologue_GetPersonaWrap, sizeof(kPrologue_GetPersonaWrap), reinterpret_cast<LPVOID>(&hk_SteamGetFriendPersonaName),    reinterpret_cast<LPVOID*>(&oSteamGetPersonaWrapper) },
            { "Steam::CopyFriendPersonaNameA (+0x10BAD24)",StormRva::kSteam_CopyFriendPersonaNameA, kPrologue_CopyPersonaA,   sizeof(kPrologue_CopyPersonaA),   reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameA), reinterpret_cast<LPVOID*>(&oSteamCopyPersonaA) },
            { "Steam::CopyFriendPersonaNameB (+0x0847A67)",StormRva::kSteam_CopyFriendPersonaNameB, kPrologue_CopyPersonaB,   sizeof(kPrologue_CopyPersonaB),   reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameB), reinterpret_cast<LPVOID*>(&oSteamCopyPersonaB) },
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

    LogNetcode("[Init] %d/%d .pdata-verified hooks active.", installedCount, expectedCount);
    ApplyProcessGamingBoost();
    ApplyWifiGamingLowLatencyMode(true);
}

void ShutdownNetcodeHooks() {
    using namespace StormInternal;

    ShutdownWifiGamingLowLatencyMode();
    ShutdownProcessGamingBoost();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    RemoveLobbySlotNullFixBridge();
    RestoreVerifiedBytePatches();
}
