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
    return (entry.bReceived != 0 && entry.checksum != 0);
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
    const int32_t sessMemberCount = std::clamp(*reinterpret_cast<const int32_t*>(pSessBytes + 0x2C), 0, 16);
    if (sessMemberCount <= 0) {
        return;
    }

    for (int32_t b = 0; b < battlerCount; ++b) {
        const int32_t targetMemberId = static_cast<int32_t>(pSync->m_memberIds[b]);
        if (targetMemberId == myMemberId) {
            continue;
        }

        for (int32_t i = 0; i < sessMemberCount; ++i) {
            const uint8_t* entryPtr = pSessBytes + 0x13C + static_cast<size_t>(i) * 0x48;
            const int32_t entryMemberId = *reinterpret_cast<const int32_t*>(entryPtr + 0x40);
            if (entryMemberId == targetMemberId) {
                const uint64_t remoteSteamId = *reinterpret_cast<const uint64_t*>(entryPtr + 0x38);
                if (remoteSteamId != 0) {
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

} // namespace

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

    // If we are an active Battler and the lobby has Spectators, also warm up Channel 6 to the Head Spectator
    if (pSync->m_myPlayerIdx >= 0 && pSync->m_myPlayerIdx < 4) {
        const int32_t  battlerCount = std::clamp(pSync->m_battlerCount, 0, 8);
        const auto*    pSession     = reinterpret_cast<const uint8_t*>(pSync->m_pNetSession);
        const uint32_t sessCount    = *reinterpret_cast<const uint32_t*>(pSession + 0x2C);
        if (sessCount > static_cast<uint32_t>(battlerCount)) {
            for (int32_t w = 0; w < (8 - battlerCount); ++w) {
                const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
                if (candidateId < 0) continue;
                for (uint32_t i = 0; i < sessCount && i < 16; ++i) {
                    const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
                    if (*reinterpret_cast<const int32_t*>(entry + 0x40) == candidateId) {
                        const uint64_t watcherSteamId = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                        if (watcherSteamId != 0) {
                            ccNetUdpEndpoint ep{ watcherSteamId, 6u };
                            g_fnUdpSocketSend(pSync->m_udpSocket, &ep, rawNoOp, 12u);
                        }
                        return;
                    }
                }
            }
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

    // 4. Transmit any explicit NACK-requested resend frames as individual 1-record UDP datagrams
    //    (remain == 0 <= 12 -> 100% immune to vanilla's lookahead bug!).
    int32_t auxFramesDelivered = 0;
    for (int32_t r = 0; r < resendCount; ++r) {
        if (s_SameFrameSendCount == 1 && resendFrames[r] >= (currentFrame - 2)) {
            continue; // Will be covered by current frame's sliding window
        }
        uint8_t singleBuf[32] = {};
        alignas(16) ccNetFrameInputPacket pkt = {};
        g_fnFreeSyncExtractFramePkt(pSync, &pkt, resendFrames[r]);
        if (pkt.frameNumber != 0 && pkt.checksum != 0) {
            uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(singleBuf, &pkt);
            const uint32_t singleLen = static_cast<uint32_t>(endPtr - singleBuf);
            if (singleLen >= 12) {
                SendAuxiliaryDatagramToBattlers(pSync, singleBuf, singleLen);
                ++auxFramesDelivered;
            }
        }
    }

    // 5. CRITICAL ANTI-FLOOD GUARD FOR STALLED TICKS (s_SameFrameSendCount > 1):
    //    In vanilla NSUNSC.exe (0x1410F234D..0x1410F24D6), when the game is stalled waiting for the
    //    remote peer's input (s_SameFrameSendCount >= 2), currentFrame was ALREADY sampled and sent
    //    on tick 1. Flooding duplicate packets on every stalled tick congests Wi-Fi / Steam P2P!
    //    While stalled, ONLY send a packet if:
    //      a) We have outbound NACK headers in m_pRawUdpSendBuf (clampedPrefixBytes > 0), OR
    //      b) On tick 4 of a stall (1 single backup re-transmission of currentFrame in case tick 1 dropped).
    if (s_SameFrameSendCount > 1) {
        if (clampedPrefixBytes == 0 && s_SameFrameSendCount != 4) {
            pSync->m_rawUdpSendSize = 0;
            return auxFramesDelivered;
        }
        // Pack only currentFrame (1 record at end of packet -> remain == 0 <= 12, 100% vanilla-safe)
        if (IsLocalFrameReadyForSend(pSync, currentFrame)) {
            alignas(16) ccNetFrameInputPacket curPkt = {};
            g_fnFreeSyncExtractFramePkt(pSync, &curPkt, currentFrame);
            if (curPkt.frameNumber != 0 && curPkt.checksum != 0) {
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
        if (pkt.frameNumber == 0 || pkt.checksum == 0) {
            return 0;
        }
        uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(outBuf, &pkt);
        return static_cast<uint32_t>(endPtr - outBuf);
    };

    if (windowCount <= 0) {
        pSync->m_rawUdpSendSize = clampedPrefixBytes;
        return auxFramesDelivered;
    }

    // 7. Pre-serialize each frame in windowFrames
    uint8_t  winBlobs[6][32] = {};
    uint32_t winLens[6]      = {};
    int32_t  validWinCount   = 0;
    for (int32_t w = 0; w < windowCount; ++w) {
        const uint32_t len = serializeSingleFrame(windowFrames[w], winBlobs[validWinCount]);
        if (len >= 12) {
            winLens[validWinCount++] = len;
        }
    }

    if (validWinCount <= 0) {
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

    // 9. If bestStart > 0, rewrite the chosen Vanilla-Safe suffix into m_pRawUdpSendBuf.
    //    To prevent Wi-Fi UDP packet-rate doubling and reordering, ONLY send a single-record
    //    auxiliary datagram for (currentFrame - 1) when mainFramesPacked == 1 (i.e. input size
    //    transitioned on currentFrame so 0 redundant frames fit in the main packet) and no NACKs are prefixed.
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

    const uint32_t sessionMemberCount = *reinterpret_cast<const uint32_t*>(pSession + 0x2C);
    if (sessionMemberCount <= static_cast<uint32_t>(battlerCount)) {
        oSendSpectatorPacket(pSync);
        return;
    }

    int32_t  headWatcherMemberId = -1;
    uint64_t headWatcherSteamId  = 0;
    for (int32_t w = 0; w < (8 - battlerCount); ++w) {
        const int32_t candidateId = static_cast<int32_t>(pSync->m_memberIds[battlerCount + w]);
        if (candidateId < 0) continue;
        for (uint32_t i = 0; i < sessionMemberCount && i < 16; ++i) {
            const uint8_t* entry = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
            if (*reinterpret_cast<const int32_t*>(entry + 0x40) == candidateId) {
                headWatcherMemberId = candidateId;
                headWatcherSteamId  = *reinterpret_cast<const uint64_t*>(entry + 0x38);
                break;
            }
        }
        if (headWatcherMemberId != -1) {
            break;
        }
    }

    const int32_t myMemberId = static_cast<int32_t>(pSync->m_memberIds[pSync->m_myPlayerIdx]);
    if (headWatcherMemberId == -1 || headWatcherMemberId == myMemberId || headWatcherSteamId == 0) {
        oSendSpectatorPacket(pSync);
        return;
    }

    // 2. Build ascending sliding window [currentFrame - effectiveDepth + 1 .. currentFrame]
    const int32_t currentFrame = pSync->m_sendFrameCounter;
    if (currentFrame != s_LastPackedSpectatorSendFrame) {
        s_LastPackedSpectatorSendFrame = currentFrame;
        s_SameFrameSpectatorSendCount  = 1;
    } else {
        ++s_SameFrameSpectatorSendCount;
    }

    int32_t configuredDepth = g_Config.enable_packet_redundancy
        ? std::clamp(std::max(g_Config.redundancy_frame_depth, 3), 1, 5)
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
        if (pkt.frameNumber != 0 && pkt.checksum != 0) {
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
        g_fnUdpSocketSend(&pSync->m_udpSocket, &ep, rawBase, totalBytes);
    }
}

} // namespace StormInternal
