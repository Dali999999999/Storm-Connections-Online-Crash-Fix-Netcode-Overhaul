#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <string>

// ============================================================================
// NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS (NSUNSC.exe, ImageBase 0x140000000)
// Reverse-Engineered Netcode Architecture & Exact .pdata-Verified RVAs
// ============================================================================

namespace StormRva {
    // Global singletons in .data
    constexpr uintptr_t kGlobalNetManagerPtr        = 0x21C29C0; // VA 0x1421C29C0 (+0x50 = m_worstRttMs)
    constexpr uintptr_t kGlobalNetFreeBattleSyncPtr = 0x21C4FF0; // VA 0x1421C4FF0 (ccNetFreeBattleSync*)
    constexpr uintptr_t kGlobalSyncCheckHandlerPtr  = 0x21C5000; // VA 0x1421C5000
    constexpr uintptr_t kGlobalTransmittingModalFlg = 0x21C5008; // VA 0x1421C5008
    constexpr uintptr_t kGlobalGameFrameworkPtr     = 0x9709518; // VA 0x149709518 (+0x952 = active battle FPS byte: 60 or 30)

    // Lobby / Session slot layout helper (ccNetLocalInfo::GetMatchSlotArray)
    constexpr uintptr_t kLocalInfo_GetSlotArray     = 0x061DCA0; // VA 0x14061DCA0 (returns int8_t*: [0]=1P, [1]=2P, [3..8]=Spectators)

    // BattleBase synchronization (ccSceneNetworkBattleBase.cpp)
    // NOTE: True .pdata entry is 0x8A1F30 .. 0x8A208D (0x8A1F50 is mid-instruction!)
    constexpr uintptr_t kBattleBase_CtrlBattleSync  = 0x08A1F30; // VA 0x1408A1F30 .. 0x1408A208D
    constexpr uintptr_t kBattleBase_IsCoercionEnd   = 0x08A2E00; // VA 0x1408A2E00 .. 0x1408A2FBA

    // Pre-match RTT negotiation (ccSceneNetworkBattleSyncBase.cpp)
    constexpr uintptr_t kSyncBase_Initialize        = 0x06BA280; // VA 0x1406BA280 .. 0x1406BA6D4
    constexpr uintptr_t kSyncBase_PollSyncState     = 0x06BA6E0; // VA 0x1406BA6E0 .. 0x1406BAB43
    constexpr uintptr_t kSyncBase_IsHost            = 0x06BAB60; // VA 0x1406BAB60 .. 0x1406BABCB
    constexpr uintptr_t kSyncBase_RecvHostWorstRTT  = 0x06BAC60; // VA 0x1406BAC60 .. 0x1406BACEE
    constexpr uintptr_t kSyncBase_RecvClientRTT     = 0x06BACF0; // VA 0x1406BACF0 .. 0x1406BB05C
    constexpr uintptr_t kSyncBase_SendClientRTT     = 0x06BB120; // VA 0x1406BB120 .. 0x1406BB27F
    constexpr uintptr_t kSyncBase_SendHostWorstRTT  = 0x06BB280; // VA 0x1406BB280 .. 0x1406BB360
    constexpr uintptr_t kNetReliableSendPacket      = 0x10B1A10; // VA 0x1410B1A10 .. 0x1410B1A80

    // SteamFriends017 Persona Name functions (fixes 0xC0000005 crash @ RVA 0x010BAD24 & twin @ 0x0847A67)
    constexpr uintptr_t kIat_SteamInternalContextInit = 0x1736490; // VA 0x141736490 (steam_api64!SteamInternal_ContextInit)
    constexpr uintptr_t kData_SteamFriends017Ctx      = 0x2019E58; // VA 0x142019E58 (SteamFriends017 context init data)
    constexpr uintptr_t kSteam_CopyFriendPersonaNameA = 0x10BACF0; // VA 0x1410BACF0 .. 0x1410BAD36 (faults @ +0x34 = 0x10BAD24)
    constexpr uintptr_t kSteam_GetFriendPersonaName   = 0x10BAD40; // VA 0x1410BAD40 .. 0x1410BADE8 (ISteamFriends::GetFriendPersonaName wrapper)
    constexpr uintptr_t kSteam_CopyFriendPersonaNameB = 0x0847A50; // VA 0x140847A50 .. 0x140847A79 (twin SSE over-read @ +0x17 = 0x0847A67)

    // ccNetInputBuffer ring buffer methods (ccNetInputBuffer.cpp)
    constexpr uintptr_t kInputBuf_InsertFrameInput  = 0x10ED140; // VA 0x1410ED140 .. 0x1410ED1F1
    constexpr uintptr_t kInputBuf_Reset             = 0x10ED250; // VA 0x1410ED250 .. 0x1410ED280
    constexpr uintptr_t kInputBuf_ReadFrameInput    = 0x10ED280; // VA 0x1410ED280 .. 0x1410ED327
    constexpr uintptr_t kInputBuf_SeedInitialFrames = 0x10ED330; // VA 0x1410ED330 .. 0x1410ED3B7
    constexpr uintptr_t kInputBuf_GetOrAdvanceSlot  = 0x10ED3C0; // VA 0x1410ED3C0 .. 0x1410ED4C4
    constexpr uintptr_t kInputBuf_AreAllPlayersRcvd = 0x10ED4D0; // VA 0x1410ED4D0 .. 0x1410ED4FA
    constexpr uintptr_t kInputBuf_PackPlayerInput   = 0x10ED6E0; // VA 0x1410ED6E0 .. 0x1410ED754
    constexpr uintptr_t kInputBuf_TryAdvanceSyncFrm = 0x10ED760; // VA 0x1410ED760 .. 0x1410ED793

    // ccNetFreeBattleSync core lockstep methods (ccNetFreeBattleSync.cpp)
    constexpr uintptr_t kFreeSync_PushResendRequest = 0x10F13F0; // VA 0x1410F13F0 .. 0x1410F1492
    constexpr uintptr_t kFreeSync_ParseRcvdPacket   = 0x10F1560; // VA 0x1410F1560 .. 0x1410F16D1
    constexpr uintptr_t kFreeSync_CalcNetFrameByRtt = 0x10F16E0; // VA 0x1410F16E0 .. 0x1410F1786
    constexpr uintptr_t kFreeSync_SerializeSendBuf  = 0x10F1850; // VA 0x1410F1850 .. 0x1410F1912
    constexpr uintptr_t kFreeSync_CtrlFighting      = 0x10F1A80; // VA 0x1410F1A80 .. 0x1410F1BF5
    constexpr uintptr_t kFreeSync_CtrlTimeOut       = 0x10F1CB0; // VA 0x1410F1CB0 .. 0x1410F1DF7
    constexpr uintptr_t kFreeSync_BuildSendPacket   = 0x10F2310; // VA 0x1410F2310 .. 0x1410F25C2
    constexpr uintptr_t kFreeSync_SerializeOneFrame = 0x10F25D0; // VA 0x1410F25D0 .. 0x1410F2731
    constexpr uintptr_t kFreeSync_ReceivePackets        = 0x10F2740; // VA 0x1410F2740 .. 0x1410F2D70
    constexpr uintptr_t kFreeSync_ReceiveWatcherPackets = 0x10F2D80; // VA 0x1410F2D80 .. 0x1410F315E
    constexpr uintptr_t kFreeSync_SetupFrameSync        = 0x10F3640; // VA 0x1410F3640 .. 0x1410F381D
    constexpr uintptr_t kFreeSync_SendBattlerPacket     = 0x10F3840; // VA 0x1410F3840 .. 0x1410F3A46
    constexpr uintptr_t kFreeSync_SendSpectatorPacket   = 0x10F3A50; // VA 0x1410F3A50 .. 0x1410F3CEB
    constexpr uintptr_t kFreeSync_UpdateMemberDelay     = 0x10F40D0; // VA 0x1410F40D0 .. 0x1410F41F8
    constexpr uintptr_t kFreeSync_ExtractFramePkt   = 0x10F4200; // VA 0x1410F4200 .. 0x1410F426F
    constexpr uintptr_t kFreeSync_StartFrameSync    = 0x10F4270; // VA 0x1410F4270 .. 0x1410F42A4

    // ccNetUDPSocket (Steam ISteamNetworking P2P channel 6 wrapper)
    constexpr uintptr_t kUdpSocket_Recv             = 0x1162140; // VA 0x141162140 .. 0x1411623A8
    constexpr uintptr_t kUdpSocket_Send             = 0x11623B0; // VA 0x1411623B0 .. 0x14116241E

    // nuccD3D11Device (DirectX 11 / DXGI presentation & frame-latency pipeline)
    constexpr uintptr_t kNuccD3D11_Present          = 0x1417370; // VA 0x141417370 .. 0x1414173FF (+0x48=ID3D11Device*, +0x50=Context*, +0x60=SwapChain*)

    // Exact instruction RVAs for byte-verified CyberConnect2 engine bug fixes:
    // 1) PushResendRequest null-deref (`movups [0], xmm0` when 16-node resend pool is full)
    constexpr uintptr_t kPatch_PushResendNullDeref  = 0x10F1425; // VA 0x1410F1425 (`48 85 C9 74 28` -> `74 2E`)
    // 2) ReceivePackets inlined NACK resend null-deref (`movups [0], xmm0` when 16-node pool is full)
    constexpr uintptr_t kPatch_RecvResendNullDeref  = 0x10F28F9; // VA 0x1410F28F9 (`48 85 C9 74 27` -> `74 2D`)
    // 3) ReceivePackets broken multi-record checksum lookahead (`[rsp+0x60]+remainBytes` instead of `recvSize-remainBytes`)
    constexpr uintptr_t kPatch_RecvLookaheadBug     = 0x10F299B; // VA 0x1410F299B (`41 83 FF 0C 76 32` -> `EB 32`)
    // 4) CtrlWatcherInputAdvance unsigned underflow (`m_maxWatcherFrame - m_sendFrameCounter > 200` when 0 < 8 -> `jbe` to `jmp`)
    constexpr uintptr_t kPatch_WatcherUnderflowBug  = 0x10F43CF; // VA 0x1410F43CF (`41 81 F9 C8 00 00 00 76 18` -> `EB 18`)
    // 5) CtrlWatcherInputAdvance stall-counter clobber (`mov [rbx+0x48], eax` overwriting m_stallFrameCounter with m_waitCounter)
    constexpr uintptr_t kPatch_WatcherStallClobber  = 0x10F463B; // VA 0x1410F463B (`8B 43 44 89 43 48` -> `8B 43 44 90 90 90`)
    // 6) ccStateMachineCustomMatchLobby::State1_Update unslotted/spectator peer null-deref (0xC0000005 @ +0x113D7F2)
    constexpr uintptr_t kPatch_LobbySlotNullDeref   = 0x113D7E6; // VA 0x14113D7E6 .. 0x14113D7FB (21 bytes)
    constexpr uintptr_t kLobby_NotifyPeer           = 0x1143150; // VA 0x141143150
    constexpr uintptr_t kLobby_State1_Next          = 0x113D7FB; // VA 0x14113D7FB
}

#pragma pack(push, 1)

// 0x20 (32) bytes: Per-player input entry inside a ccNetFrameSlot, and also the
// exact layout of an outbound/inbound unpacked FrameInputPacket in m_pSendFrameBuf.
struct ccNetFrameInputPacket {
    uint8_t  playerIdx;      // +0x00 (0..3)
    uint8_t  optionByte;     // +0x01 (usually 1)
    uint16_t pad02;          // +0x02
    int32_t  frameNumber;    // +0x04 (1..0xFFFF; 0 is reserved for pre-seeded frames)
    uint32_t buttons;        // +0x08 (bitmask)
    int16_t  analogWords[8]; // +0x0C..0x1B (left/right stick decomposed axes)
    uint32_t checksum;       // +0x1C (SyncCheckWriter frame checksum)
};
static_assert(sizeof(ccNetFrameInputPacket) == 0x20, "ccNetFrameInputPacket must be 0x20 bytes");

// 0x20 (32) bytes: Per-player input entry stored inside ccNetFrameSlot (+0x04 + playerIdx*0x20)
struct ccNetSlotPlayerEntry {
    int32_t  bReceived;      // +0x00 (slot + 0x04): 1 when received/valid, 0 when missing
    uint32_t buttons;        // +0x04 (slot + 0x08)
    int16_t  analogWords[8]; // +0x08 (slot + 0x0C..0x1B)
    uint8_t  optionByte;     // +0x18 (slot + 0x1C)
    uint8_t  pad19[3];       // +0x19 (slot + 0x1D..0x1F)
    uint32_t checksum;       // +0x1C (slot + 0x20)
};
static_assert(sizeof(ccNetSlotPlayerEntry) == 0x20, "ccNetSlotPlayerEntry must be 0x20 bytes");

// 0x84 (132) bytes: Single frame slot in ccNetInputBuffer's 12,000-slot ring buffer
struct ccNetFrameSlot {
    int32_t              frameNumber; // +0x00
    ccNetSlotPlayerEntry players[4];  // +0x04..0x83
};
static_assert(sizeof(ccNetFrameSlot) == 0x84, "ccNetFrameSlot must be 0x84 bytes");

// 0x182B94 bytes: ccNetInputBuffer ring buffer (located at g_pNetManager + 0x108, pointed to by pSync + 0x100)
struct ccNetInputBuffer {
    int32_t        m_confirmedAllPlayersFrame; // +0x000000: highest contiguous frame where all players have bReceived == 1
    int32_t        m_currentSyncFrame;         // +0x000004: current simulation read frame (-1 before first step)
    int32_t        m_memberCount;              // +0x000008: active battler count (2 in 1v1)
    ccNetFrameSlot m_slots[0x2EE0];            // +0x00000C: 12,000 frame slots (12000 * 0x84 = 0x182B80 bytes)
    int32_t        m_oldestFrameInRing;        // +0x182B8C: base frame index of the ring window
    int32_t        m_ringHeadSlotIdx;          // +0x182B90: physical slot index corresponding to m_oldestFrameInRing
};
static_assert(offsetof(ccNetInputBuffer, m_oldestFrameInRing) == 0x182B8C, "m_oldestFrameInRing offset mismatch");
static_assert(offsetof(ccNetInputBuffer, m_ringHeadSlotIdx) == 0x182B90, "m_ringHeadSlotIdx offset mismatch");

// 0x10 (16) bytes: Intrusive resend request node in ccNetFreeBattleSync (+0x140 .. +0x23F)
struct ccNetResendNode {
    union {
        struct {
            int32_t frameNumber;          // +0x00: When active in m_pResendQueueHead (+0x250)
            int32_t pad04;                // +0x04
        };
        ccNetResendNode* pNextFree;       // +0x00: When on free list m_pResendFreeList (+0x248)
    };
    ccNetResendNode* pNextQueue;          // +0x08: next node in m_pResendQueueHead
};
static_assert(sizeof(ccNetResendNode) == 0x10, "ccNetResendNode must be 0x10 bytes");

// 0x0C (12) bytes: Endpoint descriptor passed to ccNetUDPSocket::Send (0x1411623B0)
struct ccNetUdpEndpoint {
    uint64_t steamId; // +0x00: remote peer CSteamID
    uint32_t channel; // +0x08: Steam P2P channel (6 for battle frame sync)
};
static_assert(sizeof(ccNetUdpEndpoint) == 0x0C, "ccNetUdpEndpoint must be 0x0C bytes");

// 0x280 (640) bytes: ccNetFreeBattleSync (allocated at 0x1408A2B33, stored at 0x1421C4FF0)
struct ccNetFreeBattleSync {
    int32_t                m_battlerCount;         // +0x000 (2)
    uint32_t               m_maxWatcherFrame;      // +0x004
    uint8_t                pad008[0x20];           // +0x008..0x027
    int32_t                m_extraDelayOffset;     // +0x028 (added to CalcNetFrameByRtt)
    int32_t                m_netDelayFrames;       // +0x02C (active input delay in frames, must be >= 1)
    int32_t                m_myPlayerIdx;          // +0x030 (0 or 1 for battlers, -1 for spectators)
    int32_t                m_myWatcherIdx;         // +0x034
    int8_t                 m_memberIds[8];         // +0x038..0x03F
    int32_t                m_sendFrameCounter;     // +0x040 (local input frame being sampled/sent)
    int32_t                m_waitCounter;          // +0x044 (ticks stalled waiting before NACK; triggers at > 8)
    int32_t                m_stallFrameCounter;    // +0x048 (consecutive stalled frames)
    int32_t                m_bIsStarted;           // +0x04C (1 after StartFrameSync)
    int32_t                m_bIsStalled;           // +0x050 (1 when simulation could not advance this tick)
    int32_t                m_bDisconnectFlag;      // +0x054
    int32_t                m_disconnectMemberIdx;  // +0x058
    int32_t                m_lastPeerMemberId;     // +0x05C
    void*                  m_missingVecAllocVtbl;  // +0x060 (nuccStlAllocator vtable)
    uint8_t                pad068[0x10];           // +0x068..0x077
    int32_t*               m_missingFramesBegin;   // +0x078 (std::vector<int32_t>::_Myfirst)
    int32_t*               m_missingFramesEnd;     // +0x080 (std::vector<int32_t>::_Mylast)
    int32_t*               m_missingFramesCap;     // +0x088 (std::vector<int32_t>::_Myend)
    uint8_t                m_spectatorDeque[0x40]; // +0x090..0x0CF
    uint8_t                m_udpSocket[0x20];      // +0x0D0..0x0EF (ccNetUDPSocket)
    void*                  m_pNetCtrl;             // +0x0F0
    uint32_t               m_netChannelId;         // +0x0F8 (0x1C)
    uint32_t               pad0FC;                 // +0x0FC
    ccNetInputBuffer*      m_pInputBuffer;         // +0x100
    void*                  m_pNetSession;          // +0x108
    ccNetFrameInputPacket* m_pSendFrameBuf;        // +0x110
    uint8_t*               m_pRawUdpSendBuf;       // +0x118 (allocated 0x5DC00 = 384 KB at 60fps)
    uint32_t               m_rawUdpSendSize;       // +0x120
    int32_t                m_coercionReason;       // +0x124 (0=normal, 1/2/3=end match)
    uint8_t                m_resendPoolHdr[0x18];  // +0x128..0x13F
    ccNetResendNode        m_resendNodes[16];      // +0x140..0x23F
    int32_t                m_resendPoolUsed;       // +0x240
    uint32_t               pad244;                 // +0x244
    ccNetResendNode*       m_pResendFreeList;      // +0x248
    ccNetResendNode*       m_pResendQueueHead;     // +0x250
    ccNetResendNode*       m_pResendQueueTail;     // +0x258
    int32_t                m_bSkipStall;           // +0x260
    int32_t                m_memberCount;          // +0x264 (2 in 1v1)
    uint32_t               m_rngSeed;              // +0x268 (0x12345678)
    int32_t                m_worstRttMs;           // +0x26C (agreed RTT in ms)
    void*                  m_pSyncCheckWriter;     // +0x270
    void*                  m_pSyncCheckHandler;    // +0x278
};
static_assert(offsetof(ccNetFreeBattleSync, m_netDelayFrames) == 0x2C, "m_netDelayFrames offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_sendFrameCounter) == 0x40, "m_sendFrameCounter offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_bIsStalled) == 0x50, "m_bIsStalled offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_udpSocket) == 0xD0, "m_udpSocket offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_pInputBuffer) == 0x100, "m_pInputBuffer offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_pNetSession) == 0x108, "m_pNetSession offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_pSendFrameBuf) == 0x110, "m_pSendFrameBuf offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_pRawUdpSendBuf) == 0x118, "m_pRawUdpSendBuf offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_rawUdpSendSize) == 0x120, "m_rawUdpSendSize offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_pResendQueueHead) == 0x250, "m_pResendQueueHead offset mismatch");
static_assert(offsetof(ccNetFreeBattleSync, m_worstRttMs) == 0x26C, "m_worstRttMs offset mismatch");
static_assert(sizeof(ccNetFreeBattleSync) == 0x280, "ccNetFreeBattleSync must be 0x280 bytes");

#pragma pack(pop)

// Configuration loaded from netcode_config.ini
struct NetcodeConfig {
    bool enable_console            = true;
    bool enable_file_log           = true;

    // Online Crash Fix: prevents 0xC0000005 Access Violation at NSUNSC.exe+0x10BAD24 (and +0x847A67)
    // caused by CyberConnect2 executing 32-byte unaligned SSE `movups` reads past the null terminator
    // of SteamFriends017::GetFriendPersonaName() pointers located near a 4KB page boundary (0x...0FE2),
    // plus null-pointer SSE write crashes at +0x10F1455 & +0x10F2928 when the 16-slot resend pool fills.
    bool fix_online_persona_crash  = true;

    // Master switch for the Lockstep Netcode Overhaul hooks
    bool enable_netcode_overhaul   = true;

    // Delay configuration:
    //   delay_mode = 0 : Vanilla delay formula (redundancy + micro-jitter still active)
    //   delay_mode = 1 : Smart Peer-Safe One-Way Delay (DEFAULT: negotiates reduced delay via Host RTT
    //                    and handshake; 100% compatible with both modded & unmodded opponents!)
    //   delay_mode = 2 : Direct Modded-Peer Override (forces formula/floor directly in SetupFrameSync;
    //                    use when both players have the mod with identical settings)
    bool enable_custom_delay   = true;
    int  delay_mode            = 1;
    int  forced_delay_frames   = -1;   // -1 = adaptive One-Way Latency formula; >= 1 = fixed delay frames
    int  min_delay_frames      = 1;    // Protocol mathematical minimum is 1 frame (Frame 0 is reserved)
    int  max_delay_clamp       = 16;   // Allow up to 16 frames on 300ms+ transcontinental pings (causality-safe)
    int  jitter_margin_ms      = 5;    // Tight VSync-phase guard (5ms) + automatic RTT/20 route jitter cushion

    // DirectX 11 Zero-Queue Low-Latency Render & OS Gaming Scheduler Boost
    bool enable_low_latency_render = true;
    int  max_frame_latency         = 1;    // IDXGIDevice1::SetMaximumFrameLatency(1) + GPUThreadPriority(+5)
    bool enable_scheduler_boost    = true; // Win11 EcoQoS/Timer-Coalescing OFF + 1ms timer + MMCSS "Games" thread boost

    // Multi-frame UDP packet redundancy (Vanilla-lookahead-safe packing + single-record auxiliary UDP sends)
    bool enable_packet_redundancy = true;
    int  redundancy_frame_depth   = 3; // Send [frame - 2, frame - 1, frame] per tick (1..8)
    int  max_resend_batch         = 3; // Drain up to 3 NACK resend requests per tick (vanilla drains 1)

    // Micro-jitter absorption, Wi-Fi anti-scan gaming mode & stall governance
    bool enable_wifi_gaming_mode      = true; // Win32 wlanapi MediaStreaming=ON + BackgroundScan=OFF while game runs
    bool enable_dynamic_peer_autotune = true; // Auto +1/+2f rematch cushion if peer exceeds 3% / 18% mid-fight stalls
    bool enable_micro_jitter_poll     = true;
    int  micro_jitter_spin_us         = 1100; // High-precision QPC poll (1100us on tick 0, 650us on tick 1, 25us spacing)
    bool fast_nack_recovery           = true; // Trigger accelerated NACK at 2 stalled frames (33ms) + retry at 7 (116ms)
    bool suppress_short_overlay       = true; // Suppress "Transmitting..." dark modal on brief spikes
    int  overlay_threshold_frames     = 180;  // Show "Transmitting..." modal only after 180 frames (3.0s) instead of 30 (0.5s)
};

struct NetcodeTelemetry {
    std::atomic<int32_t>  lastRawRttMs{0};
    std::atomic<int32_t>  lastNegotiatedRttMs{0};
    std::atomic<int32_t>  lastVanillaDelayFrames{0};
    std::atomic<int32_t>  activeDelayFrames{0};
    std::atomic<int32_t>  dynamicJitterBoostFrames{0};
    std::atomic<bool>     peerHasModHandshake{false};
    std::atomic<bool>     localIsHost{false};
    std::atomic<bool>     wifiGamingModeActive{false};
    std::atomic<bool>     schedulerBoostActive{false};
    std::atomic<uint64_t> packetsBuilt{0};
    std::atomic<uint64_t> redundantFramesSent{0};
    std::atomic<uint64_t> auxDatagramsSent{0};
    std::atomic<uint64_t> multiResendsDrained{0};
    std::atomic<uint64_t> nacksClamped{0};
    std::atomic<uint64_t> jitterPollsRecovered{0};
    std::atomic<uint64_t> fastNacksTriggered{0};
    std::atomic<uint64_t> lockstepStallFrames{0};
    std::atomic<uint64_t> totalBattleFrames{0};
    std::atomic<uint64_t> personaCallsHandled{0};
    std::atomic<uint64_t> personaPageBoundarySaves{0};
    std::atomic<uint64_t> lobbySlotNullDerefSaves{0};
};

extern HMODULE          g_hSelfModule;
extern NetcodeConfig    g_Config;
extern NetcodeTelemetry g_Telemetry;

void LogNetcode(const char* fmt, ...);
void LoadConfig(const std::wstring& configPath);
void WriteDefaultConfigIfMissing(const std::wstring& configPath);
void InitializeNetcodeHooks();
void ShutdownNetcodeHooks();
