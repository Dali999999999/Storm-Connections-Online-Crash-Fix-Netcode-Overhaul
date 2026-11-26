#include "netcode_internal.h"
#include <algorithm>
#include <cstring>

// ============================================================================
// Module 3 & 3b:
//   - Vanilla-Safe Multi-Frame UDP Redundancy & Single-Record Auxiliary Resends
//     (`hk_BuildBattlerSendPacket` @ VA 0x1410F2310)
//   - Spectator Stream Redundancy & NACK Deque Deduplication
//     (`hk_SendSpectatorPacket` @ VA 0x1410F3A50)
// ============================================================================

namespace StormInternal {

namespace {

// Pop one additional resend frame request from ccNetFreeBattleSync's intrusive queue (+0x250),
// matching the exact linked-list manipulation at 0x1410F2410 .. 0x1410F244B.
bool PopNextResendRequest(ccNetFreeBattleSync* pSync, int32_t& outFrameNumber) {
    if (!pSync || !pSync->m_pResendQueueHead) {
        return false;
    }

    ccNetResendNode* node = pSync->m_pResendQueueHead;
    if (node == pSync->m_pResendQueueTail) {
        pSync->m_pResendQueueTail = nullptr;
        pSync->m_pResendQueueHead = nullptr;
    } else {
        pSync->m_pResendQueueHead = node->pNextQueue;
    }

    outFrameNumber = node->frameNumber;

    // Return node to free list (+0x248)
    node->pNextFree = pSync->m_pResendFreeList;
    pSync->m_pResendFreeList = node;
    return true;
}

// Validate that a local player's frame is present and checksummed in ccNetInputBuffer
bool IsLocalFrameReadyForSend(ccNetFreeBattleSync* pSync, int32_t frameNumber) {
    if (!pSync || !pSync->m_pInputBuffer || !g_fnInputBufGetOrAdvanceSlot) {
        return false;
    }
    int32_t minValidFrame = std::max<int32_t>(1, pSync->m_netDelayFrames);
    if (frameNumber < minValidFrame || frameNumber > 0xFFFF) {
        return false;
    }
    int32_t myIdx = pSync->m_myPlayerIdx;
    if (myIdx < 0 || myIdx >= 4) {
        return false;
    }

    ccNetFrameSlot* slot = g_fnInputBufGetOrAdvanceSlot(pSync->m_pInputBuffer, frameNumber);
    if (!slot || slot->frameNumber != frameNumber) {
        return false;
    }

    const ccNetSlotPlayerEntry& entry = slot->players[myIdx];
    return (entry.bReceived != 0);
}

// Exact instruction-by-instruction simulation of unmodded NSUNSC.exe's ReceivePackets parser
// (VA 0x1410F2810 .. 0x1410F2B7C), INCLUDING CyberConnect2's broken lookahead check at 0x1410F299B
// where `remain > 12` indexes `recvBuf + remain` instead of `recvBuf + (recvSize - remain)`.
// Any datagram for which this function returns `true` is 100% mathematically guaranteed to be
// accepted without `"Data Corrupted"` by BOTH unmodded and modded NSUNSC.exe peers!
bool IsDatagramSafeForVanillaPeer(const uint8_t* buf, uint32_t totalBytes) noexcept {
    if (!buf || totalBytes < 12 || totalBytes > 0x1000) {
        return false;
    }

    uint32_t pos    = 0;
    int32_t  remain = static_cast<int32_t>(totalBytes);

    while (remain >= 12) {
        uint32_t hdr0 = 0, hdr1 = 0, hdr2 = 0;
        std::memcpy(&hdr0, buf + pos + 0, 4);
        std::memcpy(&hdr1, buf + pos + 4, 4);
        std::memcpy(&hdr2, buf + pos + 8, 4);

        // 0x1410F285E..0x1410F2870: per-record 12-byte header checksum
        if ((hdr0 + hdr1) != hdr2) {
            return false;
        }

        pos    += 12;
        remain -= 12;

        // 0x1410F2890: NACK / spectator control record (bit 31 set)
        if (static_cast<int32_t>(hdr0) < 0) {
            if ((hdr0 & 0x08000000u) != 0) {
                if (remain < 4) return false;
                pos    += 4;
                remain -= 4;
            }
            continue;
        }

        // 0x1410F2965..0x1410F2989: FrameInput variable payload length
        int32_t payloadBytes = 0;
        if ((hdr0 & 0x40000000u) != 0) payloadBytes += 4; // buttons
        if ((hdr0 & 0x20000000u) != 0) payloadBytes += 4; // left stick
        if ((hdr0 & 0x10000000u) != 0) payloadBytes += 4; // right stick

        if (remain < payloadBytes) {
            return false;
        }
        pos    += static_cast<uint32_t>(payloadBytes);
        remain -= payloadBytes;

        // 0x1410F299B..0x1410F29CD: Vanilla's buggy lookahead check (`buf + remain`)
        if (remain > 12) {
            if (static_cast<uint32_t>(remain) + 12u > totalBytes) {
                return false;
            }
            uint32_t lk0 = 0, lk1 = 0, lk2 = 0;
            std::memcpy(&lk0, buf + remain + 0, 4);
            std::memcpy(&lk1, buf + remain + 4, 4);
            std::memcpy(&lk2, buf + remain + 8, 4);
            if ((lk0 + lk1) != lk2) {
                return false;
            }
        }
    }

    return (remain == 0);
}

// Clamps outbound NACK headers at the start of m_pRawUdpSendBuf to at most kMaxOutboundNacksPerTick (4)
// oldest missing frames so we never exhaust an unmodded peer's 16-node resend pool.
uint32_t ClampOutboundNackPrefix(uint8_t* rawBuf, uint32_t rawPrefixBytes) noexcept {
    if (!rawBuf || rawPrefixBytes == 0) {
        return 0;
    }

    uint32_t readPos   = 0;
    uint32_t writePos  = 0;
    uint32_t nackCount = 0;
    uint64_t dropped   = 0;

    while (readPos + 12 <= rawPrefixBytes) {
        uint32_t hdr0 = 0, hdr1 = 0, hdr2 = 0;
        std::memcpy(&hdr0, rawBuf + readPos + 0, 4);
        std::memcpy(&hdr1, rawBuf + readPos + 4, 4);
        std::memcpy(&hdr2, rawBuf + readPos + 8, 4);

        if ((hdr0 + hdr1) != hdr2 || static_cast<int32_t>(hdr0) >= 0) {
            break;
        }

        const bool isSpectator16 = ((hdr0 & 0x08000000u) != 0);
        const uint32_t recLen    = isSpectator16 ? 16u : 12u;
        if (readPos + recLen > rawPrefixBytes) {
            break;
        }

        if (isSpectator16 || nackCount < kMaxOutboundNacksPerTick) {
            if (writePos != readPos) {
                std::memmove(rawBuf + writePos, rawBuf + readPos, recLen);
            }
            writePos += recLen;
            if (!isSpectator16) {
                ++nackCount;
            }
        } else {
            ++dropped;
        }

        readPos += recLen;
    }

    if (dropped > 0) {
        g_Telemetry.nacksClamped.fetch_add(dropped, std::memory_order_relaxed);
    }
    return writePos;
}

// Sends an auxiliary Vanilla-Safe UDP datagram to all remote battlers using the exact
// session lookup and ccNetUDPSocket::Send (0x1411623B0) path from SendBattlerPacket (0x1410F3990).
void SendAuxiliaryDatagramToBattlers(ccNetFreeBattleSync* pSync, const uint8_t* data, uint32_t dataSize) noexcept {
    if (!g_fnUdpSocketSend || !pSync || !data || dataSize < 12) {
        return;
    }
    if (!pSync->m_pNetSession || *reinterpret_cast<void* const*>(pSync->m_udpSocket + 0x18) == nullptr) {
        return;
    }
    if (!IsDatagramSafeForVanillaPeer(data, dataSize)) {
        return;
    }

    const int32_t battlerCount = std::clamp(pSync->m_battlerCount, 0, 8);
    if (battlerCount <= 0) {
        return;
    }

    const int32_t mySlot = (pSync->m_myPlayerIdx != -1)
        ? pSync->m_myPlayerIdx
        : (pSync->m_myWatcherIdx + battlerCount);
    if (mySlot < 0 || mySlot >= 8) {
        return;
    }
    const int32_t myMemberId = static_cast<int32_t>(pSync->m_memberIds[mySlot]);

    const auto* pSessBytes = reinterpret_cast<const uint8_t*>(pSync->m_pNetSession);

    for (int32_t b = 0; b < battlerCount; ++b) {
        const int32_t targetMemberId = static_cast<int32_t>(pSync->m_memberIds[b]);
        if (targetMemberId < 0 || targetMemberId == myMemberId) {
            continue;
        }

        for (int32_t i = 0; i < 8; ++i) {
            const uint8_t* entryPtr = pSessBytes + 0x13C + static_cast<size_t>(i) * 0x48;
            const int32_t entryMemberId = *reinterpret_cast<const int32_t*>(entryPtr + 0x40);
            if (entryMemberId == targetMemberId) {
                const uint64_t remoteSteamId = *reinterpret_cast<const uint64_t*>(entryPtr + 0x38);
                if (IsValidSteamId(remoteSteamId)) {
                    ccNetUdpEndpoint endpoint{ remoteSteamId, 6u };
                    g_fnUdpSocketSend(pSync->m_udpSocket, &endpoint, data, dataSize);
                    g_Telemetry.auxDatagramsSent.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    }
}

bool PopNextSpectatorResendRequest(ccNetFreeBattleSync* pSync, int32_t& outFrame) noexcept {
    if (!pSync) return false;
    auto* pBytes = reinterpret_cast<uint8_t*>(pSync);
    uint8_t** pMap    = *reinterpret_cast<uint8_t***>(pBytes + 0xB0);
    uint64_t  mapSize = *reinterpret_cast<uint64_t*>(pBytes + 0xB8);
    uint64_t& myOff   = *reinterpret_cast<uint64_t*>(pBytes + 0xC0);
    uint64_t& mySize  = *reinterpret_cast<uint64_t*>(pBytes + 0xC8);

    if (mySize == 0 || !pMap || mapSize == 0) {
        return false;
    }

    const uint64_t blockIdx = (myOff >> 1) & (mapSize - 1);
    const uint64_t elemIdx  = myOff & 1ULL;
    const uint8_t* blockPtr = pMap[blockIdx];
    outFrame = blockPtr ? *reinterpret_cast<const int32_t*>(blockPtr + elemIdx * 8 + 4) : -1;

    const uint64_t newSize = mySize - 1;
    mySize = newSize;
    myOff  = (newSize == 0) ? 0ULL : (myOff + 1ULL);
    return (outFrame >= 0);
}

constexpr int32_t kMaxRelayCursors = 8;
struct WatcherRelayCursor {
    int32_t memberId          = -1;
    int32_t lastSentHeadFrame = -1;
    int32_t sameHeadCount     = 0;
};

WatcherRelayCursor g_WatcherRelayCursors[kMaxRelayCursors] = {};
int32_t            g_LastSpectatorRelayByBattlerFrame      = -1;
int32_t            g_SameRelayByBattlerCount               = 0;
int32_t            g_StartupRotateIndex                    = 0;
std::atomic<uint64_t> g_LastSessionPreWarmTickMs{ 0 };

// Read-only slot lookup inside ccNetInputBuffer's 12,000-slot ring buffer without advancing ring head.
const ccNetFrameSlot* FindConfirmed1v1SlotReadOnly(const ccNetInputBuffer* pBuf, int32_t frame) noexcept {
    if (!pBuf || frame <= 0) {
        return nullptr;
    }
    if (pBuf->m_ringHeadSlotIdx < 0 || pBuf->m_ringHeadSlotIdx >= 0x2EE0) {
        return nullptr;
    }
    const int32_t delta = frame - pBuf->m_oldestFrameInRing;
    if (delta < 0 || delta >= 0x2EE0) {
        return nullptr;
    }
    const int32_t slotIdx = (pBuf->m_ringHeadSlotIdx + delta) % 0x2EE0;
    const ccNetFrameSlot* slot = &pBuf->m_slots[slotIdx];
    if (slot->frameNumber != frame ||
        slot->players[0].bReceived == 0 ||
        slot->players[1].bReceived == 0) {
        return nullptr;
    }
    return slot;
}

// Encodes an exact native 0x38 (56)-byte Head-Spectator relay record (hdr0 = frame | 0x08000000).
// ReceiveWatcherPackets (0x1410F2EE6..0x1410F3138) sets desyncHash = 0 for BOTH players on relay
// records, ensuring player0.desyncHash == player1.desyncHash == 0 (zero risk of OnDesync()).
uint8_t* AppendRelayRecord(uint8_t* dst, int32_t frame, int32_t senderMemberId, const ccNetFrameSlot* slot) noexcept {
    static_assert(offsetof(ccNetFrameSlot, players) + offsetof(ccNetSlotPlayerEntry, buttons) == 0x08, "P0 offset mismatch");
    static_assert(offsetof(ccNetFrameSlot, players) + sizeof(ccNetSlotPlayerEntry) + offsetof(ccNetSlotPlayerEntry, buttons) == 0x28, "P1 offset mismatch");

    const uint32_t hdr0 = static_cast<uint32_t>(frame) | 0x08000000u;
    const uint32_t hdr1 = 0u;
    const uint32_t hdr2 = hdr0 + hdr1;
    std::memcpy(dst + 0x00, &hdr0, 4);
    std::memcpy(dst + 0x04, &hdr1, 4);
    std::memcpy(dst + 0x08, &hdr2, 4);
    std::memcpy(dst + 0x0C, &senderMemberId, 4);
    std::memcpy(dst + 0x10, &slot->players[0].buttons, 20);
    std::memcpy(dst + 0x24, &slot->players[1].buttons, 20);
    return dst + 0x38;
}

int32_t ResolveLocalMemberId(const ccNetFreeBattleSync* pSync, int32_t battlerCount) noexcept {
    if (!pSync) return -1;
    const int32_t mySlot = (pSync->m_myPlayerIdx >= 0 && pSync->m_myPlayerIdx < 4)
        ? pSync->m_myPlayerIdx
        : (battlerCount + pSync->m_myWatcherIdx);
    if (mySlot < 0 || mySlot >= 8) {
        return -1;
    }
    return static_cast<int32_t>(pSync->m_memberIds[mySlot]);
}

uint64_t FindSessionSteamIdForMember(const uint8_t* pSession, int32_t memberId) noexcept {
    if (!pSession || memberId < 0) return 0;
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        if (*reinterpret_cast<const int32_t*>(entry + 0x40) == memberId) {
            const uint64_t sid = *reinterpret_cast<const uint64_t*>(entry + 0x38);
            if (IsValidSteamId(sid)) {
                return sid;
            }
        }
    }
    return 0;
}

// Allows a modded Battler to directly stream confirmed 1v1 relay records (desyncHash=0) to secondary
// spectators (w >= 1) on Channel 6 so an unmodded 30 FPS Head Spectator never throttles them to 30 FPS.
void RelayConfirmedFramesToSecondarySpectators(ccNetFreeBattleSync* pSync,
                                                 const uint8_t* pSession,
                                                 int32_t battlerCount,
                                                 int32_t myMemberId) noexcept {
    if (!pSync || !pSync->m_pInputBuffer || !pSession || !g_fnUdpSocketSend) return;
    if (pSync->m_battlerCount != 2) return;
    if (*reinterpret_cast<void* const*>(pSync->m_udpSocket + 0x18) == nullptr) return;

    const ccNetInputBuffer* pBuf = pSync->m_pInputBuffer;
    const int32_t confirmedFrame = pBuf->m_confirmedAllPlayersFrame;
    if (confirmedFrame < 1) {
        return;
    }

    // Check upfront whether at least 2 valid spectators exist (so w >= 1 has a recipient)
    int32_t validWatchers = 0;
    for (int32_t w = 0; w < (8 - battlerCount); ++w) {
        const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
        if (candidateId < 0 || candidateId == myMemberId) {
            continue;
        }
        if (FindSessionSteamIdForMember(pSession, candidateId) != 0) {
            ++validWatchers;
        }
    }
    if (validWatchers < 2) {
        return;
    }

    const bool advanced = (confirmedFrame > g_LastSpectatorRelayByBattlerFrame);
    if (advanced) {
        g_SameRelayByBattlerCount = 1;
    } else {
        ++g_SameRelayByBattlerCount;
        // Gentle backup resend on tick 3 and every 10th tick up to 40, then wait for new confirmation
        if (g_SameRelayByBattlerCount != 3 &&
            (g_SameRelayByBattlerCount > 40 || (g_SameRelayByBattlerCount % 10) != 0)) {
            return;
        }
    }

    int32_t firstNeeded = (g_LastSpectatorRelayByBattlerFrame >= 1 && advanced)
        ? (g_LastSpectatorRelayByBattlerFrame + 1)
        : (confirmedFrame - 3);
    int32_t startFrame = std::min(firstNeeded, confirmedFrame - 3);
    if ((confirmedFrame - startFrame + 1) > 6) {
        startFrame = confirmedFrame - 5;
    }
    startFrame = std::max(1, startFrame);

    uint8_t  relayBuf[6 * 0x38 + 16] = {};
    uint8_t* writePtr                = relayBuf;
    int32_t  highestPacked           = -1;

    for (int32_t f = startFrame; f <= confirmedFrame; ++f) {
        const ccNetFrameSlot* slot = FindConfirmed1v1SlotReadOnly(pBuf, f);
        if (!slot) {
            continue;
        }
        writePtr      = AppendRelayRecord(writePtr, f, myMemberId, slot);
        highestPacked = f;
    }

    const uint32_t totalBytes = static_cast<uint32_t>(writePtr - relayBuf);
    if (totalBytes == 0) {
        return;
    }

    if (highestPacked > g_LastSpectatorRelayByBattlerFrame) {
        g_LastSpectatorRelayByBattlerFrame = highestPacked;
    }

    bool firstWatcherSeen = false;
    for (int32_t w = 0; w < (8 - battlerCount); ++w) {
        const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
        if (candidateId < 0 || candidateId == myMemberId) {
            continue;
        }
        const uint64_t steamId = FindSessionSteamIdForMember(pSession, candidateId);
        if (steamId == 0) {
            continue;
        }
        if (!firstWatcherSeen) {
            // Skip w == 0 (Head Spectator receives full per-player packets with desyncHash)
            firstWatcherSeen = true;
            continue;
        }
        ccNetUdpEndpoint ep{ steamId, 6u };
        g_fnUdpSocketSend(pSync->m_udpSocket, &ep, relayBuf, totalBytes);
    }
}

} // namespace

void ResetSpectatorRelayState() noexcept {
    for (auto& c : g_WatcherRelayCursors) {
        c = WatcherRelayCursor{};
    }
    g_LastSpectatorRelayByBattlerFrame = -1;
    g_SameRelayByBattlerCount          = 0;
    g_StartupRotateIndex               = 0;
    g_LastSessionPreWarmTickMs.store(0, std::memory_order_relaxed);
}

// Opens Steam P2P Channel 6 connections with ALL lobby/session peers ahead of StartFrameSync
// (especially critical when Host is Spectating and Client 1 <-> Client 2 must complete ICE/SDR
// traversal for the first time). Uses a stack-local dummy ccNetUDPSocket and Frame 0 No-Op `{0,1,1}`.
void PreWarmSessionPeersOnChannel6(bool bypassThrottle) noexcept {
    if (!g_BaseAddress || !g_fnUdpSocketSend) {
        return;
    }

    const uint64_t nowMs  = GetTickCount64();
    const uint64_t prevMs = g_LastSessionPreWarmTickMs.load(std::memory_order_relaxed);
    if (!bypassThrottle && prevMs != 0 && (nowMs - prevMs) < 250ULL) {
        return;
    }

    const uint8_t* pNetMgr = *reinterpret_cast<const uint8_t* const*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
    if (!pNetMgr) {
        return;
    }
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    if (!pSession) {
        return;
    }

    int32_t localMemberId = -1;
    if (const uint8_t* pLocalInfo = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x100)) {
        localMemberId = *reinterpret_cast<const int32_t*>(pLocalInfo + 0x24);
    }

    alignas(4) static constexpr uint32_t kWarmupNoOp12[3] = { 0x00000000u, 0x00000001u, 0x00000001u };
    const auto* rawNoOp = reinterpret_cast<const uint8_t*>(kWarmupNoOp12);

    uint8_t dummySock[0x20]    = {};
    uint8_t dummyScratch[32]   = {};
    *reinterpret_cast<void**>(dummySock + 0x18) = dummyScratch;

    bool sentAny = false;
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
        const uint64_t steamId  = *reinterpret_cast<const uint64_t*>(entry + 0x38);
        if (!IsValidSteamId(steamId) || memberId == localMemberId) {
            continue;
        }
        ccNetUdpEndpoint ep{ steamId, 6u };
        g_fnUdpSocketSend(dummySock, &ep, rawNoOp, 12u);
        sentAny = true;
    }

    if (sentAny) {
        g_LastSessionPreWarmTickMs.store(nowMs, std::memory_order_relaxed);
    }
}

// Pre-warms Steam P2P Channel 6 during SetupFrameSyncAndBuffers / UpdateMemberCountAndDelay
// (seconds before Frame 0 of the fight begins) using the native 12-byte Frame 0 No-Op
// `{ 0x00000000, 0x00000001, 0x00000001 }` (`frameNumber == 0`, ignored by DeserializeAndStoreFrameInput
// at 0x1410F1673 on both modded & unmodded peers). Since NSUNSC.exe closes Channel 6 at the end of
// every match (0x14116246B), this eliminates the cold-channel setup hiccup on Frame 0.
void PreWarmBattleUdpChannel(ccNetFreeBattleSync* pSync) noexcept {
    if (!g_fnUdpSocketSend || !pSync || !pSync->m_pNetSession) {
        return;
    }
    if (*reinterpret_cast<void* const*>(pSync->m_udpSocket + 0x18) == nullptr) {
        return;
    }
    alignas(4) static constexpr uint32_t kWarmupNoOp12[3] = { 0x00000000u, 0x00000001u, 0x00000001u };
    const auto* rawNoOp = reinterpret_cast<const uint8_t*>(kWarmupNoOp12);

    SendAuxiliaryDatagramToBattlers(pSync, rawNoOp, 12u);

    // Also pre-warm Channel 6 toward ALL Spectators (so secondary spectators also open P2P early)
    const int32_t  battlerCount = std::clamp(pSync->m_battlerCount, 0, 8);
    const int32_t  myMemberId   = ResolveLocalMemberId(pSync, battlerCount);
    const auto*    pSession     = reinterpret_cast<const uint8_t*>(pSync->m_pNetSession);
    for (int32_t w = 0; w < (8 - battlerCount); ++w) {
        const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
        if (candidateId < 0 || candidateId == myMemberId) {
            continue;
        }
        const uint64_t watcherSteamId = FindSessionSteamIdForMember(pSession, candidateId);
        if (watcherSteamId != 0) {
            ccNetUdpEndpoint ep{ watcherSteamId, 6u };
            g_fnUdpSocketSend(pSync->m_udpSocket, &ep, rawNoOp, 12u);
        }
    }
}

// ============================================================================
// 3. Vanilla-Safe Multi-Frame UDP Redundancy & Single-Record Auxiliary Resends
//    Hook on ccNetFreeBattleSync::BuildBattlerSendPacket (VA 0x1410F2310)
// ============================================================================

int32_t __fastcall hk_BuildBattlerSendPacket(ccNetFreeBattleSync* pSync) {
    if (!pSync || !pSync->m_pInputBuffer || !pSync->m_pRawUdpSendBuf) {
        return oBuildSendPacket(pSync);
    }

    // 1. Clamp any outbound NACK headers written by SendBattlerPacket (0x1410F3840) to <= 4 NACKs
    //    (48 bytes max) so we NEVER overflow an unmodded opponent's 16-slot resend pool.
    const uint32_t clampedPrefixBytes = ClampOutboundNackPrefix(pSync->m_pRawUdpSendBuf, pSync->m_rawUdpSendSize);
    pSync->m_rawUdpSendSize = clampedPrefixBytes;

    // 2. Run original BuildBattlerSendPacket:
    //    Samples local input + SyncCheckWriter checksum for m_sendFrameCounter into m_pInputBuffer,
    //    and pops at most 1 pending resend request into m_pSendFrameBuf[0].
    const int32_t origFrameCount = oBuildSendPacket(pSync);

    if (!g_fnFreeSyncExtractFramePkt ||
        !g_fnFreeSyncSerializeOneFrm ||
        pSync->m_myPlayerIdx < 0 ||
        pSync->m_myPlayerIdx >= 4) {
        return origFrameCount;
    }

    const int32_t currentFrame = pSync->m_sendFrameCounter;
    if (currentFrame != s_LastPackedSendFrame) {
        s_LastPackedSendFrame = currentFrame;
        s_SameFrameSendCount  = 1;
    } else {
        ++s_SameFrameSendCount;
    }

    // 3. Collect explicit NACK-requested resend frames (including any 1st resend popped by oBuildSendPacket
    //    so even when EnablePacketRedundancy=0 we still protect resends from vanilla's lookahead bug!).
    int32_t resendFrames[8] = {};
    int32_t resendCount     = 0;
    auto addUniqueResend = [&](int32_t f) {
        if (f == currentFrame || !IsLocalFrameReadyForSend(pSync, f)) return;
        for (int32_t i = 0; i < resendCount; ++i) {
            if (resendFrames[i] == f) return;
        }
        if (resendCount < 8) {
            resendFrames[resendCount++] = f;
        }
    };

    if (origFrameCount > 0 && pSync->m_pSendFrameBuf) {
        int32_t firstPopped = pSync->m_pSendFrameBuf[0].frameNumber;
        if (firstPopped != currentFrame) {
            addUniqueResend(firstPopped);
        }
    }

    if (g_Config.enable_packet_redundancy) {
        const int32_t extraResendsToPop = std::clamp(g_Config.max_resend_batch, 1, 4) - 1;
        for (int32_t i = 0; i < extraResendsToPop; ++i) {
            int32_t extraResendFrame = 0;
            if (!PopNextResendRequest(pSync, extraResendFrame)) {
                break;
            }
            addUniqueResend(extraResendFrame);
            g_Telemetry.multiResendsDrained.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::sort(resendFrames, resendFrames + resendCount);

    // 4. Helper to transmit any explicit NACK-requested resend frames as individual 1-record UDP datagrams
    //    (remain == 0 <= 12 -> 100% immune to vanilla's lookahead bug!), skipping ONLY frames that
    //    were actually packed into the main datagram's Vanilla-Safe suffix.
    int32_t auxFramesDelivered = 0;
    auto sendUncoveredResends = [&](const int32_t* coveredFrames, int32_t coveredCount) {
        for (int32_t r = 0; r < resendCount; ++r) {
            bool covered = false;
            for (int32_t c = 0; c < coveredCount; ++c) {
                if (coveredFrames[c] == resendFrames[r]) {
                    covered = true;
                    break;
                }
            }
            if (covered) {
                continue;
            }
            uint8_t singleBuf[32] = {};
            alignas(16) ccNetFrameInputPacket pkt = {};
            g_fnFreeSyncExtractFramePkt(pSync, &pkt, resendFrames[r]);
            if (pkt.frameNumber != 0) {
                uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(singleBuf, &pkt);
                const uint32_t singleLen = static_cast<uint32_t>(endPtr - singleBuf);
                if (singleLen >= 12) {
                    SendAuxiliaryDatagramToBattlers(pSync, singleBuf, singleLen);
                    ++auxFramesDelivered;
                }
            }
        }
    };

    // 5. CRITICAL ANTI-FLOOD GUARD FOR STALLED TICKS (s_SameFrameSendCount > 1):
    //    Normally, while stalled waiting for a remote input mid-fight, silence keeps Wi-Fi un-congested.
    //    EXCEPTION: During initial opening lockstep (confirmed < m_netDelayFrames), Client<->Client Steam P2P
    //    may still be finishing NAT/ICE handshake, so keep Channel 6 alive gently and rotate unconfirmed
    //    opening frames [m_netDelayFrames .. currentFrame - 1] so no opening frames remain lost!
    if (s_SameFrameSendCount > 1) {
        sendUncoveredResends(nullptr, 0);
        const int32_t confFrame        = pSync->m_pInputBuffer->m_confirmedAllPlayersFrame;
        const int32_t delayFloor       = std::max<int32_t>(1, pSync->m_netDelayFrames);
        const bool    inStartupLockstep = (confFrame < delayFloor);

        const bool startupKeepaliveTick = inStartupLockstep &&
            (s_SameFrameSendCount == 2 ||
             s_SameFrameSendCount == 4 ||
             (s_SameFrameSendCount >= 6 && (s_SameFrameSendCount % 6) == 0));

        if (clampedPrefixBytes == 0 && s_SameFrameSendCount != 4 && !startupKeepaliveTick) {
            pSync->m_rawUdpSendSize = 0;
            return auxFramesDelivered;
        }

        if (inStartupLockstep && (clampedPrefixBytes > 0 || startupKeepaliveTick)) {
            const int32_t firstMissing = std::max(delayFloor, confFrame + 1);
            const int32_t spanCount    = currentFrame - firstMissing;
            if (spanCount > 0) {
                const int32_t maxRotate = std::min<int32_t>(2, spanCount);
                for (int32_t k = 0; k < maxRotate; ++k) {
                    const int32_t candFrame = firstMissing + ((g_StartupRotateIndex + k) % spanCount);
                    if (candFrame >= delayFloor && candFrame < currentFrame) {
                        bool alreadySent = false;
                        for (int32_t r = 0; r < resendCount; ++r) {
                            if (resendFrames[r] == candFrame) {
                                alreadySent = true;
                                break;
                            }
                        }
                        if (!alreadySent && IsLocalFrameReadyForSend(pSync, candFrame)) {
                            uint8_t singleBuf[32] = {};
                            alignas(16) ccNetFrameInputPacket pkt = {};
                            g_fnFreeSyncExtractFramePkt(pSync, &pkt, candFrame);
                            if (pkt.frameNumber != 0) {
                                uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(singleBuf, &pkt);
                                const uint32_t singleLen = static_cast<uint32_t>(endPtr - singleBuf);
                                if (singleLen >= 12) {
                                    SendAuxiliaryDatagramToBattlers(pSync, singleBuf, singleLen);
                                    ++auxFramesDelivered;
                                }
                            }
                        }
                    }
                }
                g_StartupRotateIndex = (g_StartupRotateIndex + maxRotate) % spanCount;
            }
        }

        // Pack only currentFrame (1 record at end of packet -> remain == 0 <= 12, 100% vanilla-safe)
        if (IsLocalFrameReadyForSend(pSync, currentFrame)) {
            alignas(16) ccNetFrameInputPacket curPkt = {};
            g_fnFreeSyncExtractFramePkt(pSync, &curPkt, currentFrame);
            if (curPkt.frameNumber != 0) {
                uint8_t* writePtr = pSync->m_pRawUdpSendBuf + clampedPrefixBytes;
                uint8_t* endPtr   = g_fnFreeSyncSerializeOneFrm(writePtr, &curPkt);
                const uint32_t candidateTotal = static_cast<uint32_t>(endPtr - pSync->m_pRawUdpSendBuf);
                if (IsDatagramSafeForVanillaPeer(pSync->m_pRawUdpSendBuf, candidateTotal)) {
                    pSync->m_rawUdpSendSize = candidateTotal;
                    g_Telemetry.packetsBuilt.fetch_add(1, std::memory_order_relaxed);
                    return 1 + auxFramesDelivered;
                }
            }
        }
        pSync->m_rawUdpSendSize = clampedPrefixBytes;
        return auxFramesDelivered;
    }

    // 6. Newly advanced frame (s_SameFrameSendCount == 1):
    //    Build ascending sliding window [currentFrame - effectiveDepth + 1 .. currentFrame].
    int32_t configuredDepth = g_Config.enable_packet_redundancy
        ? std::clamp(g_Config.redundancy_frame_depth, 1, 4)
        : 1;
    if (g_LiveAntiJitterActive.load()) {
        configuredDepth = std::clamp(std::max(configuredDepth, g_LiveAntiJitterRedundancyDepth.load()), 1, 4);
    }

    int32_t windowFrames[6] = {};
    int32_t windowCount     = 0;
    for (int32_t k = configuredDepth - 1; k >= 0; --k) {
        const int32_t f = currentFrame - k;
        if (IsLocalFrameReadyForSend(pSync, f)) {
            windowFrames[windowCount++] = f;
        }
    }

    auto serializeSingleFrame = [&](int32_t frameNum, uint8_t outBuf[32]) -> uint32_t {
        alignas(16) ccNetFrameInputPacket pkt = {};
        g_fnFreeSyncExtractFramePkt(pSync, &pkt, frameNum);
        if (pkt.frameNumber == 0) {
            return 0;
        }
        uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(outBuf, &pkt);
        return static_cast<uint32_t>(endPtr - outBuf);
    };

    if (windowCount <= 0) {
        sendUncoveredResends(nullptr, 0);
        pSync->m_rawUdpSendSize = clampedPrefixBytes;
        return auxFramesDelivered;
    }

    // 7. Pre-serialize each frame in windowFrames (preserving exact frame number per blob)
    uint8_t  winBlobs[6][32] = {};
    uint32_t winLens[6]      = {};
    int32_t  winFrameNums[6] = {};
    int32_t  validWinCount   = 0;
    for (int32_t w = 0; w < windowCount; ++w) {
        const uint32_t len = serializeSingleFrame(windowFrames[w], winBlobs[validWinCount]);
        if (len >= 12) {
            winLens[validWinCount]      = len;
            winFrameNums[validWinCount] = windowFrames[w];
            ++validWinCount;
        }
    }

    if (validWinCount <= 0) {
        sendUncoveredResends(nullptr, 0);
        pSync->m_rawUdpSendSize = clampedPrefixBytes;
        return auxFramesDelivered;
    }

    // 8. Find the longest suffix winBlobs[bestStart .. validWinCount - 1] that, when appended after
    //    clampedPrefixBytes in m_pRawUdpSendBuf, passes IsDatagramSafeForVanillaPeer.
    uint8_t* const rawBase = pSync->m_pRawUdpSendBuf;
    int32_t  bestStart     = validWinCount - 1;
    uint32_t bestTotalSize = clampedPrefixBytes;

    for (int32_t startIdx = 0; startIdx < validWinCount; ++startIdx) {
        uint8_t* writePtr = rawBase + clampedPrefixBytes;
        for (int32_t j = startIdx; j < validWinCount; ++j) {
            std::memcpy(writePtr, winBlobs[j], winLens[j]);
            writePtr += winLens[j];
        }
        const uint32_t candidateTotal = static_cast<uint32_t>(writePtr - rawBase);
        if (IsDatagramSafeForVanillaPeer(rawBase, candidateTotal)) {
            bestStart     = startIdx;
            bestTotalSize = candidateTotal;
            break;
        }
    }

    const int32_t mainFramesPacked = validWinCount - bestStart;

    // 9. Deliver any explicit NACK-requested resend frame that did NOT fit into the main
    //    Vanilla-Safe suffix (fixes silent resend drops when Redundancy is off, depth < 3,
    //    or bestStart > 0 due to an input-size transition).
    sendUncoveredResends(winFrameNums + bestStart, mainFramesPacked);

    // 10. If bestStart > 0, rewrite the chosen Vanilla-Safe suffix into m_pRawUdpSendBuf.
    //     To prevent Wi-Fi UDP packet-rate doubling and reordering, ONLY send a single-record
    //     auxiliary datagram for (currentFrame - 1) when mainFramesPacked == 1 (i.e. input size
    //     transitioned on currentFrame so 0 redundant frames fit in the main packet) and no NACKs
    //     or explicit resends were already sent this tick.
    if (bestStart > 0) {
        if (mainFramesPacked == 1 && validWinCount >= 2 && clampedPrefixBytes == 0 && auxFramesDelivered == 0) {
            SendAuxiliaryDatagramToBattlers(pSync, winBlobs[validWinCount - 2], winLens[validWinCount - 2]);
            ++auxFramesDelivered;
        }

        uint8_t* writePtr = rawBase + clampedPrefixBytes;
        for (int32_t j = bestStart; j < validWinCount; ++j) {
            std::memcpy(writePtr, winBlobs[j], winLens[j]);
            writePtr += winLens[j];
        }
        bestTotalSize = static_cast<uint32_t>(writePtr - rawBase);
    }

    pSync->m_rawUdpSendSize = bestTotalSize;
    const int32_t totalDelivered = mainFramesPacked + auxFramesDelivered;

    g_Telemetry.packetsBuilt.fetch_add(1, std::memory_order_relaxed);
    if (totalDelivered > 1) {
        g_Telemetry.redundantFramesSent.fetch_add(static_cast<uint64_t>(totalDelivered - 1), std::memory_order_relaxed);
    }

    return mainFramesPacked;
}

// ============================================================================
// 3b. Spectator Stream Redundancy & NACK Deque Deduplication
//     Hook on ccNetFreeBattleSync::SendSpectatorPacket (VA 0x1410F3A50)
//     In vanilla NSUNSC.exe, Head-Spectator-to-Spectator relay (0x1410F3D70) sends
//     3 redundant frames per packet, but Battler-to-Head-Spectator (0x1410F3A50)
//     only sends 1 current frame and pops at most 1 entry from the 8-slot
//     m_spectatorDeque (+0x90..+0xCF). We add sliding-window redundancy and
//     multi-resend deduplication so spectators never stall on single packet drops.
// ============================================================================

void __fastcall hk_SendSpectatorPacket(ccNetFreeBattleSync* pSync) {
    if (!pSync ||
        !pSync->m_pInputBuffer ||
        !pSync->m_pRawUdpSendBuf ||
        !g_fnFreeSyncExtractFramePkt ||
        !g_fnFreeSyncSerializeOneFrm ||
        !g_fnUdpSocketSend ||
        pSync->m_myPlayerIdx < 0 ||
        pSync->m_myPlayerIdx >= 4) {
        oSendSpectatorPacket(pSync);
        return;
    }

    // 1. Locate the Head Spectator in pSync->m_pNetSession (+0x108), matching 0x1410F1FB0 & 0x1410F3C2E..0x1410F3CBC
    const int32_t  battlerCount = std::clamp(pSync->m_battlerCount, 0, 8);
    const uint8_t* pSession     = reinterpret_cast<const uint8_t*>(pSync->m_pNetSession);
    if (!pSession) {
        oSendSpectatorPacket(pSync);
        return;
    }

    int32_t  headWatcherMemberId = -1;
    uint64_t headWatcherSteamId  = 0;
    for (int32_t w = 0; w < (8 - battlerCount); ++w) {
        const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
        if (candidateId < 0) continue;
        const uint64_t sid = FindSessionSteamIdForMember(pSession, candidateId);
        if (sid != 0) {
            headWatcherMemberId = candidateId;
            headWatcherSteamId  = sid;
            break;
        }
    }

    const int32_t myMemberId = static_cast<int32_t>(pSync->m_memberIds[pSync->m_myPlayerIdx]);
    if (headWatcherMemberId == -1 || headWatcherMemberId == myMemberId || headWatcherSteamId == 0) {
        oSendSpectatorPacket(pSync);
        return;
    }

    // Stream confirmed 1v1 relay frames (desyncHash=0) directly to non-head Spectators (w >= 1)
    // before any stalled early return so an unmodded 30 FPS Head Spectator never throttles them.
    RelayConfirmedFramesToSecondarySpectators(pSync, pSession, battlerCount, myMemberId);

    // 2. Build ascending sliding window [currentFrame - effectiveDepth + 1 .. currentFrame]
    const int32_t currentFrame = pSync->m_sendFrameCounter;
    if (currentFrame != s_LastPackedSpectatorSendFrame) {
        s_LastPackedSpectatorSendFrame = currentFrame;
        s_SameFrameSpectatorSendCount  = 1;
    } else {
        ++s_SameFrameSpectatorSendCount;
    }

    int32_t configuredDepth = g_Config.enable_packet_redundancy
        ? std::clamp(g_Config.redundancy_frame_depth, 1, 5)
        : 1;
    if (g_LiveAntiJitterActive.load()) {
        configuredDepth = std::clamp(std::max(configuredDepth, g_LiveAntiJitterRedundancyDepth.load()), 1, 5);
    }

    const bool keepBurstWindow = (s_SameFrameSpectatorSendCount <= 8) ||
                                 (s_SameFrameSpectatorSendCount <= 120 && (s_SameFrameSpectatorSendCount % 3) == 0);
    const int32_t effectiveDepth = keepBurstWindow ? configuredDepth : 1;

    int32_t windowFrames[6] = {};
    int32_t windowCount     = 0;
    for (int32_t k = effectiveDepth - 1; k >= 0; --k) {
        const int32_t f = currentFrame - k;
        if (f >= pSync->m_netDelayFrames && IsLocalFrameReadyForSend(pSync, f)) {
            windowFrames[windowCount++] = f;
        }
    }

    auto isInWindow = [&](int32_t f) noexcept -> bool {
        for (int32_t w = 0; w < windowCount; ++w) {
            if (windowFrames[w] == f) return true;
        }
        return false;
    };

    // 3. Drain and deduplicate pending NACK resend requests from m_spectatorDeque (+0x90..+0xCF)
    //    so even an unmodded spectator spamming 60 NACKs/sec never clogs the 8-slot deque.
    int32_t resendFrames[4] = {};
    int32_t resendCount     = 0;
    const int32_t maxResends = g_Config.enable_packet_redundancy
        ? std::clamp(g_Config.max_resend_batch, 1, 4)
        : 1;

    for (int32_t pops = 0; pops < 16; ++pops) {
        int32_t reqFrame = -1;
        if (!PopNextSpectatorResendRequest(pSync, reqFrame)) {
            break;
        }
        if (reqFrame < pSync->m_netDelayFrames || !IsLocalFrameReadyForSend(pSync, reqFrame) || isInWindow(reqFrame)) {
            continue;
        }
        bool alreadyQueued = false;
        for (int32_t i = 0; i < resendCount; ++i) {
            if (resendFrames[i] == reqFrame) {
                alreadyQueued = true;
                break;
            }
        }
        if (alreadyQueued) {
            continue;
        }
        if (resendCount < maxResends) {
            resendFrames[resendCount++] = reqFrame;
            if (resendCount > 1) {
                g_Telemetry.multiResendsDrained.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (resendCount >= maxResends) {
            break;
        }
    }
    std::sort(resendFrames, resendFrames + resendCount);

    // If stalled on the same frame (s_SameFrameSpectatorSendCount > 1) and the spectator hasn't
    // requested any resend (resendCount == 0), only send 1 backup packet on tick 4 to avoid UDP spam.
    if (s_SameFrameSpectatorSendCount > 1 && resendCount == 0 && s_SameFrameSpectatorSendCount != 4) {
        pSync->m_rawUdpSendSize = 0;
        return;
    }

    // 4. Serialize [resendFrames..., windowFrames...] sequentially into m_pRawUdpSendBuf (+0x118).
    //    ReceiveWatcherPackets (0x1410F2E50..0x1410F2FF7) loops `cmp esi, 0xC; jae 0x1410F2E50`
    //    natively over all complete FrameInput records in the datagram on both modded & unmodded spectators.
    uint8_t* const rawBase = pSync->m_pRawUdpSendBuf;
    uint8_t* writePtr      = rawBase;

    auto appendSerializedFrame = [&](int32_t frameNum) {
        alignas(16) ccNetFrameInputPacket pkt = {};
        g_fnFreeSyncExtractFramePkt(pSync, &pkt, frameNum);
        if (pkt.frameNumber != 0) {
            writePtr = g_fnFreeSyncSerializeOneFrm(writePtr, &pkt);
        }
    };

    for (int32_t r = 0; r < resendCount; ++r) {
        appendSerializedFrame(resendFrames[r]);
    }
    for (int32_t w = 0; w < windowCount; ++w) {
        appendSerializedFrame(windowFrames[w]);
    }

    const uint32_t totalBytes = static_cast<uint32_t>(writePtr - rawBase);
    pSync->m_rawUdpSendSize   = totalBytes;
    pSync->m_lastPeerMemberId = headWatcherMemberId;

    if (totalBytes > 0) {
        ccNetUdpEndpoint ep = {};
        ep.steamId = headWatcherSteamId;
        ep.channel = 6u; // Battle FrameSync P2P channel
        g_fnUdpSocketSend(pSync->m_udpSocket, &ep, rawBase, totalBytes);
    }
}

// ============================================================================
// 3c. Unthrottled Head-Spectator Relay
//     Hook on ccNetFreeBattleSync::SendWatcherRelayPacket (VA 0x1410F3D70)
//     In vanilla NSUNSC.exe, Head Spectator uses m_currentSyncFrame (locked to its
//     own render rate, e.g. 30 FPS) instead of m_confirmedAllPlayersFrame, which caps
//     secondary 60 FPS Spectators to 30 FPS slow-motion. We relay confirmed 1v1 frames
//     up to m_confirmedAllPlayersFrame with catch-up bursts (up to 6 frames / tick).
// ============================================================================

void __fastcall hk_SendWatcherRelayPacket(ccNetFreeBattleSync* pSync, int32_t targetMemberId) {
    if (!pSync ||
        !pSync->m_pInputBuffer ||
        !pSync->m_pNetSession ||
        !pSync->m_pRawUdpSendBuf ||
        !g_fnUdpSocketSend ||
        pSync->m_battlerCount != 2 ||
        targetMemberId < 0) {
        oSendWatcherRelayPacket(pSync, targetMemberId);
        return;
    }

    const auto*    pSession      = reinterpret_cast<const uint8_t*>(pSync->m_pNetSession);
    const uint64_t targetSteamId = FindSessionSteamIdForMember(pSession, targetMemberId);
    if (targetSteamId == 0) {
        oSendWatcherRelayPacket(pSync, targetMemberId);
        return;
    }

    const int32_t battlerCount   = std::clamp(pSync->m_battlerCount, 0, 8);
    const int32_t senderMemberId = ResolveLocalMemberId(pSync, battlerCount);
    if (senderMemberId < 0) {
        oSendWatcherRelayPacket(pSync, targetMemberId);
        return;
    }

    const ccNetInputBuffer* pBuf = pSync->m_pInputBuffer;
    const int32_t headFrame      = std::max(pBuf->m_confirmedAllPlayersFrame, pBuf->m_currentSyncFrame);
    if (headFrame <= 0) {
        pSync->m_rawUdpSendSize = 0;
        return;
    }

    // Locate or allocate per-target cursor (up to 8 spectators)
    WatcherRelayCursor* cursor = nullptr;
    for (auto& c : g_WatcherRelayCursors) {
        if (c.memberId == targetMemberId) {
            cursor = &c;
            break;
        }
    }
    if (!cursor) {
        for (auto& c : g_WatcherRelayCursors) {
            if (c.memberId < 0) {
                c.memberId          = targetMemberId;
                c.lastSentHeadFrame = headFrame - 1;
                c.sameHeadCount     = 0;
                cursor              = &c;
                break;
            }
        }
    }

    const bool advanced = cursor ? (headFrame > cursor->lastSentHeadFrame) : true;
    if (cursor) {
        if (advanced) {
            cursor->sameHeadCount = 1;
        } else {
            ++cursor->sameHeadCount;
            // While Head Spectator has not confirmed a newer frame, keep duplicate traffic low
            const bool allowRepeat = (cursor->sameHeadCount == 2) ||
                                     (cursor->sameHeadCount == 4) ||
                                     (cursor->sameHeadCount >= 6 && cursor->sameHeadCount <= 180 && (cursor->sameHeadCount % 6) == 0) ||
                                     (cursor->sameHeadCount > 180 && (cursor->sameHeadCount % 30) == 0);
            if (!allowRepeat) {
                pSync->m_rawUdpSendSize = 0;
                return;
            }
        }
    }

    const int32_t firstNeeded = (cursor && advanced) ? (cursor->lastSentHeadFrame + 1) : (headFrame - 3);
    int32_t startFrame = std::min(firstNeeded, headFrame - 3);
    if ((headFrame - startFrame + 1) > 6) {
        startFrame = headFrame - 5;
    }
    startFrame = std::max(1, startFrame);

    uint8_t* const rawBase  = pSync->m_pRawUdpSendBuf;
    uint8_t*       writePtr = rawBase;
    int32_t        highestPacked = -1;

    for (int32_t f = startFrame; f <= headFrame; ++f) {
        const ccNetFrameSlot* slot = FindConfirmed1v1SlotReadOnly(pBuf, f);
        if (!slot) {
            continue;
        }
        writePtr      = AppendRelayRecord(writePtr, f, senderMemberId, slot);
        highestPacked = f;
    }

    const uint32_t totalBytes = static_cast<uint32_t>(writePtr - rawBase);
    pSync->m_rawUdpSendSize   = totalBytes;
    pSync->m_lastPeerMemberId = targetMemberId;

    if (totalBytes > 0) {
        if (cursor && highestPacked > cursor->lastSentHeadFrame) {
            cursor->lastSentHeadFrame = highestPacked;
        }
        ccNetUdpEndpoint ep{ targetSteamId, 6u };
        g_fnUdpSocketSend(pSync->m_udpSocket, &ep, rawBase, totalBytes);
    }
}

} // namespace StormInternal
