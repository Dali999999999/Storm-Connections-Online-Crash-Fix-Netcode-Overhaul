#include "netcode_internal.h"
#include <algorithm>
#include <cstring>

namespace StormInternal {

namespace {

// Cold-start RTT spike filter: rejects transient Phase-1 Steam P2P ICE spikes
// (e.g. 135 ms initial probe settling to 42 ms in Phase 2) without underestimating
// genuinely high or rising latency routes.
struct RttSampleWindow {
    static constexpr int32_t kCap = 8;
    int32_t  samples[kCap]  = {};
    int32_t  count          = 0;
    int32_t  peakMs         = 0;
    int32_t  lastRawMs      = 0;
    uint64_t lastPushTickMs = 0;

    void Reset() noexcept {
        count          = 0;
        peakMs         = 0;
        lastRawMs      = 0;
        lastPushTickMs = 0;
    }

    void Push(int32_t rttMs, uint64_t nowTickMs) noexcept {
        if (rttMs <= 0 || rttMs > 2000) return;
        // Avoid double-counting the exact same cached RTT value echoed within 1.0s of the same sync phase
        if (count > 0 && rttMs == lastRawMs && nowTickMs >= lastPushTickMs && (nowTickMs - lastPushTickMs) < 1000ULL) {
            return;
        }
        lastRawMs      = rttMs;
        lastPushTickMs = nowTickMs;
        if (rttMs > peakMs) peakMs = rttMs;

        if (count < kCap) {
            samples[count++] = rttMs;
        } else {
            for (int32_t i = 1; i < kCap; ++i) {
                samples[i - 1] = samples[i];
            }
            samples[kCap - 1] = rttMs;
        }
    }

    int32_t FilteredMs() const noexcept {
        if (count <= 0) return 0;
        if (count == 1) return samples[0];
        if (count == 2) {
            const int32_t s0 = samples[0];
            const int32_t s1 = samples[1];
            // Transient cold-start spike: first sample >= +20ms AND >= 1.4x higher than fresh settled sample
            if (s0 >= s1 + 20 && (s0 * 10) >= (s1 * 14)) {
                return s1;
            }
            // Respect genuine upward latency climb; smooth minor downward variance
            return (s1 > s0) ? s1 : ((s0 + s1 + 1) / 2);
        }

        const int32_t winLen = std::min<int32_t>(count, 5);
        int32_t sorted[5]    = {};
        for (int32_t i = 0; i < winLen; ++i) {
            sorted[i] = samples[count - winLen + i];
        }
        std::sort(sorted, sorted + winLen);

        const int32_t median = (winLen % 2 == 1)
            ? sorted[winLen / 2]
            : ((sorted[(winLen / 2) - 1] + sorted[winLen / 2] + 1) / 2);
        const int32_t latest = samples[count - 1];
        return (latest > median) ? ((median + latest + 1) / 2) : median;
    }
};

RttSampleWindow s_HostLocalRttWin    = {};
RttSampleWindow s_HostBattler0RttWin = {};
RttSampleWindow s_HostBattler1RttWin = {};
RttSampleWindow s_ClientRttWin       = {};
uint64_t        s_RttWindowPeerId    = 0;
bool            s_LoggedRttSpikeDrop = false;
int32_t         s_LastLoggedHsKind   = -1; // 0=unmodded, 1=modded
int32_t         s_LastLoggedHsRawRtt = -1;
int32_t         s_LastLoggedHsDelay  = -1;
bool            s_LoggedOutdated30FpsHost = false;

void ResetPerMatchRttWindows() noexcept {
    s_HostLocalRttWin.Reset();
    s_HostBattler0RttWin.Reset();
    s_HostBattler1RttWin.Reset();
    s_ClientRttWin.Reset();
    s_RttWindowPeerId         = 0;
    s_LoggedRttSpikeDrop      = false;
    s_LastLoggedHsKind        = -1;
    s_LastLoggedHsRawRtt      = -1;
    s_LastLoggedHsDelay       = -1;
    s_LoggedOutdated30FpsHost = false;
}

int32_t ResolveHostFilteredRawRttMs(bool hostIsBattler) noexcept {
    const int32_t localF = s_HostLocalRttWin.FilteredMs();
    const int32_t b0F    = s_HostBattler0RttWin.FilteredMs();
    const int32_t b1F    = s_HostBattler1RttWin.FilteredMs();

    int32_t remoteF = std::max(b0F, b1F);
    // If Host is an active Battler with >= 2 settled direct readings to Opponent, ignore an
    // unconfirmed single-sample cold-start spike reported by an unmodded Opponent on Phase 1.
    if (hostIsBattler && localF > 0 && s_HostLocalRttWin.count >= 2 && remoteF > 0) {
        const int32_t remoteSamples = (b0F >= b1F) ? s_HostBattler0RttWin.count : s_HostBattler1RttWin.count;
        if (remoteSamples == 1 && remoteF >= localF + 25 && (remoteF * 10) >= (localF * 15)) {
            remoteF = localF;
        }
    }

    const int32_t resolved = std::max(localF, remoteF);
    const int32_t peakAll  = std::max({ s_HostLocalRttWin.peakMs, s_HostBattler0RttWin.peakMs, s_HostBattler1RttWin.peakMs });
    if (!s_LoggedRttSpikeDrop && resolved > 0 && peakAll >= resolved + 20) {
        s_LoggedRttSpikeDrop = true;
        LogNetcode("[RTT-Filter] Filtered cold-start RTT spike (Host): Peak=%d ms -> Settled=%d ms (local=%d/%d, b0=%d/%d, b1=%d/%d)",
                   peakAll, resolved,
                   localF, s_HostLocalRttWin.count,
                   b0F, s_HostBattler0RttWin.count,
                   b1F, s_HostBattler1RttWin.count);
    }
    return resolved;
}

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

const int8_t* GetLobbySlotArray() noexcept {
    if (!g_fnLocalInfoGetSlotArray) return nullptr;
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return nullptr;
    const void* pLocalInfo = *reinterpret_cast<const void* const*>(pNetMgr + 0x100);
    return pLocalInfo ? g_fnLocalInfoGetSlotArray(pLocalInfo) : nullptr;
}

// Prefer the authoritative lobby BattleTable slot array (pSlots[0]=1P, pSlots[1]=2P) over
// [pLocalInfo + 0x1C], because during Endless Lobby role rotations Phase 1 of SyncBase can run
// before [pLocalInfo + 0x1C] has refreshed from the previous match's battler/spectator role.
int32_t GetLocalBattlerIndex() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return -1;
    const uint8_t* pLocalInfo = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x100);
    if (!pLocalInfo) return -1;

    const int32_t localId = *reinterpret_cast<const int32_t*>(pLocalInfo + 0x24);
    const int8_t* pSlots  = GetLobbySlotArray();
    if (pSlots && localId >= 0) {
        const int32_t b0Id = static_cast<int32_t>(pSlots[0]);
        const int32_t b1Id = static_cast<int32_t>(pSlots[1]);
        if (b0Id == localId) return 0;
        if (b1Id == localId) return 1;
        if (b0Id >= 0 && b1Id >= 0) return -1; // Both battler slots assigned to other peers -> Spectator
    }
    return *reinterpret_cast<const int32_t*>(pLocalInfo + 0x1C);
}

int32_t GetSessionHostMemberId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    if (!pSession) return 0;
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
        if (IsValidSteamId(steamId) && *reinterpret_cast<const int32_t*>(entry + 0x24) != 0) {
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

    if (pSlots) {
        if (myBattlerIdx == 0 || myBattlerIdx == 1) {
            const int32_t oppMemberId = static_cast<int32_t>(pSlots[1 - myBattlerIdx]);
            if (oppMemberId >= 0) {
                for (int32_t i = 0; i < 8; ++i) {
                    const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                    const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                    if (IsValidSteamId(steamId) && *reinterpret_cast<const int32_t*>(entry + 0x40) == oppMemberId) {
                        return steamId;
                    }
                }
            }
        } else {
            const int32_t b0Id = static_cast<int32_t>(pSlots[0]);
            const int32_t b1Id = static_cast<int32_t>(pSlots[1]);
            uint64_t pairHash  = 0;
            for (int32_t i = 0; i < 8; ++i) {
                const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
                const uint64_t steamId  = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                if (IsValidSteamId(steamId) && (memberId == b0Id || memberId == b1Id)) {
                    pairHash ^= (steamId + 0x9e3779b97f4a7c15ULL);
                }
            }
            if (pairHash != 0) return pairHash;
        }
    }
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
        if (IsValidSteamId(steamId) && *reinterpret_cast<const int32_t*>(entry + 0x40) != localId) {
            return steamId;
        }
    }
    return 0;
}

// Guards against cross-match / cross-opponent RTT sample contamination when pre-Initialize
// lobby sync packets (subCmd=0x10) fire after a previous match or after an Endless Lobby rotation.
void EnsureRttWindowsFreshForCurrentPeer() noexcept {
    const uint64_t curPeerId = GetRemoteSessionPeerSteamId();
    if (curPeerId == 0) {
        return;
    }
    if (s_RttWindowPeerId != 0 && curPeerId != s_RttWindowPeerId) {
        s_HostLocalRttWin.Reset();
        s_HostBattler0RttWin.Reset();
        s_HostBattler1RttWin.Reset();
        s_ClientRttWin.Reset();
        s_LoggedRttSpikeDrop = false;
    }
    s_RttWindowPeerId = curPeerId;
}

int32_t GetDirectOpponentBattlerRttMs() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    const int8_t*  pSlots   = GetLobbySlotArray();
    if (!pSession || !pSlots) return 0;

    const int32_t myBattlerIdx = GetLocalBattlerIndex();
    if (myBattlerIdx == 0 || myBattlerIdx == 1) {
        const int32_t oppMemberId = static_cast<int32_t>(pSlots[1 - myBattlerIdx]);
        if (oppMemberId >= 0) {
            for (int32_t i = 0; i < 8; ++i) {
                const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                if (IsValidSteamId(*reinterpret_cast<const uint64_t*>(entry + 0x38)) &&
                    *reinterpret_cast<const int32_t*>(entry + 0x40) == oppMemberId) {
                    return *reinterpret_cast<const int32_t*>(entry + 0x2C);
                }
            }
        }
        return 0;
    }
    const int32_t b0Id = static_cast<int32_t>(pSlots[0]);
    const int32_t b1Id = static_cast<int32_t>(pSlots[1]);
    int32_t maxBattlerRtt = 0;
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
        if (IsValidSteamId(*reinterpret_cast<const uint64_t*>(entry + 0x38)) &&
            (memberId == b0Id || memberId == b1Id)) {
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
    const int32_t safeRawRtt = std::max(0, rawRtt);
    // Refresh Host's own battler-slot mod flag from the live BattleTable in case Phase 1 ran
    // before local role finished updating (prevents false BattlersModded=1/2 on Spectator->Battler).
    const int32_t localBattlerIdx = GetLocalBattlerIndex();
    if (localBattlerIdx == 0) {
        g_Battler0HasMod.store(true);
    } else if (localBattlerIdx == 1) {
        g_Battler1HasMod.store(true);
    }

    const bool    bothBattlersModded = g_Battler0HasMod.load() && g_Battler1HasMod.load();
    const bool    anyBattlerModded   = g_Battler0HasMod.load() || g_Battler1HasMod.load();
    const bool    is30Fps            = (GetEngineBattleFps() == 30);
    g_Telemetry.peerHasModHandshake.store(bothBattlersModded);

    const int32_t oneFrameMaxRtt = is30Fps ? 40 : 26;
    const int32_t lobbyFloor     = (safeRawRtt > oneFrameMaxRtt) ? 2 : 1;
    int32_t minFloor = 3;
    if (bothBattlersModded) {
        minFloor = std::max(lobbyFloor, g_Config.min_delay_frames);
    } else if (anyBattlerModded) {
        const int32_t mixedFloor = (is30Fps || safeRawRtt <= 40) ? 2 : 3;
        minFloor = std::max(mixedFloor, g_Config.min_delay_frames);
    } else {
        minFloor = std::max(is30Fps ? 2 : 3, g_Config.min_delay_frames);
    }
    int32_t targetDelay = ComputeHostAuthoritativeTargetDelay(safeRawRtt, minFloor, g_Config.max_delay_clamp, bothBattlersModded);
    // Merge remote peer's floor AFTER Host AutoBoost so peer boost + host boost are never double-counted
    if (anyBattlerModded && g_Config.forced_delay_frames < 1) {
        const int32_t peerFloor = g_PeerMinDelayFloor.load();
        if (peerFloor > 0) {
            const int32_t cfgMax    = std::clamp(g_Config.max_delay_clamp, minFloor, 16);
            const int32_t upperCap  = bothBattlersModded
                ? cfgMax
                : std::min(cfgMax, ComputeVanillaDelayFrames(safeRawRtt, 0));
            targetDelay = std::clamp(std::max(targetDelay, peerFloor), minFloor, std::max(targetDelay, upperCap));
        }
    }
    outTargetDelay  = targetDelay;
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
    if (*reinterpret_cast<const uint8_t*>(pFramework + 0x952) == 30) {
        return 30;
    }

    // In mixed 30 FPS / 60 FPS lobbies, NSUNSC.exe's ccSceneBattleModeRootBase::Initialize (+0x0876846)
    // checks pNetManager's member FPS map (+0x183128, populated by +0x08405E0) and forces SetBattleFps(30)
    // if ANY active member in the session has is60Fps == 0. Because Pre-Match RTT negotiation
    // (ccSceneNetworkBattleSyncBase) runs BEFORE ccSceneBattleModeRootBase::Initialize downgrades
    // [pFramework + 0x952] to 30, inspect [pNetManager + 0x183128] directly so RTT negotiation and
    // SetupFrameSync always agree on 30 FPS from Phase 1.
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (pNetMgr) {
        const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
        const uint8_t* pHead    = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x183128);
        if (pSession && pHead) {
            const uint8_t* node = *reinterpret_cast<const uint8_t* const*>(pHead);
            for (int32_t step = 0; step < 32 && node && node != pHead; ++step) {
                const uint8_t nodeUId = *reinterpret_cast<const uint8_t*>(node + 0x10);
                const int32_t is60Fps = *reinterpret_cast<const int32_t*>(node + 0x14);
                if (is60Fps == 0 && nodeUId >= 10) {
                    for (int32_t i = 0; i < 8; ++i) {
                        const uint8_t* entry   = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                        const uint64_t steamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                        const int32_t  slotUId = *reinterpret_cast<const int32_t*>(entry + 0x40);
                        if (IsValidSteamId(steamId) && slotUId == static_cast<int32_t>(nodeUId)) {
                            return 30;
                        }
                    }
                }
                node = *reinterpret_cast<const uint8_t* const*>(node);
            }
        }
    }
    return 60;
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

    // 1 frame is safely reachable up to ~26 ms RTT at 60 FPS (16.67 ms) or ~40 ms RTT at 30 FPS (33.33 ms)
    const int32_t oneFrameMaxRtt = is30Fps ? 40 : 26;
    const int32_t phaseSafeMin   = (clampedRtt > oneFrameMaxRtt) ? std::max(2, safeMin) : safeMin;
    const int32_t baseMarginMs = std::clamp(g_Config.jitter_margin_ms, 0, 10);
    // In 30 FPS mode, each simulation step is 33.33 ms (2x wider than 60 FPS), so include +9ms phase margin.
    // At 60 FPS, include +3ms route-safety guard (when JitterMargin > 0) + RTT/13 jitter scaling (~48ms->2f, 108ms->5f).
    const int32_t cushionMs = is30Fps
        ? (baseMarginMs + 9 + (clampedRtt / 20))
        : (baseMarginMs + (baseMarginMs > 0 ? 3 : 0) + (clampedRtt / 13));
    const int32_t budgetMs     = owdMs + cushionMs;
    const int32_t targetFrames = is30Fps
        ? std::max(1, (budgetMs * 3 + 99) / 100)
        : std::max(1, (budgetMs * 3 + 49) / 50);

    return std::clamp(targetFrames, phaseSafeMin, std::max(safeMax, phaseSafeMin));
}

int32_t ComputeHostAuthoritativeTargetDelay(int32_t rttMs, int32_t minFloor, int32_t maxClamp, bool peerModded) {
    const bool    is30Fps      = (GetEngineBattleFps() == 30);
    const int32_t clampedRtt   = std::max(0, rttMs);
    const int32_t maxBoost     = is30Fps ? 1 : 2; // At 30 FPS, +1 frame is already +33.3ms (2x 60FPS frames)
    const int32_t baseMarginMs = std::clamp(g_Config.jitter_margin_ms, 0, 10);

    if (!peerModded && g_Config.forced_delay_frames < 1) {
        const int32_t vanillaDelay     = ComputeVanillaDelayFrames(clampedRtt, 0);
        const int32_t owdMs            = (clampedRtt + 1) / 2;
        const bool    anyBattlerModded = g_Battler0HasMod.load() || g_Battler1HasMod.load();

        if (is30Fps) {
            const int32_t cushion30 = anyBattlerModded
                ? (baseMarginMs + 13 + (clampedRtt / 16))
                : (baseMarginMs + 22 + (clampedRtt / 12));
            int32_t safeDelay30 = std::clamp(((owdMs + cushion30) * 3 + 99) / 100, 2, std::max(2, vanillaDelay - 1));
            if (g_Config.enable_dynamic_peer_autotune && anyBattlerModded) {
                const int32_t boost = std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, maxBoost);
                safeDelay30 = std::clamp(safeDelay30 + boost, 2, vanillaDelay);
            }
            return std::clamp(safeDelay30, std::max(2, minFloor), std::max(2, maxClamp));
        }

        // Strictly ordered cushion60: BothModded (base+3+RTT/13) <= Mixed 1/2 (base+8+RTT/13) <= Vanilla 0/2 (base+12+RTT/12)
        // Ensures an unmodded peer (lacking PacketRedundancy/WifiAntiLag/PrecisionPacer) NEVER gets less margin than a modded peer!
        const int32_t cushion60 = anyBattlerModded
            ? (baseMarginMs + 8 + (clampedRtt / 13))
            : (baseMarginMs + 12 + (clampedRtt / 12));
        const int32_t minSafe60 = (anyBattlerModded && clampedRtt <= 40) ? 2 : 3;
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
    const int32_t  prevMatchPeakFrame    = s_MatchPeakSyncFrame;
    const uint64_t prevMatchStallCount   = s_MatchStallsAtPeakFrame;
    const uint64_t prevMatchNetStalls    = s_MatchNetStallsAtPeakFrame;
    const uint64_t prevMatchSlipStalls   = s_MatchSlipStallsAtPeakFrame;
    const bool     prevOpeningCaptured   = s_OpeningSettleCaptured;
    const uint64_t prevOpeningStallCount = s_OpeningSettleStalls;

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
            // Evaluate ONLY genuine network/jitter stalls (multi-tick or burst stalls), NOT isolated
            // 1-tick peer clock-slip stalls (e.g. peer at 58.9Hz vs our 60.00Hz), because adding
            // input delay cannot fix a slower peer clock and only inflates input lag (+16.7..+33.3ms).
            const bool     useSteadySpan = prevOpeningCaptured && prevMatchPeakFrame > 45 && prevMatchStallCount >= prevOpeningStallCount;
            const int32_t  evalFrames    = useSteadySpan ? (prevMatchPeakFrame - 45) : prevMatchPeakFrame;
            const uint64_t evalNetStalls = prevMatchNetStalls;
            const double   netStallRatio = static_cast<double>(evalNetStalls) / static_cast<double>(evalFrames);
            const double   slipRatio     = static_cast<double>(prevMatchSlipStalls) / static_cast<double>(evalFrames);
            const bool     is30Fps       = (GetEngineBattleFps() == 30);
            const int32_t  maxBoost      = is30Fps ? 1 : 2;
            const int32_t  decayMinFrms  = is30Fps ? 900 : 1800;

            if (netStallRatio >= 0.018) {
                const int32_t stepBoost = (!is30Fps && netStallRatio >= 0.120) ? 2 : 1;
                const int32_t prevBoost = g_PeerDynamicJitterBoostFrames.load();
                const int32_t newBoost  = std::clamp(prevBoost + stepBoost, 1, maxBoost);
                if (newBoost != prevBoost) {
                    g_PeerDynamicJitterBoostFrames.store(newBoost);
                    g_Telemetry.dynamicJitterBoostFrames.store(newBoost);
                    LogNetcode("[AutoTune] Rematch vs same peer had %0.2f%% network stalls (%llu/%d f, ClockSlip=%0.2f%%) -> +%d frame cushion!",
                               netStallRatio * 100.0,
                               static_cast<unsigned long long>(evalNetStalls),
                               evalFrames,
                               slipRatio * 100.0,
                               newBoost);
                }
            } else if (netStallRatio < 0.005 && prevMatchPeakFrame >= decayMinFrms) {
                const int32_t prevBoost = g_PeerDynamicJitterBoostFrames.load();
                if (prevBoost > 0) {
                    const int32_t newBoost = prevBoost - 1;
                    g_PeerDynamicJitterBoostFrames.store(newBoost);
                    g_Telemetry.dynamicJitterBoostFrames.store(newBoost);
                    LogNetcode("[AutoTune] Route verified rock-solid (NetStalls=%0.2f%%, ClockSlip=%0.2f%% over %d f) -> Cushion relaxed to +%d f!",
                               netStallRatio * 100.0, slipRatio * 100.0, evalFrames, newBoost);
                } else if (prevMatchPeakFrame >= 3600) {
                    LogNetcode("[AutoTune] Route verified rock-solid (NetStalls=%0.2f%%, ClockSlip=%0.2f%% over %d f). Optimal pacing retained.",
                               netStallRatio * 100.0, slipRatio * 100.0, evalFrames);
                }
            } else if (prevMatchPeakFrame >= 3600) {
                LogNetcode("[AutoTune] Moderate mid-fight jitter (NetStalls=%0.2f%%, ClockSlip=%0.2f%% over %d f) — below +1f threshold (1.80%%), keeping +%d f cushion.",
                           netStallRatio * 100.0, slipRatio * 100.0, evalFrames,
                           g_PeerDynamicJitterBoostFrames.load());
            }
        }

        g_NegotiatedTargetDelay.store(-1);
        g_PeerMinDelayFloor.store(0);
        g_CapturedHostRawRttMs.store(0);
        g_ClientTrueRawRttMs.store(0);
        g_LastSentSyntheticRttMs.store(0);
        g_AuthoritativeHostRttMs.store(0);
        g_HostHandshakeObserved.store(false);
        g_Battler0HasMod.store(myBattlerIdx == 0);
        g_Battler1HasMod.store(myBattlerIdx == 1);
        g_Telemetry.peerHasModHandshake.store(false);
        ResetPerMatchRttWindows();
        s_RttWindowPeerId = remoteSteamId;
        ApplyWifiGamingLowLatencyMode(false);
    } else {
        EnsureRttWindowsFreshForCurrentPeer();
    }

    g_Telemetry.localIsHost.store(isHost);
    g_LastSyncInitTickMs.store(nowTick);
    if (remoteSteamId != 0) {
        g_LastNegotiatedPeerSteamId.store(remoteSteamId);
    }
    g_NegotiationPendingForMatch.store(true);

    // Only seed RTT on Initialize when local player is an active Battler (0 or 1).
    // Prefer live ccNetSessionData::m_slots[opp]+0x2C (continuously updated by Steamworks)
    // over stale [pNetMgr+0x50] so Phase 2 sees the settled post-ICE RTT instead of a Phase-1 spike.
    if (myBattlerIdx >= 0) {
        const int32_t liveOppRtt = GetDirectOpponentBattlerRttMs();
        int32_t* const pWorstRtt = GetGlobalNetManagerWorstRttPtr();
        int32_t initRawRtt       = (liveOppRtt > 0) ? liveOppRtt : (pWorstRtt ? *pWorstRtt : 0);
        const int32_t lastSynth  = g_LastSentSyntheticRttMs.load();
        const int32_t authSynth  = g_AuthoritativeHostRttMs.load();
        if (initRawRtt > 0 && initRawRtt != lastSynth && initRawRtt != authSynth) {
            if (isHost) {
                s_HostLocalRttWin.Push(initRawRtt, nowTick);
                const int32_t filtered = ResolveHostFilteredRawRttMs(true);
                if (filtered > 0) {
                    g_CapturedHostRawRttMs.store(filtered);
                    g_Telemetry.lastRawRttMs.store(filtered);
                }
            } else {
                s_ClientRttWin.Push(initRawRtt, nowTick);
                const int32_t filtered = s_ClientRttWin.FilteredMs();
                if (filtered > 0) {
                    g_ClientTrueRawRttMs.store(filtered);
                    if (!isPhase2SameMatch || !g_HostHandshakeObserved.load() || g_Telemetry.lastRawRttMs.load() <= 0) {
                        g_Telemetry.lastRawRttMs.store(filtered);
                    }
                }
            }
        }
    }

    // Proactively open Channel 6 P2P connections with all session peers (Host, Clients & Spectators)
    // seconds before StartFrameSync so Client<->Client ICE traversal finishes before Frame 0.
    PreWarmSessionPeersOnChannel6(true);
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
            const uint64_t nowTick  = GetTickCount64();
            EnsureRttWindowsFreshForCurrentPeer();
            if (!isSenderSpectator && clientRtt > 0 && clientRtt != lastSynth && (isSenderBattler || g_CapturedHostRawRttMs.load() <= 0)) {
                if (isSenderB0) {
                    s_HostBattler0RttWin.Push(clientRtt, nowTick);
                } else {
                    s_HostBattler1RttWin.Push(clientRtt, nowTick);
                }
                if (myBattlerIdx >= 0) {
                    const int32_t liveOpp = GetDirectOpponentBattlerRttMs();
                    if (liveOpp > 0 && liveOpp != lastSynth) {
                        s_HostLocalRttWin.Push(liveOpp, nowTick);
                    }
                }
                const int32_t filtered = ResolveHostFilteredRawRttMs(myBattlerIdx >= 0);
                if (filtered > 0) {
                    g_CapturedHostRawRttMs.store(filtered);
                    g_Telemetry.lastRawRttMs.store(filtered);
                }
            }

            if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeMagic1) {
                const int32_t peerFloor = std::clamp<int32_t>(pPacket[7], 1, 16);
                if (isSenderB0) g_Battler0HasMod.store(true);
                if (isSenderB1) g_Battler1HasMod.store(true);
                if (isSenderBattler) {
                    const int32_t prevFloor = g_PeerMinDelayFloor.load();
                    g_PeerMinDelayFloor.store(std::max(prevFloor, peerFloor));
                    LogNetcode("[Handshake] Remote battler (member=%d) has StormNetcode mod (min_floor=%d, clientRTT=%d ms)",
                               senderId, peerFloor, clientRtt);
                } else {
                    LogNetcode("[Handshake] Remote spectator (member=%d, Phase-1 stale NC) has StormNetcode mod (ignored clientRTT=%d ms)",
                               senderId, clientRtt);
                }
            } else if (isWatcherHandshake) {
                // Even if an older-version remote peer sends Phase-1 'NS' before its local battler index
                // refreshes on an Endless Lobby rotation, 'NS' still proves StormNetcode is installed.
                // Mark the assigned battler slot modded without reading its dummy floor (2u) or RTT (0ms).
                if (isSenderB0) g_Battler0HasMod.store(true);
                if (isSenderB1) g_Battler1HasMod.store(true);
                if (isSenderB0 || isSenderB1) {
                    LogNetcode("[Handshake] Remote battler (member=%d, Phase-1 stale NS) has StormNetcode mod", senderId);
                } else {
                    LogNetcode("[Handshake] Remote spectator (member=%d) has StormNetcode mod", senderId);
                }
            }

            g_Telemetry.peerHasModHandshake.store(g_Battler0HasMod.load() && g_Battler1HasMod.load());
            if (isSenderSpectator || (g_Config.enable_custom_delay && g_Config.delay_mode >= 1)) {
                *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
            }
        } else if (!isHost && (localId < 0 || senderId != localId)) {
            // Client receiving re-broadcasted chan=4 subCmd=0x10:
            //   1) 'NM' roster bitmask stamped by a Modded Host (100% v2.8 & vanilla compatible), OR
            //   2) 'NC'/'NS' client handshake forwarded verbatim by an Unmodded Spectator Host!
            const int32_t myBattlerIdx = GetLocalBattlerIndex();
            if (myBattlerIdx == 0) g_Battler0HasMod.store(true);
            if (myBattlerIdx == 1) g_Battler1HasMod.store(true);

            if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeRosterMagic1) {
                if ((pPacket[7] & 0x01u) != 0u) g_Battler0HasMod.store(true);
                if ((pPacket[7] & 0x02u) != 0u) g_Battler1HasMod.store(true);
            } else if (pPacket[5] == kHandshakeMagic0 &&
                       (pPacket[6] == kHandshakeMagic1 || pPacket[6] == kHandshakeWatcherMagic1)) {
                const int8_t* pSlots = GetLobbySlotArray();
                const int32_t b0Id   = pSlots ? static_cast<int32_t>(pSlots[0]) : -1;
                const int32_t b1Id   = pSlots ? static_cast<int32_t>(pSlots[1]) : -1;
                if ((b0Id >= 0 && senderId == b0Id) || (b0Id < 0 && myBattlerIdx == 1)) {
                    g_Battler0HasMod.store(true);
                }
                if ((b1Id >= 0 && senderId == b1Id) || (b1Id < 0 && myBattlerIdx == 0)) {
                    g_Battler1HasMod.store(true);
                }
            }
            const bool bothModded = g_Battler0HasMod.load() && g_Battler1HasMod.load();
            g_Telemetry.peerHasModHandshake.store((myBattlerIdx >= 0) ? bothModded : (g_HostHandshakeObserved.load() || bothModded));
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

            const int32_t myBattlerIdx = GetLocalBattlerIndex();
            if (myBattlerIdx == 0) g_Battler0HasMod.store(true);
            if (myBattlerIdx == 1) g_Battler1HasMod.store(true);

            if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeMagic1) {
                g_HostHandshakeObserved.store(true);
                // Mark Host's battler slot (if Host is an active Battler, not Spectator) as modded so
                // 1v1 Clients also unlock shieldDepth=4 while keeping accurate peerHasModHandshake in 3+ lobbies!
                const int32_t hostId = *reinterpret_cast<const int32_t*>(pPacket + 0x08);
                const int8_t* pSlots = GetLobbySlotArray();
                const int32_t b0Id   = pSlots ? static_cast<int32_t>(pSlots[0]) : -1;
                const int32_t b1Id   = pSlots ? static_cast<int32_t>(pSlots[1]) : -1;
                if ((b0Id >= 0 && hostId == b0Id) || (b0Id < 0 && b1Id < 0 && myBattlerIdx == 1)) {
                    g_Battler0HasMod.store(true);
                }
                if ((b1Id >= 0 && hostId == b1Id) || (b0Id < 0 && b1Id < 0 && myBattlerIdx == 0)) {
                    g_Battler1HasMod.store(true);
                }
                const bool bothModded = g_Battler0HasMod.load() && g_Battler1HasMod.load();
                g_Telemetry.peerHasModHandshake.store((myBattlerIdx >= 0) ? bothModded : true);

                const int32_t hostTargetDelay = std::clamp<int32_t>(pPacket[7], 1, 16);
                const int32_t rawHostRtt      = *reinterpret_cast<const int32_t*>(pPacket + 0x14);
                if (rawHostRtt > 0 && (rawHostRtt != broadcastRtt || g_Telemetry.lastRawRttMs.load() <= 0)) {
                    g_Telemetry.lastRawRttMs.store(rawHostRtt);
                }
                g_NegotiatedTargetDelay.store(hostTargetDelay);

                const int32_t displayRawRtt = g_Telemetry.lastRawRttMs.load();
                if (s_LastLoggedHsKind != 1 || s_LastLoggedHsRawRtt != displayRawRtt || s_LastLoggedHsDelay != hostTargetDelay) {
                    s_LastLoggedHsKind   = 1;
                    s_LastLoggedHsRawRtt = displayRawRtt;
                    s_LastLoggedHsDelay  = hostTargetDelay;
                    LogNetcode("[Handshake] Host has StormNetcode mod! Raw RTT=%d ms -> Agreed Delay=%d frames",
                               displayRawRtt, hostTargetDelay);
                }

                // Detect outdated-mod Host sending a 60 FPS SyntheticRTT into a 30 FPS match:
                // unmodded 30 FPS peers will compute a different delay from broadcastRtt, causing instant lockstep desync!
                if (!s_LoggedOutdated30FpsHost && GetEngineBattleFps() == 30 && hostTargetDelay >= 2 && hostTargetDelay <= 16) {
                    const int32_t vanilla30FromSynth = ComputeVanillaDelayFrames(broadcastRtt, 0);
                    if (vanilla30FromSynth != hostTargetDelay && broadcastRtt == kSyntheticRttForFrames60[hostTargetDelay]) {
                        s_LoggedOutdated30FpsHost = true;
                        LogNetcode("[WARN] Outdated Host mod sent 60FPS SyntheticRTT=%d ms (Delay=%d f) in a 30FPS match (unmodded 30FPS peers compute %d f -> desync!). Host should update to v2.9!",
                                   broadcastRtt, hostTargetDelay, vanilla30FromSynth);
                    }
                }
            } else {
                g_HostHandshakeObserved.store(false);
                g_Telemetry.peerHasModHandshake.store(g_Battler0HasMod.load() && g_Battler1HasMod.load());
                g_Telemetry.lastRawRttMs.store(broadcastRtt);
                const int32_t vanillaDelay = ComputeVanillaDelayFrames(broadcastRtt, 0);
                g_NegotiatedTargetDelay.store(vanillaDelay);
                if (s_LastLoggedHsKind != 0 || s_LastLoggedHsRawRtt != broadcastRtt || s_LastLoggedHsDelay != vanillaDelay) {
                    s_LastLoggedHsKind   = 0;
                    s_LastLoggedHsRawRtt = broadcastRtt;
                    s_LastLoggedHsDelay  = vanillaDelay;
                    LogNetcode("[Handshake] Host is unmodded (Broadcast RTT=%d ms -> Lockstep-Safe Delay=%d frames)",
                               broadcastRtt, vanillaDelay);
                }
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
            PreWarmSessionPeersOnChannel6(false);
            const uint64_t nowTick = GetTickCount64();
            if (!isHost) {
                int32_t myRawRtt        = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                const int32_t authSynth = g_AuthoritativeHostRttMs.load();
                const int32_t myBattlerIdx = GetLocalBattlerIndex();
                if (myBattlerIdx >= 0) {
                    if (!g_NegotiationPendingForMatch.load()) {
                        ResetPerMatchRttWindows();
                    }
                    EnsureRttWindowsFreshForCurrentPeer();
                    const int32_t liveOppRtt = GetDirectOpponentBattlerRttMs();
                    if (liveOppRtt > 0 && liveOppRtt != authSynth) {
                        s_ClientRttWin.Push(liveOppRtt, nowTick);
                    } else if (myRawRtt > 0 && myRawRtt != authSynth) {
                        s_ClientRttWin.Push(myRawRtt, nowTick);
                    }
                    const int32_t filteredClientRtt = s_ClientRttWin.FilteredMs();
                    if (filteredClientRtt > 0) {
                        if (!s_LoggedRttSpikeDrop && s_ClientRttWin.peakMs >= filteredClientRtt + 20) {
                            s_LoggedRttSpikeDrop = true;
                            LogNetcode("[RTT-Filter] Filtered cold-start RTT spike (Client): Peak=%d ms -> Settled=%d ms (%d samples)",
                                       s_ClientRttWin.peakMs, filteredClientRtt, s_ClientRttWin.count);
                        }
                        myRawRtt = filteredClientRtt;
                        g_ClientTrueRawRttMs.store(filteredClientRtt);
                        *reinterpret_cast<int32_t*>(pPacket + 0x10) = filteredClientRtt;
                        if (!g_HostHandshakeObserved.load() || g_Telemetry.lastRawRttMs.load() <= 0) {
                            g_Telemetry.lastRawRttMs.store(filteredClientRtt);
                        }
                    } else if (g_ClientTrueRawRttMs.load() > 0) {
                        myRawRtt = g_ClientTrueRawRttMs.load();
                        *reinterpret_cast<int32_t*>(pPacket + 0x10) = myRawRtt;
                    }
                }

                if (myBattlerIdx == -1) {
                    *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
                    pPacket[5] = kHandshakeMagic0;
                    pPacket[6] = kHandshakeWatcherMagic1;
                    pPacket[7] = 2u;
                } else {
                    const int32_t maxBoost = (GetEngineBattleFps() == 30) ? 1 : 2;
                    int32_t desiredFloor   = std::clamp(g_Config.min_delay_frames, 1, 16);
                    if (g_Config.forced_delay_frames >= 1) {
                        desiredFloor = std::clamp(std::max(desiredFloor, g_Config.forced_delay_frames), 1, 16);
                    }
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
                g_Telemetry.localIsHost.store(true);

                const int32_t senderId      = *reinterpret_cast<const int32_t*>(pPacket + 0x08);
                const int32_t localHostId   = GetLocalMemberId();
                const bool    isHostOwnSend = (localHostId >= 0 && senderId == localHostId);
                const int32_t myBattlerIdx  = GetLocalBattlerIndex();
                const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                const int32_t lastSynth     = g_LastSentSyntheticRttMs.load();

                if (myBattlerIdx == 0) g_Battler0HasMod.store(true);
                if (myBattlerIdx == 1) g_Battler1HasMod.store(true);

                // Stamp 'NM' modded-battler roster bitmask into chan=4 subCmd=0x10 padding bytes
                // (ignored by vanilla & v2.6..v2.8 clients; informs v2.9+ clients of exact 2/2 modded state).
                pPacket[5] = kHandshakeMagic0;
                pPacket[6] = kHandshakeRosterMagic1;
                pPacket[7] = static_cast<uint8_t>((g_Battler0HasMod.load() ? 0x01u : 0u) |
                                                  (g_Battler1HasMod.load() ? 0x02u : 0u));

                // Never contaminate RTT windows with stale samples from a finished previous match
                // if a pre-Initialize subCmd=0x10 packet fires before hk_SyncBase_Initialize runs.
                if (!g_NegotiationPendingForMatch.load()) {
                    ResetPerMatchRttWindows();
                }
                EnsureRttWindowsFreshForCurrentPeer();

                if (myBattlerIdx >= 0) {
                    const int32_t liveOppRtt = GetDirectOpponentBattlerRttMs();
                    if (liveOppRtt > 0 && liveOppRtt != lastSynth) {
                        s_HostLocalRttWin.Push(liveOppRtt, nowTick);
                    } else if (isHostOwnSend && currentPktRtt > 0 && currentPktRtt != lastSynth) {
                        s_HostLocalRttWin.Push(currentPktRtt, nowTick);
                    }
                }
                int32_t rawRtt = ResolveHostFilteredRawRttMs(myBattlerIdx >= 0);
                if (rawRtt <= 0 && currentPktRtt > 0 && currentPktRtt != lastSynth) {
                    rawRtt = currentPktRtt;
                }
                if (rawRtt <= 0) {
                    rawRtt = GetDirectOpponentBattlerRttMs();
                }
                if (rawRtt > 0) {
                    g_CapturedHostRawRttMs.store(rawRtt);
                    g_Telemetry.lastRawRttMs.store(rawRtt);
                }

                if (!isHostOwnSend) {
                    if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
                        *reinterpret_cast<int32_t*>(pPacket + 0x10) = 0;
                    }
                } else if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1 && (rawRtt > 0 || g_Config.forced_delay_frames >= 1)) {
                    int32_t targetDelay = 2, syntheticRtt = 7;
                    ComputeHostNegotiationState(rawRtt, targetDelay, syntheticRtt);
                    pPacket[7] = static_cast<uint8_t>((g_Battler0HasMod.load() ? 0x01u : 0u) |
                                                      (g_Battler1HasMod.load() ? 0x02u : 0u));
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
            const uint64_t nowTick      = GetTickCount64();
            const int32_t myBattlerIdx  = GetLocalBattlerIndex();
            const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
            const int32_t lastSynth     = g_LastSentSyntheticRttMs.load();

            if (!g_NegotiationPendingForMatch.load()) {
                ResetPerMatchRttWindows();
            }
            EnsureRttWindowsFreshForCurrentPeer();

            if (myBattlerIdx >= 0) {
                const int32_t liveOppRtt = GetDirectOpponentBattlerRttMs();
                if (liveOppRtt > 0 && liveOppRtt != lastSynth) {
                    s_HostLocalRttWin.Push(liveOppRtt, nowTick);
                }
            }
            int32_t rawRtt = ResolveHostFilteredRawRttMs(myBattlerIdx >= 0);
            if (rawRtt <= 0 && currentPktRtt > 0 && currentPktRtt != lastSynth) {
                rawRtt = currentPktRtt;
            }
            if (rawRtt <= 0) {
                rawRtt = GetDirectOpponentBattlerRttMs();
            }
            if (rawRtt > 0) {
                g_CapturedHostRawRttMs.store(rawRtt);
                g_Telemetry.lastRawRttMs.store(rawRtt);
            }

            // Never broadcast a bogus 2-frame handshake before receiving at least one real RTT sample (e.g. Host Spectating)!
            if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1 && (rawRtt > 0 || g_Config.forced_delay_frames >= 1)) {
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
    ResetLobbyWaitDeadlockState();
    if (!pSync) {
        oSetupFrameSync(pSync);
        return;
    }

    const int32_t savedRtt = pSync->m_worstRttMs;
    int32_t targetDelay    = std::clamp(ResolveTargetDelayForSync(pSync, savedRtt), 1, 16);
    g_NegotiationPendingForMatch.store(false);
    g_LastSyncInitTickMs.store(0);
    // Clear pre-match RTT windows immediately once frame sync starts so samples from this
    // opponent can never leak into a future match or lobby rotation.
    ResetPerMatchRttWindows();

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
    if (pSync->m_bIsStarted != 0) {
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
