#include "netcode_internal.h"
#include <algorithm>
#include <cstring>

namespace StormInternal {

namespace {

// Exact midpoint RTT values (in ms) that make vanilla CalcNetFrameByRtt(rtt) at 60 FPS (frameMs = 16)
// and 30 FPS (frameMs = 33) deterministically output D frames for D in [2..16].
constexpr int32_t kSyntheticRttForFrames60[17] = {
    7, 7, 7, 20, 33, 46, 58, 71, 84, 97, 110, 122, 135, 148, 161, 174, 240
};
constexpr int32_t kSyntheticRttForFrames30[17] = {
    14, 14, 14, 41, 67, 93, 120, 146, 173, 199, 225, 252, 278, 305, 331, 357, 420
};

int32_t* GetGlobalNetManagerWorstRttPtr() noexcept {
    if (!g_BaseAddress) return nullptr;
    uintptr_t pNetMgr = *reinterpret_cast<uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
    return pNetMgr ? reinterpret_cast<int32_t*>(pNetMgr + 0x50) : nullptr;
}

uint8_t* GetGlobalNetManagerBytes() noexcept {
    return g_BaseAddress ? *reinterpret_cast<uint8_t* const*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr) : nullptr;
}

int32_t GetLocalMemberId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return -1;
    const uint8_t* pLocalInfo = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x100);
    return pLocalInfo ? *reinterpret_cast<const int32_t*>(pLocalInfo + 0x24) : -1;
}

int32_t GetLocalBattlerIndex() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return -1;
    const uint8_t* pLocalInfo = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x100);
    return pLocalInfo ? *reinterpret_cast<const int32_t*>(pLocalInfo + 0x1C) : -1;
}

const int8_t* GetLobbySlotArray() noexcept {
    if (!g_fnLocalInfoGetSlotArray) return nullptr;
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return nullptr;
    const void* pLocalInfo = *reinterpret_cast<const void* const*>(pNetMgr + 0x100);
    return pLocalInfo ? g_fnLocalInfoGetSlotArray(pLocalInfo) : nullptr;
}

int32_t GetSessionMemberCount() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 2;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    return pSession ? std::clamp(*reinterpret_cast<const int32_t*>(pSession + 0x2C), 0, 16) : 2;
}

int32_t GetSessionHostMemberId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    if (!pSession) return 0;
    const int32_t count = std::clamp(*reinterpret_cast<const int32_t*>(pSession + 0x2C), 0, 16);
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        if (*reinterpret_cast<const int32_t*>(entry + 0x24) != 0) {
            return *reinterpret_cast<const int32_t*>(entry + 0x40);
        }
    }
    return 0;
}

// Identifies the current match's active Battler pair by combining their SteamIDs (even when Spectating)
uint64_t GetRemoteSessionPeerSteamId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    if (!pSession) return 0;

    const int32_t localId      = GetLocalMemberId();
    const int32_t myBattlerIdx = GetLocalBattlerIndex();
    const int8_t* pSlots       = GetLobbySlotArray();
    const int32_t count        = std::clamp(*reinterpret_cast<const int32_t*>(pSession + 0x2C), 0, 16);

    if (pSlots) {
        if (myBattlerIdx == 0 || myBattlerIdx == 1) {
            const int32_t oppMemberId = static_cast<int32_t>(pSlots[1 - myBattlerIdx]);
            if (oppMemberId >= 0) {
                for (int32_t i = 0; i < count; ++i) {
                    const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                    const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                    if (steamId != 0 && *reinterpret_cast<const int32_t*>(entry + 0x40) == oppMemberId) {
                        return steamId;
                    }
                }
            }
        } else {
            const int32_t b0Id = static_cast<int32_t>(pSlots[0]);
            const int32_t b1Id = static_cast<int32_t>(pSlots[1]);
            uint64_t pairHash  = 0;
            for (int32_t i = 0; i < count; ++i) {
                const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
                const uint64_t steamId  = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                if (steamId != 0 && (memberId == b0Id || memberId == b1Id)) {
                    pairHash ^= (steamId + 0x9e3779b97f4a7c15ULL);
                }
            }
            if (pairHash != 0) return pairHash;
        }
    }
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
        if (steamId != 0 && *reinterpret_cast<const int32_t*>(entry + 0x40) != localId) {
            return steamId;
        }
    }
    return 0;
}

int32_t GetDirectOpponentBattlerRttMs() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    const int8_t*  pSlots   = GetLobbySlotArray();
    if (!pSession || !pSlots) return 0;

    const int32_t myBattlerIdx = GetLocalBattlerIndex();
    const int32_t entryCount   = std::clamp(*reinterpret_cast<const int32_t*>(pSession + 0x28), 0, 16);
    if (myBattlerIdx == 0 || myBattlerIdx == 1) {
        const int32_t oppMemberId = static_cast<int32_t>(pSlots[1 - myBattlerIdx]);
        for (int32_t i = 0; i < entryCount; ++i) {
            const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
            if (*reinterpret_cast<const int32_t*>(entry + 0x40) == oppMemberId) {
                return *reinterpret_cast<const int32_t*>(entry + 0x2C);
            }
        }
        return 0;
    }
    const int32_t b0Id = static_cast<int32_t>(pSlots[0]);
    const int32_t b1Id = static_cast<int32_t>(pSlots[1]);
    int32_t maxBattlerRtt = 0;
    for (int32_t i = 0; i < entryCount; ++i) {
        const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
        if (memberId == b0Id || memberId == b1Id) {
            maxBattlerRtt = std::max(maxBattlerRtt, *reinterpret_cast<const int32_t*>(entry + 0x2C));
        }
    }
    return maxBattlerRtt;
}

bool IsLocalSessionHost() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr || *reinterpret_cast<const void* const*>(pNetMgr + 0x100) == nullptr) return false;
    if (g_fnSyncBaseIsHost) return g_fnSyncBaseIsHost(nullptr) != 0;
    const int32_t localId = GetLocalMemberId();
    return (localId >= 0 && localId == GetSessionHostMemberId());
}

void PreSendHostAuthoritativeRttPacket(void* pNetCtrl, int32_t syntheticRtt, int32_t targetDelay, int32_t rawRtt) noexcept {
    if (!pNetCtrl || !oNetReliableSendPacket) return;
    alignas(8) uint8_t hostRttPkt[0x18] = {};
    *reinterpret_cast<uint32_t*>(hostRttPkt + 0x00) = 0x18u;
    hostRttPkt[0x04] = 3u;
    hostRttPkt[0x05] = kHandshakeMagic0;
    hostRttPkt[0x06] = kHandshakeMagic1;
    hostRttPkt[0x07] = static_cast<uint8_t>(std::clamp(targetDelay, 1, 16));
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x08) = GetSessionHostMemberId();
    *reinterpret_cast<uint32_t*>(hostRttPkt + 0x0C) = 0x09u;
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x10) = syntheticRtt;
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x14) = rawRtt;
    oNetReliableSendPacket(pNetCtrl, hostRttPkt, 0x18u, -1);
}

void ComputeHostNegotiationState(int32_t rawRtt, int32_t& outTargetDelay, int32_t& outSyntheticRtt) noexcept {
    const int32_t safeRawRtt         = std::max(0, rawRtt);
    const bool    bothBattlersModded = g_Battler0HasMod.load() && g_Battler1HasMod.load();
    const bool    anyBattlerModded   = g_Battler0HasMod.load() || g_Battler1HasMod.load();
    const bool    is30Fps            = (GetEngineBattleFps() == 30);
    g_Telemetry.peerHasModHandshake.store(bothBattlersModded);

    const int32_t lobbyFloor = (GetSessionMemberCount() > 2 || safeRawRtt > 8) ? 2 : 1;
    int32_t minFloor = 3;
    if (bothBattlersModded) {
        minFloor = std::max(lobbyFloor, std::max(g_Config.min_delay_frames, g_PeerMinDelayFloor.load()));
    } else if (anyBattlerModded) {
        const int32_t mixedFloor = (is30Fps || safeRawRtt <= 22) ? 2 : 3;
        minFloor = std::max(mixedFloor, g_Config.min_delay_frames);
    } else {
        minFloor = std::max(is30Fps ? 2 : 3, g_Config.min_delay_frames);
    }
    outTargetDelay  = ComputeHostAuthoritativeTargetDelay(safeRawRtt, minFloor, g_Config.max_delay_clamp, bothBattlersModded);
    outSyntheticRtt = GetSyntheticRttForFrames(outTargetDelay);
}

int32_t ResolveTargetDelayForSync(ccNetFreeBattleSync* pSync, int32_t rttMs) {
    const int32_t extraOffset = pSync ? pSync->m_extraDelayOffset : 0;
    int32_t rawRtt = g_Telemetry.lastRawRttMs.load();
    if (rawRtt <= 0) rawRtt = rttMs;

    const int32_t vanillaDelay = ComputeVanillaDelayFrames(rawRtt, extraOffset);
    g_Telemetry.lastVanillaDelayFrames.store(vanillaDelay);

    if (!g_Config.enable_custom_delay || g_Config.delay_mode == 0) {
        return ComputeVanillaDelayFrames(rttMs, extraOffset);
    }
    if (g_Config.delay_mode == 2) {
        const int32_t minFloor = std::max(1, g_Config.min_delay_frames);
        return std::clamp(ComputeOptimalOneWayDelayFrames(rawRtt, minFloor, g_Config.max_delay_clamp) + extraOffset, 1, 16);
    }

    const bool isHost = IsLocalSessionHost();
    g_Telemetry.localIsHost.store(isHost);
    if (!isHost) {
        if (g_HostHandshakeObserved.load()) {
            const int32_t negotiated = g_NegotiatedTargetDelay.load();
            if (negotiated >= 1) return std::clamp(negotiated + extraOffset, 1, 16);
        }
        return ComputeVanillaDelayFrames(rttMs, extraOffset);
    }

    const int32_t negotiated = g_NegotiatedTargetDelay.load();
    if (negotiated >= 1) return std::clamp(negotiated + extraOffset, 1, 16);

    int32_t targetDelay = 2, syntheticRtt = 7;
    ComputeHostNegotiationState(rawRtt, targetDelay, syntheticRtt);
    return std::clamp(targetDelay + extraOffset, 1, 16);
}

} // namespace

// ============================================================================
// Delay Math & Wire-Protocol Validation Helpers (30 FPS & 60 FPS Engine-Aware)
// ============================================================================

int32_t GetEngineBattleFps() noexcept {
    if (!g_BaseAddress) return 60;
    const uintptr_t pFramework = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalGameFrameworkPtr);
    if (!pFramework) return 60;
    return (*reinterpret_cast<const uint8_t*>(pFramework + 0x952) == 30) ? 30 : 60;
}

double GetEngineTrueFramePeriodMs() noexcept {
    return (GetEngineBattleFps() == 30) ? 33.3333 : 16.6667;
}

int32_t GetSyntheticRttForFrames(int32_t targetFrames) noexcept {
    const int32_t clamped = std::clamp(targetFrames, 2, 16);
    return (GetEngineBattleFps() == 30) ? kSyntheticRttForFrames30[clamped] : kSyntheticRttForFrames60[clamped];
}

int32_t ComputeVanillaDelayFrames(int32_t rttMs, int32_t extraOffset) {
    const int32_t frameMs = (GetEngineBattleFps() == 30) ? 33 : 16;
    const int32_t adj     = rttMs + (rttMs / 4);
    const int32_t frames  = (adj + (frameMs - 1)) / frameMs + extraOffset + 1;
    return std::clamp(frames, 2, 16);
}

int32_t ComputeOptimalOneWayDelayFrames(int32_t rttMs, int32_t minFloor, int32_t maxClamp) {
    int32_t safeMin = std::clamp(minFloor, 1, 16);
    int32_t safeMax = std::clamp(maxClamp, safeMin, 16);

    if (g_Config.forced_delay_frames >= 1) {
        return std::clamp(g_Config.forced_delay_frames, safeMin, safeMax);
    }

    const bool    is30Fps    = (GetEngineBattleFps() == 30);
    const int32_t clampedRtt = std::max(0, rttMs);
    const int32_t owdMs      = (clampedRtt + 1) / 2;

    const int32_t phaseSafeMin = (clampedRtt > 8) ? std::max(2, safeMin) : safeMin;
    const int32_t baseMarginMs = std::clamp(g_Config.jitter_margin_ms, 2, 6);
    // In 30 FPS mode, each simulation step is 33.33 ms (2x wider than 60 FPS), so include +14ms phase margin
    const int32_t cushionMs = is30Fps
        ? (baseMarginMs + 14 + (clampedRtt / 12))
        : (baseMarginMs + (clampedRtt / 20) + std::max(0, (clampedRtt - 140) / 16));
    const int32_t budgetMs     = owdMs + cushionMs;
    const int32_t targetFrames = is30Fps
        ? std::max(1, (budgetMs * 3 + 99) / 100)
        : std::max(1, (budgetMs * 3 + 49) / 50);

    return std::clamp(targetFrames, phaseSafeMin, std::max(safeMax, phaseSafeMin));
}

int32_t ComputeHostAuthoritativeTargetDelay(int32_t rttMs, int32_t minFloor, int32_t maxClamp, bool peerModded) {
    const bool    is30Fps    = (GetEngineBattleFps() == 30);
    const int32_t clampedRtt = std::max(0, rttMs);
    const int32_t maxBoost   = is30Fps ? 1 : 2; // At 30 FPS, +1 frame is already +33.3ms (2x 60FPS frames)

    if (!peerModded && g_Config.forced_delay_frames < 1) {
        const int32_t vanillaDelay     = ComputeVanillaDelayFrames(clampedRtt, 0);
        const int32_t owdMs            = (clampedRtt + 1) / 2;
        const bool    anyBattlerModded = g_Battler0HasMod.load() || g_Battler1HasMod.load();

        if (is30Fps) {
            // In 30 FPS (framePeriod = 33.33ms), account for the 33.3ms 30Hz sampling phase offset:
            // At RTT=230ms (OWD=115ms), cushion30 = 26 + 23 = 49ms -> budget = 164ms -> 5f up to 180ms, 6f (200ms) at 230ms (vs Vanilla 10f = 333ms).
            const int32_t cushion30 = anyBattlerModded ? (26 + clampedRtt / 10) : (32 + clampedRtt / 9);
            int32_t safeDelay30     = std::clamp(((owdMs + cushion30) * 3 + 99) / 100, 2, std::max(2, vanillaDelay - 1));
            if (g_Config.enable_dynamic_peer_autotune && anyBattlerModded) {
                const int32_t boost = std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, maxBoost);
                safeDelay30 = std::clamp(safeDelay30 + boost, 2, vanillaDelay);
            }
            return std::clamp(safeDelay30, std::max(2, minFloor), std::max(2, maxClamp));
        }

        const int32_t cushion60 = anyBattlerModded ? (15 + (clampedRtt / 14)) : (20 + (clampedRtt / 12));
        const int32_t minSafe60 = (anyBattlerModded && clampedRtt <= 22) ? 2 : 3;
        int32_t safeDelay       = std::clamp(((owdMs + cushion60) * 3 + 49) / 50, minSafe60, std::max(minSafe60, vanillaDelay - 1));

        if (g_Config.enable_dynamic_peer_autotune && anyBattlerModded) {
            const int32_t boost = std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, maxBoost);
            safeDelay = std::clamp(safeDelay + boost, minSafe60, vanillaDelay);
        }
        return std::clamp(safeDelay, std::max(minSafe60, minFloor), std::max(minSafe60, maxClamp));
    }

    int32_t baseTarget = ComputeOptimalOneWayDelayFrames(clampedRtt, minFloor, maxClamp);
    if (g_Config.enable_dynamic_peer_autotune && g_Config.forced_delay_frames < 1) {
        const int32_t boost = std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, maxBoost);
        if (boost > 0) {
            const int32_t safeMax = std::clamp(maxClamp, 1, 16);
            baseTarget = std::clamp(baseTarget + boost, 1, std::max(baseTarget, safeMax));
        }
    }
    return baseTarget;
}

// ============================================================================
// 1. Pre-Match RTT Negotiation & Peer Handshake Hooks (ccSceneNetworkBattleSyncBase)
// ============================================================================

void __fastcall hk_SyncBase_Initialize(void* pSyncBase) {
    const int32_t  prevMatchPeakFrame  = s_MatchPeakSyncFrame;
    const uint64_t prevMatchStallCount = s_MatchStallsAtPeakFrame;

    ResetPerMatchPacingState();
    oSyncBaseInit(pSyncBase);

    const uint64_t nowTick       = GetTickCount64();
    const uint64_t prevTick      = g_LastSyncInitTickMs.load();
    const uint64_t remoteSteamId = GetRemoteSessionPeerSteamId();
    const uint64_t prevPeerId    = g_LastNegotiatedPeerSteamId.load();
    const bool     isHost        = IsLocalSessionHost();
    const int32_t  myBattlerIdx  = GetLocalBattlerIndex();

    const bool isPhase2SameMatch =
        g_NegotiationPendingForMatch.load() &&
        (nowTick >= prevTick && (nowTick - prevTick) < 15000ULL) &&
        (remoteSteamId == 0 || prevPeerId == 0 || remoteSteamId == prevPeerId);

    if (!isPhase2SameMatch) {
        if (remoteSteamId != 0 && prevPeerId != 0 && remoteSteamId != prevPeerId) {
            g_PeerDynamicJitterBoostFrames.store(0);
            g_Telemetry.dynamicJitterBoostFrames.store(0);
        } else if (g_Config.enable_dynamic_peer_autotune && prevMatchPeakFrame >= 450 && myBattlerIdx >= 0) {
            const double stallRatio = static_cast<double>(prevMatchStallCount) /
                                      static_cast<double>(prevMatchPeakFrame);
            const bool    is30Fps  = (GetEngineBattleFps() == 30);
            const int32_t maxBoost = is30Fps ? 1 : 2;
            if (stallRatio >= 0.018) {
                const int32_t stepBoost = (!is30Fps && stallRatio >= 0.120) ? 2 : 1;
                const int32_t prevBoost = g_PeerDynamicJitterBoostFrames.load();
                const int32_t newBoost  = std::clamp(prevBoost + stepBoost, 1, maxBoost);
                if (newBoost != prevBoost) {
                    g_PeerDynamicJitterBoostFrames.store(newBoost);
                    g_Telemetry.dynamicJitterBoostFrames.store(newBoost);
                    LogNetcode("[AutoTune] Rematch vs same peer had %0.2f%% mid-fight stalls (%llu/%d f) -> +%d frame cushion!",
                               stallRatio * 100.0,
                               static_cast<unsigned long long>(prevMatchStallCount),
                               prevMatchPeakFrame,
                               newBoost);
                }
            } else if (stallRatio < 0.005 && prevMatchPeakFrame >= 3600) {
                LogNetcode("[AutoTune] Route verified rock-solid (%0.2f%% stalls over %d f). Optimal pacing retained.",
                           stallRatio * 100.0, prevMatchPeakFrame);
            }
        }

        g_NegotiatedTargetDelay.store(-1);
        g_PeerMinDelayFloor.store(2);
        g_CapturedHostRawRttMs.store(0);
        g_ClientTrueRawRttMs.store(0);
        g_LastSentSyntheticRttMs.store(0);
        g_AuthoritativeHostRttMs.store(0);
        g_HostHandshakeObserved.store(false);
        g_Battler0HasMod.store(myBattlerIdx == 0);
        g_Battler1HasMod.store(myBattlerIdx == 1);
        g_Telemetry.peerHasModHandshake.store(false);
        ApplyWifiGamingLowLatencyMode(false);
    }

    g_Telemetry.localIsHost.store(isHost);
    g_LastSyncInitTickMs.store(nowTick);
    if (remoteSteamId != 0) {
        g_LastNegotiatedPeerSteamId.store(remoteSteamId);
    }
    g_NegotiationPendingForMatch.store(true);

    // Only seed initial raw RTT on Initialize when local player is an active Battler (0 or 1).
    if (myBattlerIdx >= 0) {
        if (int32_t* pWorstRtt = GetGlobalNetManagerWorstRttPtr()) {
            int32_t initRawRtt = (*pWorstRtt > 0) ? *pWorstRtt : GetDirectOpponentBattlerRttMs();
            const int32_t lastSynth = g_LastSentSyntheticRttMs.load();
            const int32_t authSynth = g_AuthoritativeHostRttMs.load();
            if (initRawRtt > 0 && initRawRtt != lastSynth && initRawRtt != authSynth) {
                if (initRawRtt > g_CapturedHostRawRttMs.load()) g_CapturedHostRawRttMs.store(initRawRtt);
                if (initRawRtt > g_ClientTrueRawRttMs.load()) g_ClientTrueRawRttMs.store(initRawRtt);
                if (isHost || !isPhase2SameMatch || g_Telemetry.lastRawRttMs.load() <= 0) {
                    g_Telemetry.lastRawRttMs.store(g_CapturedHostRawRttMs.load());
                }
            }
        }
    }
}

void __fastcall hk_SyncBase_RecvClientRTT(void* pSyncBase, void* pArg2, uint8_t* pPacket) {
    const bool isHost = IsLocalSessionHost();
    if (pPacket && *reinterpret_cast<const uint32_t*>(pPacket + 0x0C) == 0x10) {
        const int32_t senderId = *reinterpret_cast<const int32_t*>(pPacket + 0x08);
        const int32_t localId  = GetLocalMemberId();
        if (isHost && (localId < 0 || senderId != localId)) {
            const int32_t clientRtt    = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
            const int32_t myBattlerIdx = GetLocalBattlerIndex();
            const int8_t* pSlots       = GetLobbySlotArray();
            const int32_t b0Id         = pSlots ? static_cast<int32_t>(pSlots[0]) : -1;
            const int32_t b1Id         = pSlots ? static_cast<int32_t>(pSlots[1]) : -1;

            const bool isSenderB0 = (b0Id >= 0 && senderId == b0Id) || (b0Id < 0 && myBattlerIdx == 1);
            const bool isSenderB1 = (b1Id >= 0 && senderId == b1Id) || (b1Id < 0 && myBattlerIdx == 0);
            const bool isWatcherHandshake = (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeWatcherMagic1);
            const bool isSenderBattler   = (isSenderB0 || isSenderB1) && !isWatcherHandshake;
            const bool isSenderSpectator = isWatcherHandshake || (b0Id >= 0 && b1Id >= 0 && !isSenderB0 && !isSenderB1);

            const int32_t lastSynth = g_LastSentSyntheticRttMs.load();
            const int32_t prevRaw   = g_CapturedHostRawRttMs.load();
            if (!isSenderSpectator && clientRtt > prevRaw && clientRtt != lastSynth && (isSenderBattler || prevRaw <= 0)) {
                g_CapturedHostRawRttMs.store(clientRtt);
                g_Telemetry.lastRawRttMs.store(clientRtt);
            }

            if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeMagic1) {
                const int32_t peerFloor = std::clamp<int32_t>(pPacket[7], 1, 16);
                if (isSenderB0) g_Battler0HasMod.store(true);
                if (isSenderB1) g_Battler1HasMod.store(true);
                if (isSenderBattler) {
                    const int32_t prevFloor = g_PeerMinDelayFloor.load();
                    g_PeerMinDelayFloor.store((prevFloor <= 2) ? peerFloor : std::max(prevFloor, peerFloor));
                }
                LogNetcode("[Handshake] Remote battler (member=%d) has StormNetcode mod (min_floor=%d, clientRTT=%d ms)",
                           senderId, peerFloor, clientRtt);
            } else if (isWatcherHandshake) {
                LogNetcode("[Handshake] Remote spectator (member=%d) has StormNetcode mod", senderId);
            }

            g_Telemetry.peerHasModHandshake.store(g_Battler0HasMod.load() && g_Battler1HasMod.load());
            if (isSenderSpectator || (g_Config.enable_custom_delay && g_Config.delay_mode >= 1)) {
                *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
            }
        }
    }
    oSyncBaseRecvClientRtt(pSyncBase, pArg2, pPacket);
}

void __fastcall hk_SyncBase_RecvHostWorstRTT(void* pSyncBase, void* pArg2, uint8_t* pPacket) {
    if (pPacket) {
        const uint32_t subCmd = *reinterpret_cast<const uint32_t*>(pPacket + 0x0C);
        if (subCmd == 0x09) {
            g_Telemetry.localIsHost.store(false);
            const int32_t broadcastRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
            g_AuthoritativeHostRttMs.store(broadcastRtt);
            g_Telemetry.lastNegotiatedRttMs.store(broadcastRtt);
            g_NegotiationPendingForMatch.store(true);

            if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeMagic1) {
                g_HostHandshakeObserved.store(true);
                g_Telemetry.peerHasModHandshake.store(true);
                const int32_t hostTargetDelay = std::clamp<int32_t>(pPacket[7], 1, 16);
                const int32_t rawHostRtt      = *reinterpret_cast<const int32_t*>(pPacket + 0x14);
                if (rawHostRtt > 0 && (rawHostRtt != broadcastRtt || g_Telemetry.lastRawRttMs.load() <= 0)) {
                    g_Telemetry.lastRawRttMs.store(rawHostRtt);
                }
                g_NegotiatedTargetDelay.store(hostTargetDelay);
                LogNetcode("[Handshake] Host has StormNetcode mod! Raw RTT=%d ms -> Agreed Delay=%d frames",
                           g_Telemetry.lastRawRttMs.load(), hostTargetDelay);
            } else {
                g_HostHandshakeObserved.store(false);
                g_Telemetry.peerHasModHandshake.store(false);
                g_Telemetry.lastRawRttMs.store(broadcastRtt);
                const int32_t vanillaDelay = ComputeVanillaDelayFrames(broadcastRtt, 0);
                g_NegotiatedTargetDelay.store(vanillaDelay);
                LogNetcode("[Handshake] Host is unmodded (Broadcast RTT=%d ms -> Lockstep-Safe Delay=%d frames)",
                           broadcastRtt, vanillaDelay);
            }
        }
    }
    oSyncBaseRecvHostWorstRtt(pSyncBase, pArg2, pPacket);
}

int32_t __fastcall hk_NetReliableSendPacket(void* pNetCtrl, uint8_t* pPacket, uint32_t packetSize, int32_t targetId) {
    if (pPacket && packetSize == 0x18) {
        const uint32_t pktLen = *reinterpret_cast<const uint32_t*>(pPacket + 0x00);
        const uint8_t  chan   = pPacket[0x04];
        const uint32_t subCmd = *reinterpret_cast<const uint32_t*>(pPacket + 0x0C);
        const bool     isHost = IsLocalSessionHost();

        if (pktLen == 0x18 && chan == 4 && subCmd == 0x10) {
            if (!isHost) {
                int32_t myRawRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                const int32_t authSynth = g_AuthoritativeHostRttMs.load();
                if (myRawRtt > 0 && myRawRtt != authSynth) {
                    if (myRawRtt > g_ClientTrueRawRttMs.load()) g_ClientTrueRawRttMs.store(myRawRtt);
                    if (!g_HostHandshakeObserved.load() || g_Telemetry.lastRawRttMs.load() <= 0) {
                        g_Telemetry.lastRawRttMs.store(myRawRtt);
                    }
                } else if (g_ClientTrueRawRttMs.load() > 0) {
                    myRawRtt = g_ClientTrueRawRttMs.load();
                    *reinterpret_cast<int32_t*>(pPacket + 0x10) = myRawRtt;
                }

                const int32_t myBattlerIdx = GetLocalBattlerIndex();
                if (myBattlerIdx == -1) {
                    *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
                    pPacket[5] = kHandshakeMagic0;
                    pPacket[6] = kHandshakeWatcherMagic1;
                    pPacket[7] = 2u;
                } else {
                    const int32_t maxBoost = (GetEngineBattleFps() == 30) ? 1 : 2;
                    int32_t desiredFloor   = std::clamp(g_Config.min_delay_frames, 1, 16);
                    const int32_t localBoost = g_Config.enable_dynamic_peer_autotune
                        ? std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, maxBoost)
                        : 0;
                    if (localBoost > 0 && myRawRtt > 0 && g_Config.forced_delay_frames < 1) {
                        const int32_t baseTarget = ComputeOptimalOneWayDelayFrames(myRawRtt, desiredFloor, g_Config.max_delay_clamp);
                        desiredFloor = std::clamp(baseTarget + localBoost, desiredFloor, std::clamp(g_Config.max_delay_clamp, 1, 16));
                    }
                    pPacket[5] = kHandshakeMagic0;
                    pPacket[6] = kHandshakeMagic1;
                    pPacket[7] = static_cast<uint8_t>(desiredFloor);
                }
            } else {
                pPacket[5] = 0;
                pPacket[6] = 0;
                pPacket[7] = 0;
                g_Telemetry.localIsHost.store(true);

                const int32_t senderId      = *reinterpret_cast<const int32_t*>(pPacket + 0x08);
                const int32_t localHostId   = GetLocalMemberId();
                const bool    isHostOwnSend = (localHostId >= 0 && senderId == localHostId);
                const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                const int32_t lastSynth     = g_LastSentSyntheticRttMs.load();
                int32_t rawRtt = g_CapturedHostRawRttMs.load();
                if (currentPktRtt > rawRtt && currentPktRtt != lastSynth) {
                    rawRtt = currentPktRtt;
                    g_CapturedHostRawRttMs.store(rawRtt);
                }
                if (rawRtt <= 0) {
                    rawRtt = GetDirectOpponentBattlerRttMs();
                    if (rawRtt > 0) g_CapturedHostRawRttMs.store(rawRtt);
                }
                if (rawRtt > 0) g_Telemetry.lastRawRttMs.store(rawRtt);

                if (!isHostOwnSend) {
                    if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
                        *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
                    }
                } else if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
                    int32_t targetDelay = 2, syntheticRtt = 7;
                    ComputeHostNegotiationState(rawRtt, targetDelay, syntheticRtt);
                    g_LastSentSyntheticRttMs.store(syntheticRtt);
                    *reinterpret_cast<int32_t*>(pPacket + 0x10) = syntheticRtt;
                    if (int32_t* pMgrWorstRtt = GetGlobalNetManagerWorstRttPtr()) *pMgrWorstRtt = syntheticRtt;
                    g_NegotiatedTargetDelay.store(targetDelay);
                    g_NegotiationPendingForMatch.store(true);
                    g_Telemetry.lastNegotiatedRttMs.store(syntheticRtt);
                    PreSendHostAuthoritativeRttPacket(pNetCtrl, syntheticRtt, targetDelay, rawRtt);
                }
            }
        } else if (pktLen == 0x18 && chan == 3 && subCmd == 0x09) {
            g_Telemetry.localIsHost.store(true);
            const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
            const int32_t lastSynth     = g_LastSentSyntheticRttMs.load();
            int32_t rawRtt = g_CapturedHostRawRttMs.load();
            if (rawRtt <= 0 && currentPktRtt > 0 && currentPktRtt != lastSynth) {
                rawRtt = currentPktRtt;
                g_CapturedHostRawRttMs.store(rawRtt);
            }
            if (rawRtt <= 0) {
                rawRtt = GetDirectOpponentBattlerRttMs();
                if (rawRtt > 0) g_CapturedHostRawRttMs.store(rawRtt);
            }
            if (rawRtt > 0) g_Telemetry.lastRawRttMs.store(rawRtt);

            if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
                int32_t targetDelay = 2, syntheticRtt = 7;
                ComputeHostNegotiationState(rawRtt, targetDelay, syntheticRtt);
                g_LastSentSyntheticRttMs.store(syntheticRtt);
                *reinterpret_cast<int32_t*>(pPacket + 0x10) = syntheticRtt;
                if (int32_t* pMgrWorstRtt = GetGlobalNetManagerWorstRttPtr()) *pMgrWorstRtt = syntheticRtt;
                pPacket[5] = kHandshakeMagic0;
                pPacket[6] = kHandshakeMagic1;
                pPacket[7] = static_cast<uint8_t>(targetDelay);
                *reinterpret_cast<int32_t*>(pPacket + 0x14) = rawRtt;
                g_NegotiatedTargetDelay.store(targetDelay);
                g_NegotiationPendingForMatch.store(true);
                g_Telemetry.lastNegotiatedRttMs.store(syntheticRtt);

                LogNetcode("[RTT-Negotiation] Host authoritative (%d FPS): RawRTT=%d ms (OWD=%d ms, Vanilla=%d f) -> TargetDelay=%d f (%0.1f ms, SyntheticRTT=%d ms, BattlersModded=%d/%d)",
                           GetEngineBattleFps(), rawRtt, (rawRtt + 1) / 2, ComputeVanillaDelayFrames(rawRtt, 0),
                           targetDelay, targetDelay * GetEngineTrueFramePeriodMs(), syntheticRtt,
                           (g_Battler0HasMod.load() ? 1 : 0) + (g_Battler1HasMod.load() ? 1 : 0), 2);
            }
        }
    }
    return oNetReliableSendPacket(pNetCtrl, pPacket, packetSize, targetId);
}

// ============================================================================
// 2. Input Delay Calculation & Buffer Seeding Hooks (ccNetFreeBattleSync)
// ============================================================================

int32_t __fastcall hk_CalcNetFrameByRtt(ccNetFreeBattleSync* pSync, int32_t rttMs) {
    int32_t vanilla = oCalcNetFrameByRtt(pSync, rttMs);
    int32_t target  = ResolveTargetDelayForSync(pSync, rttMs);
    g_Telemetry.activeDelayFrames.store(target);
    LogNetcode("[CalcNetFrameByRtt] rtt=%d ms | vanilla=%d frames | active=%d frames", rttMs, vanilla, target);
    return target;
}

void __fastcall hk_SetupFrameSyncAndBuffers(ccNetFreeBattleSync* pSync) {
    ResetPerMatchPacingState();
    if (!pSync) {
        oSetupFrameSync(pSync);
        return;
    }

    const int32_t savedRtt = pSync->m_worstRttMs;
    int32_t targetDelay    = std::clamp(ResolveTargetDelayForSync(pSync, savedRtt), 1, 16);
    g_NegotiationPendingForMatch.store(false);

    pSync->m_worstRttMs = GetSyntheticRttForFrames(std::clamp(std::max(targetDelay, 8), 2, 16));
    oSetupFrameSync(pSync);
    pSync->m_worstRttMs = savedRtt;

    if (pSync->m_netDelayFrames != targetDelay && pSync->m_pInputBuffer &&
        g_fnInputBufReset && g_fnInputBufSeedFrames) {
        pSync->m_netDelayFrames = targetDelay;
        g_fnInputBufReset(pSync->m_pInputBuffer);
        g_fnInputBufSeedFrames(pSync->m_pInputBuffer, targetDelay);
        pSync->m_sendFrameCounter = pSync->m_pInputBuffer->m_confirmedAllPlayersFrame;
    }

    PreWarmBattleUdpChannel(pSync);
    s_MatchStartStallCount   = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed);
    s_MatchPeakSyncFrame     = 0;
    s_MatchStallsAtPeakFrame = 0;
    g_Telemetry.activeDelayFrames.store(pSync->m_netDelayFrames);

    if (pSync->m_myPlayerIdx == -1) {
        LogNetcode("[StartFrameSync] Match initialized as Spectator (%d FPS): RawRTT=%d ms | ActiveDelay=%d f | ConfirmedFrame=%d",
                   GetEngineBattleFps(), g_Telemetry.lastRawRttMs.load(), pSync->m_netDelayFrames,
                   pSync->m_pInputBuffer ? pSync->m_pInputBuffer->m_confirmedAllPlayersFrame : -1);
    } else {
        LogNetcode("[StartFrameSync] Match initialized (%d FPS): RawRTT=%d ms | VanillaDelay=%d f | ActiveDelay=%d f (%0.1f ms, AutoBoost=+%d f) | SendCounter=%d",
                   GetEngineBattleFps(), g_Telemetry.lastRawRttMs.load(), g_Telemetry.lastVanillaDelayFrames.load(),
                   pSync->m_netDelayFrames, pSync->m_netDelayFrames * GetEngineTrueFramePeriodMs(),
                   g_Telemetry.dynamicJitterBoostFrames.load(), pSync->m_sendFrameCounter);
    }
}

void __fastcall hk_UpdateMemberCountAndDelay(ccNetFreeBattleSync* pSync, int32_t memberCount) {
    if (!pSync) {
        oUpdateMemberDelay(pSync, memberCount);
        return;
    }
    const int32_t savedRtt = pSync->m_worstRttMs;
    int32_t targetDelay    = std::clamp(ResolveTargetDelayForSync(pSync, savedRtt), 1, 16);
    pSync->m_worstRttMs    = GetSyntheticRttForFrames(std::clamp(std::max(targetDelay, 8), 2, 16));
    oUpdateMemberDelay(pSync, memberCount);
    pSync->m_worstRttMs     = savedRtt;
    pSync->m_netDelayFrames = targetDelay;
    g_Telemetry.activeDelayFrames.store(targetDelay);
    PreWarmBattleUdpChannel(pSync);
}

} // namespace StormInternal
