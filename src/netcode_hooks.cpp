#include "netcode_mod.h"
#include "MinHook.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wlanapi.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

NetcodeConfig    g_Config;
NetcodeTelemetry g_Telemetry;

namespace {

uintptr_t g_BaseAddress = 0;
LARGE_INTEGER g_QpcFreq = {};

// Handshake magic placed in unused padding bytes [+0x05..+0x07] of 24-byte (0x18) pre-match RTT packets
constexpr uint8_t kHandshakeMagic0 = 0x4E; // 'N'
constexpr uint8_t kHandshakeMagic1 = 0x43; // 'C'

// Maximum 12-byte NACK records allowed in a single outbound UDP packet so we NEVER overflow
// an unmodded opponent's 16-slot m_resendNodes pool (which crashes vanilla NSUNSC.exe @ +0x10F2928).
constexpr uint32_t kMaxOutboundNacksPerTick = 4;

// Exact midpoint RTT values (in ms) that make vanilla CalcNetFrameByRtt(rtt) at 60 FPS
// deterministically output D frames for D in [2..16]:
//   adj = rtt + rtt/4; frames = (adj + 15) / 16 + 1
constexpr int32_t kSyntheticRttForFrames[17] = {
    7,   // 0 (unused, clamped to >= 1)
    7,   // 1 (allocates 2-frame buffer before re-seeding to 1 frame)
    7,   // D = 2 frames (valid range: 1..13 ms)
    20,  // D = 3 frames (valid range: 14..26 ms)
    33,  // D = 4 frames (valid range: 27..39 ms)
    46,  // D = 5 frames (valid range: 40..51 ms)
    58,  // D = 6 frames (valid range: 52..64 ms)
    71,  // D = 7 frames (valid range: 65..77 ms)
    84,  // D = 8 frames (valid range: 78..90 ms)
    97,  // D = 9 frames (valid range: 91..103 ms)
    110, // D = 10 frames (valid range: 104..115 ms)
    122, // D = 11 frames (valid range: 116..128 ms)
    135, // D = 12 frames (valid range: 129..141 ms)
    148, // D = 13 frames (valid range: 142..154 ms)
    161, // D = 14 frames (valid range: 155..167 ms)
    174, // D = 15 frames (valid range: 168..179 ms)
    240  // D = 16 frames (valid range: 180+ ms)
};

// Per-match negotiation & per-frame lockstep pacing state
std::atomic<int32_t>  g_NegotiatedTargetDelay{-1};
std::atomic<int32_t>  g_PeerMinDelayFloor{2};
std::atomic<int32_t>  g_CapturedHostRawRttMs{0};
std::atomic<int32_t>  g_AuthoritativeHostRttMs{0};
std::atomic<bool>     g_HostHandshakeObserved{false};
std::atomic<bool>     g_NegotiationPendingForMatch{false};
std::atomic<uint64_t> g_LastSyncInitTickMs{0};
std::atomic<uint64_t> g_LastNegotiatedPeerSteamId{0};
std::atomic<int32_t>  g_PeerDynamicJitterBoostFrames{0};

uint64_t s_MatchStartStallCount   = 0;
int32_t  s_MatchPeakSyncFrame     = 0;
uint64_t s_MatchStallsAtPeakFrame = 0;

int32_t s_LastPackedSendFrame       = -1;
int32_t s_SameFrameSendCount        = 0;
int32_t s_LastStallConfirmedFrame   = -2;
int32_t s_StalledTicksOnConfFrame   = 0;

void ResetPerMatchPacingState() noexcept {
    s_LastPackedSendFrame     = -1;
    s_SameFrameSendCount      = 0;
    s_LastStallConfirmedFrame = -2;
    s_StalledTicksOnConfFrame = 0;
}

// Game internal helper function typedefs
using fnInputBufReset_t            = void(__fastcall*)(ccNetInputBuffer* pInputBuf);
using fnInputBufSeedFrames_t       = void(__fastcall*)(ccNetInputBuffer* pInputBuf, int32_t frameCount);
using fnInputBufGetOrAdvanceSlot_t = ccNetFrameSlot*(__fastcall*)(ccNetInputBuffer* pInputBuf, int32_t frameNumber);
using fnFreeSyncExtractFramePkt_t  = void(__fastcall*)(ccNetFreeBattleSync* pSync, ccNetFrameInputPacket* outPkt, int32_t frameNumber);
using fnFreeSyncSerializeOneFrm_t  = uint8_t*(__fastcall*)(uint8_t* outRawBuf, const ccNetFrameInputPacket* pPkt);
using fnUdpSocketSend_t            = int32_t(__fastcall*)(void* pUdpSocket, const ccNetUdpEndpoint* pEndpoint, const uint8_t* pData, uint32_t dataSize);
using fnSyncBaseIsHost_t           = uint8_t(__fastcall*)(void* pSyncBase);

fnInputBufReset_t            g_fnInputBufReset            = nullptr;
fnInputBufSeedFrames_t       g_fnInputBufSeedFrames       = nullptr;
fnInputBufGetOrAdvanceSlot_t g_fnInputBufGetOrAdvanceSlot = nullptr;
fnFreeSyncExtractFramePkt_t  g_fnFreeSyncExtractFramePkt  = nullptr;
fnFreeSyncSerializeOneFrm_t  g_fnFreeSyncSerializeOneFrm  = nullptr;
fnUdpSocketSend_t            g_fnUdpSocketSend            = nullptr;
fnSyncBaseIsHost_t           g_fnSyncBaseIsHost           = nullptr;

// Hooked function typedefs
using fnCtrlBattleSync_t          = void(__fastcall*)(void* pBattleBase);
using fnCalcNetFrameByRtt_t       = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t rttMs);
using fnSetupFrameSync_t          = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnUpdateMemberDelay_t       = void(__fastcall*)(ccNetFreeBattleSync* pSync, int32_t memberCount);
using fnBuildSendPacket_t         = int32_t(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnReceivePackets_t          = void(__fastcall*)(ccNetFreeBattleSync* pSync);
using fnSyncBaseInit_t            = void(__fastcall*)(void* pSyncBase);
using fnSyncBaseRecvHostWorstRtt_t= void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnSyncBaseRecvClientRtt_t   = void(__fastcall*)(void* pSyncBase, void* pArg2, uint8_t* pPacket);
using fnNetReliableSendPacket_t   = int32_t(__fastcall*)(void* pNetCtrl, uint8_t* pPacket, uint32_t packetSize, int32_t targetId);

// SteamFriends017 Persona Name typedefs (Online Crash Fix)
using fnSteamContextInit_t          = void**(__fastcall*)(void* pContextInitData);
using fnSteamGetPersonaVtbl_t       = const char*(__fastcall*)(void* pSteamFriends, uint64_t steamId);
using fnSteamCopyFriendPersona32_t  = const char*(__fastcall*)(char* outName33, const uint64_t* pSteamId);
using fnSteamGetFriendPersonaWrap_t = const char*(__fastcall*)(const uint64_t* pSteamId);
using fnNuccD3D11Present_t          = void(__fastcall*)(void* pNuccD3D11);

fnCtrlBattleSync_t           oCtrlBattleSync           = nullptr;
fnCalcNetFrameByRtt_t        oCalcNetFrameByRtt        = nullptr;
fnSetupFrameSync_t           oSetupFrameSync           = nullptr;
fnUpdateMemberDelay_t        oUpdateMemberDelay        = nullptr;
fnBuildSendPacket_t          oBuildSendPacket          = nullptr;
fnReceivePackets_t           oReceivePackets           = nullptr;
fnSyncBaseInit_t             oSyncBaseInit             = nullptr;
fnSyncBaseRecvHostWorstRtt_t oSyncBaseRecvHostWorstRtt = nullptr;
fnSyncBaseRecvClientRtt_t    oSyncBaseRecvClientRtt    = nullptr;
fnNetReliableSendPacket_t    oNetReliableSendPacket    = nullptr;

fnSteamCopyFriendPersona32_t  oSteamCopyPersonaA       = nullptr;
fnSteamCopyFriendPersona32_t  oSteamCopyPersonaB       = nullptr;
fnSteamGetFriendPersonaWrap_t oSteamGetPersonaWrapper  = nullptr;
fnNuccD3D11Present_t          oNuccD3D11Present        = nullptr;
ID3D11Device*                 s_LastConfiguredD3D11Dev = nullptr;

// ============================================================================
// 0. Online Crash Fix: SteamFriends017 Persona Name Page-Boundary Over-Read
//    Fixes deterministic 0xC0000005 Access Violation at NSUNSC.exe+0x010BAD24
//    (and twin bug at +0x0847A67) where 32-byte SSE `movups [rax + 0x10]` reads
//    past '\0' across a 4KB page boundary (e.g. RAX = 0x...0FE2 -> 0x...1000).
// ============================================================================

constexpr size_t kPersonaRingSlots = 64;
constexpr size_t kPersonaSlotBytes = 256;
alignas(64) char s_SafePersonaRing[kPersonaRingSlots][kPersonaSlotBytes] = {};
std::atomic<uint32_t> s_PersonaRingCursor{0};

// Strictly scalar 1-byte copy using `const volatile char*` so MSVC /O2 is forbidden
// from auto-vectorizing into 16-byte SSE loads that could cross an unmapped page.
__declspec(noinline) size_t CopyCStringNoOverread(char* dst, size_t dstCap, const char* src, size_t maxCopyLen) noexcept {
    if (!dst || dstCap == 0) {
        return 0;
    }
    std::memset(dst, 0, dstCap);
    if (!src) {
        return 0;
    }
    const volatile char* vsrc = src;
    const size_t limit = std::min(dstCap - 1, maxCopyLen);
    size_t i = 0;
    for (; i < limit; ++i) {
        const char c = vsrc[i];
        if (c == '\0') {
            break;
        }
        dst[i] = c;
    }
    dst[i] = '\0';
    return i;
}

const char* SanitizeSteamPersonaPointer(const char* rawName, const char* hookTag) {
    g_Telemetry.personaCallsHandled.fetch_add(1, std::memory_order_relaxed);

    const uint32_t slotIdx = s_PersonaRingCursor.fetch_add(1, std::memory_order_relaxed) % kPersonaRingSlots;
    char* safeBuf = s_SafePersonaRing[slotIdx];

    CopyCStringNoOverread(safeBuf, kPersonaSlotBytes, rawName, 127);

    if (rawName) {
        const uintptr_t rawAddr = reinterpret_cast<uintptr_t>(rawName);
        const uintptr_t pageOffset = rawAddr & 0xFFFULL;
        // Vanilla executes two 16-byte `movups` loads ([rax] and [rax+0x10] = 32 bytes).
        // Whenever pageOffset > 0xFE0 (fewer than 32 bytes left in the 4KB page), vanilla
        // crosses into `addr + 0x1000` and crashes if the adjacent page is unmapped!
        if (pageOffset > (0x1000ULL - 32ULL)) {
            const uint64_t saveNum = g_Telemetry.personaPageBoundarySaves.fetch_add(1, std::memory_order_relaxed) + 1;
            if (saveNum <= 8) {
                LogNetcode("[CrashFix] Prevented 0xC0000005 in %s! ptr=0x%llX (pageOffset=0x%03llX, %llu bytes to page end) name=\"%s\"",
                           hookTag,
                           static_cast<unsigned long long>(rawAddr),
                           static_cast<unsigned long long>(pageOffset),
                           static_cast<unsigned long long>(0x1000ULL - pageOffset),
                           safeBuf);
            }
        }
    }

    return safeBuf;
}

const char* QuerySteamFriendPersonaNameDirect(const uint64_t* pSteamId) noexcept {
    if (!pSteamId || !g_BaseAddress) {
        return "";
    }
    auto* pIatContextInit = reinterpret_cast<fnSteamContextInit_t*>(g_BaseAddress + StormRva::kIat_SteamInternalContextInit);
    if (!pIatContextInit || !*pIatContextInit) {
        return "";
    }
    void* pCtxData = reinterpret_cast<void*>(g_BaseAddress + StormRva::kData_SteamFriends017Ctx);
    void** ppFriends = (*pIatContextInit)(pCtxData);
    if (!ppFriends || !*ppFriends) {
        return "";
    }
    void* pFriends = *ppFriends;
    void** vtable = *reinterpret_cast<void***>(pFriends);
    if (!vtable || !vtable[7]) { // ISteamFriends017::GetFriendPersonaName(CSteamID) @ +0x38
        return "";
    }
    auto fnGetFriendPersonaName = reinterpret_cast<fnSteamGetPersonaVtbl_t>(vtable[7]);
    const char* rawName = fnGetFriendPersonaName(pFriends, *pSteamId);
    return rawName ? rawName : "";
}

const char* __fastcall hk_SteamGetFriendPersonaName(const uint64_t* pSteamId) {
    const char* rawName = oSteamGetPersonaWrapper
        ? oSteamGetPersonaWrapper(pSteamId)
        : QuerySteamFriendPersonaNameDirect(pSteamId);
    return SanitizeSteamPersonaPointer(rawName, "GetFriendPersonaName(0x10BAD40)");
}

// Replaces VA 0x1410BACF0 (exact function crashing at VA 0x1410BAD24 / RVA 0x010BAD24)
const char* __fastcall hk_SteamCopyFriendPersonaNameA(char* outName33, const uint64_t* pSteamId) {
    const char* rawName = QuerySteamFriendPersonaNameDirect(pSteamId);
    const char* safePtr = SanitizeSteamPersonaPointer(rawName, "CopyFriendPersonaNameA(0x10BACF0)");
    if (outName33) {
        // Safe 32-byte copy from our 256-byte zero-padded ring slot + guaranteed null terminator at [32]
        std::memcpy(outName33, safePtr, 32);
        outName33[32] = '\0';
    }
    return safePtr;
}

// Replaces VA 0x140847A50 (twin function with identical `movups [rax+0x10]` bug at VA 0x140847A67)
const char* __fastcall hk_SteamCopyFriendPersonaNameB(char* outName33, const uint64_t* pSteamId) {
    const char* rawName = oSteamGetPersonaWrapper
        ? oSteamGetPersonaWrapper(pSteamId)
        : QuerySteamFriendPersonaNameDirect(pSteamId);
    const char* safePtr = SanitizeSteamPersonaPointer(rawName, "CopyFriendPersonaNameB(0x0847A50)");
    if (outName33) {
        std::memcpy(outName33, safePtr, 32);
        outName33[32] = '\0';
    }
    return safePtr;
}

// ============================================================================
// Delay Math & Wire-Protocol Validation Helpers
// ============================================================================

// Compute vanilla CyberConnect2 delay frames (exact match to 0x1410F16E0 / 0x1410F3674)
int32_t ComputeVanillaDelayFrames(int32_t rttMs, int32_t extraOffset) {
    int32_t adj = rttMs + (rttMs / 4);
    int32_t frames = (adj + 15) / 16 + extraOffset + 1;
    return std::clamp(frames, 2, 16);
}

// Compute physically accurate One-Way Latency (OWD = RTT / 2) delay frames at true 60.0 FPS
// (1 frame = 50/3 ms = 16.6667 ms -> ceil(ms / 16.6667) = (ms * 3 + 49) / 50).
// Guarantees >= ~1 full VSync period (16 ms) of post-quantization slack + progressive route-jitter
// scaling (RTT / 12) so even long-haul 140..200 ms matches NEVER suffer sawtooth edge stalls on Match 1!
int32_t ComputeOptimalOneWayDelayFrames(int32_t rttMs, int32_t minFloor, int32_t maxClamp) {
    int32_t safeMin = std::clamp(minFloor, 1, 16);
    int32_t safeMax = std::clamp(maxClamp, safeMin, 16);

    if (g_Config.forced_delay_frames >= 1) {
        return std::clamp(g_Config.forced_delay_frames, safeMin, safeMax);
    }

    const int32_t clampedRtt = std::max(0, rttMs);
    const int32_t owdMs      = (clampedRtt + 1) / 2;

    // Physical causality minimum: remote packets take RTT/2 ms to cross the wire; +6ms VSync-phase guard
    // prevents sub-OWD sawtooth starvation even if a user kept an older netcode_config.ini.
    const int32_t causalFloorFrames = std::clamp(((owdMs + 6) * 3 + 49) / 50, 1, 16);
    const int32_t effectiveMin      = std::max(safeMin, causalFloorFrames);
    const int32_t effectiveMax      = std::max(safeMax, effectiveMin);

    // Phase-safe VSync guard (min 6ms, default 8ms) + progressive long-haul route jitter (~8.3% of RTT):
    //   - 35ms RTT  -> 2f (33ms vs 67ms vanilla, +15.8ms slack)
    //   - 65ms RTT  -> 3f (50ms vs 117ms vanilla, +17.5ms slack)
    //   - 95ms RTT  -> 4f (67ms vs 150ms vanilla, +19.2ms slack)
    //   - 115ms RTT -> 5f (83ms vs 167ms vanilla, +25.8ms slack)
    //   - 143ms RTT -> 6f (100ms vs 217ms vanilla, +28.5ms slack)
    //   - 163ms RTT -> 7f (117ms vs 233ms vanilla, +35.2ms slack)
    const int32_t jitterCushionMs = std::max(6, g_Config.jitter_margin_ms) + (clampedRtt / 12);
    const int32_t oneWayBudgetMs  = owdMs + jitterCushionMs;
    const int32_t targetFrames    = (oneWayBudgetMs * 3 + 49) / 50;

    return std::clamp(targetFrames, effectiveMin, effectiveMax);
}

int32_t ComputeHostAuthoritativeTargetDelay(int32_t rttMs, int32_t minFloor, int32_t maxClamp) {
    int32_t baseTarget = ComputeOptimalOneWayDelayFrames(rttMs, minFloor, maxClamp);
    if (g_Config.enable_dynamic_peer_autotune && g_Config.forced_delay_frames < 1) {
        const int32_t boost = std::clamp(g_PeerDynamicJitterBoostFrames.load(), 0, 2);
        if (boost > 0) {
            const int32_t safeMax = std::clamp(maxClamp, 1, 16);
            baseTarget = std::clamp(baseTarget + boost, 1, std::max(baseTarget, safeMax));
        }
    }
    return baseTarget;
}

// ============================================================================
// 0b. DirectX 11 Zero-Queue Render & Win11 Gaming Scheduler Boost
//     1) Forces IDXGIDevice1::SetMaximumFrameLatency(1) + SetGPUThreadPriority(5) (-33.3 ms local lag).
//     2) Disables Windows 11 EcoQoS & Timer-Coalescing Power Throttling so NSUNSC.exe's Sleep(0)
//        60.00 Hz frame-limiter at 0x1414173D2 never drifts lockstep phase against remote peers.
//     3) Locks 1.0 ms kernel timer resolution (winmm.dll) & registers main thread with MMCSS "Games".
// ============================================================================

struct StormPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

using fnSetProcessInformation_t = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using fnTimeBeginPeriod_t       = UINT(WINAPI*)(UINT);
using fnTimeEndPeriod_t         = UINT(WINAPI*)(UINT);
using fnAvSetMmThreadCharsW_t   = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using fnAvSetMmThreadPriority_t = BOOL(WINAPI*)(HANDLE, int);
using fnAvRevertMmThreadChars_t = BOOL(WINAPI*)(HANDLE);

HMODULE               s_hWinMmModule       = nullptr;
fnTimeEndPeriod_t     s_fnTimeEndPeriod    = nullptr;
bool                  s_TimerPeriodActive  = false;
HMODULE               s_hAvrtModule        = nullptr;
HANDLE                s_hMmcssHandle       = nullptr;
fnAvRevertMmThreadChars_t s_fnAvRevertMm   = nullptr;
std::atomic<bool>     s_MmcssThreadApplied{false};

void ApplyProcessGamingBoost() noexcept {
    if (!g_Config.enable_scheduler_boost) {
        return;
    }

    bool ecoQosDisabled = false;
    if (HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll")) {
        auto fnSetProcInfo = reinterpret_cast<fnSetProcessInformation_t>(
            GetProcAddress(hKernel32, "SetProcessInformation"));
        if (fnSetProcInfo) {
            // ProcessPowerThrottling = 4; ControlMask 0x1 (EXECUTION_SPEED) | 0x4 (IGNORE_TIMER_RESOLUTION)
            StormPowerThrottlingState state = { 1u, 0x1u | 0x4u, 0u };
            if (fnSetProcInfo(GetCurrentProcess(), 4, &state, sizeof(state))) {
                ecoQosDisabled = true;
            } else {
                // Fallback for older Windows 10 builds supporting only EXECUTION_SPEED (0x1)
                state.ControlMask = 0x1u;
                if (fnSetProcInfo(GetCurrentProcess(), 4, &state, sizeof(state))) {
                    ecoQosDisabled = true;
                }
            }
        }
    }

    const DWORD curPrio = GetPriorityClass(GetCurrentProcess());
    bool prioElevated = false;
    if (curPrio == NORMAL_PRIORITY_CLASS || curPrio == BELOW_NORMAL_PRIORITY_CLASS || curPrio == IDLE_PRIORITY_CLASS) {
        prioElevated = (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE);
    }

    if (!s_TimerPeriodActive) {
        s_hWinMmModule = LoadLibraryW(L"winmm.dll");
        if (s_hWinMmModule) {
            auto fnBegin    = reinterpret_cast<fnTimeBeginPeriod_t>(GetProcAddress(s_hWinMmModule, "timeBeginPeriod"));
            s_fnTimeEndPeriod = reinterpret_cast<fnTimeEndPeriod_t>(GetProcAddress(s_hWinMmModule, "timeEndPeriod"));
            if (fnBegin && s_fnTimeEndPeriod && fnBegin(1) == 0) {
                s_TimerPeriodActive = true;
            }
        }
    }

    g_Telemetry.schedulerBoostActive.store(ecoQosDisabled || prioElevated || s_TimerPeriodActive);
    LogNetcode("[Scheduler] Gaming Boost active: EcoQoS/TimerCoalesce=%s | Priority=%s | 1msKernelTimer=%s",
               ecoQosDisabled ? "DISABLED" : "N/A",
               prioElevated ? "ABOVE_NORMAL" : "KEPT",
               s_TimerPeriodActive ? "LOCKED" : "DEFAULT");
}

void ApplyRenderThreadMmcssBoostOnce() noexcept {
    if (!g_Config.enable_scheduler_boost || s_MmcssThreadApplied.exchange(true)) {
        return;
    }

    s_hAvrtModule = LoadLibraryW(L"avrt.dll");
    if (!s_hAvrtModule) {
        return;
    }

    auto fnSetChars = reinterpret_cast<fnAvSetMmThreadCharsW_t>(
        GetProcAddress(s_hAvrtModule, "AvSetMmThreadCharacteristicsW"));
    auto fnSetPrio  = reinterpret_cast<fnAvSetMmThreadPriority_t>(
        GetProcAddress(s_hAvrtModule, "AvSetMmThreadPriority"));
    s_fnAvRevertMm  = reinterpret_cast<fnAvRevertMmThreadChars_t>(
        GetProcAddress(s_hAvrtModule, "AvRevertMmThreadCharacteristics"));

    if (fnSetChars) {
        DWORD taskIndex = 0;
        s_hMmcssHandle = fnSetChars(L"Games", &taskIndex);
        if (s_hMmcssHandle) {
            if (fnSetPrio) {
                fnSetPrio(s_hMmcssHandle, 1); // AVRT_PRIORITY_HIGH = 1
            }
            LogNetcode("[Scheduler] Main D3D11/Simulation thread registered with MMCSS \"Games\" (AVRT_PRIORITY_HIGH).");
        }
    }
}

void ShutdownProcessGamingBoost() noexcept {
    if (s_hMmcssHandle && s_fnAvRevertMm) {
        s_fnAvRevertMm(s_hMmcssHandle);
        s_hMmcssHandle = nullptr;
    }
    if (s_hAvrtModule) {
        FreeLibrary(s_hAvrtModule);
        s_hAvrtModule = nullptr;
    }
    if (s_TimerPeriodActive && s_fnTimeEndPeriod) {
        s_fnTimeEndPeriod(1);
        s_TimerPeriodActive = false;
    }
    if (s_hWinMmModule) {
        FreeLibrary(s_hWinMmModule);
        s_hWinMmModule = nullptr;
    }
    g_Telemetry.schedulerBoostActive.store(false);
}

void __fastcall hk_NuccD3D11Present(void* pNuccD3D11) {
    ApplyRenderThreadMmcssBoostOnce();

    if (g_Config.enable_low_latency_render && pNuccD3D11) {
        const auto* pBytes  = reinterpret_cast<const uint8_t*>(pNuccD3D11);
        auto*       pDevice = *reinterpret_cast<ID3D11Device* const*>(pBytes + 0x48);

        if (pDevice && pDevice != s_LastConfiguredD3D11Dev) {
            s_LastConfiguredD3D11Dev = pDevice;
            IDXGIDevice1* pDXGIDevice1 = nullptr;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&pDXGIDevice1))) &&
                pDXGIDevice1) {
                UINT prevLatency = 3;
                pDXGIDevice1->GetMaximumFrameLatency(&prevLatency);
                if (prevLatency == 0) prevLatency = 3; // DXGI returns 0 for OS default (3 frames)

                const UINT targetLatency = static_cast<UINT>(std::clamp(g_Config.max_frame_latency, 1, 3));
                const HRESULT hr = pDXGIDevice1->SetMaximumFrameLatency(targetLatency);
                const HRESULT hrGpuPrio = pDXGIDevice1->SetGPUThreadPriority(5);

                UINT newLatency = targetLatency;
                pDXGIDevice1->GetMaximumFrameLatency(&newLatency);
                pDXGIDevice1->Release();

                const double savedMs = (prevLatency > newLatency) ? (prevLatency - newLatency) * 16.6667 : 0.0;
                LogNetcode("[LowLatency] DXGI MaxFrameLatency: %u -> %u f (hr=0x%08X, -%0.1f ms lag) | GPUThreadPrio=+5 (hr=0x%08X)",
                           prevLatency, newLatency, static_cast<unsigned int>(hr), savedMs,
                           static_cast<unsigned int>(hrGpuPrio));
            }
        }
    }

    oNuccD3D11Present(pNuccD3D11);
}

// ============================================================================
// 0c. Win32 WLAN Gaming Low-Latency Mode (wlanapi.dll session-scoped handle)
//     Eliminates the periodic 65-second Windows WLAN AutoConfig background SSID channel scan
//     (+25..40 ms radio off-channel stall every ~65s on Wi-Fi NICs like Realtek 8852BE-VT)
//     and activates 802.11 Media Streaming low-latency mode ONLY while NSUNSC.exe is running.
//     Windows kernel automatically restores normal scanning as soon as the handle/process closes.
// ============================================================================

using fnWlanOpenHandle_t     = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
using fnWlanCloseHandle_t    = DWORD(WINAPI*)(HANDLE, PVOID);
using fnWlanEnumInterfaces_t = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
using fnWlanSetInterface_t   = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, const PVOID, PVOID);
using fnWlanQueryInterface_t = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
using fnWlanFreeMemory_t     = VOID(WINAPI*)(PVOID);

HMODULE               s_hWlanApiModule    = nullptr;
HANDLE                s_hWlanClientHandle = nullptr;
fnWlanCloseHandle_t   s_fnWlanCloseHandle = nullptr;
fnWlanEnumInterfaces_t s_fnWlanEnumIfaces = nullptr;
fnWlanSetInterface_t  s_fnWlanSetIface    = nullptr;
fnWlanQueryInterface_t s_fnWlanQueryIface = nullptr;
fnWlanFreeMemory_t    s_fnWlanFreeMemory  = nullptr;

void ApplyWifiGamingLowLatencyMode(bool verboseLog) noexcept {
    if (!g_Config.enable_wifi_gaming_mode) {
        return;
    }

    if (!s_hWlanApiModule) {
        s_hWlanApiModule = LoadLibraryW(L"wlanapi.dll");
        if (!s_hWlanApiModule) {
            return; // System without WLAN service (e.g. Desktop Server / Wine)
        }
        auto fnOpen    = reinterpret_cast<fnWlanOpenHandle_t>(GetProcAddress(s_hWlanApiModule, "WlanOpenHandle"));
        s_fnWlanCloseHandle = reinterpret_cast<fnWlanCloseHandle_t>(GetProcAddress(s_hWlanApiModule, "WlanCloseHandle"));
        s_fnWlanEnumIfaces  = reinterpret_cast<fnWlanEnumInterfaces_t>(GetProcAddress(s_hWlanApiModule, "WlanEnumInterfaces"));
        s_fnWlanSetIface    = reinterpret_cast<fnWlanSetInterface_t>(GetProcAddress(s_hWlanApiModule, "WlanSetInterface"));
        s_fnWlanQueryIface  = reinterpret_cast<fnWlanQueryInterface_t>(GetProcAddress(s_hWlanApiModule, "WlanQueryInterface"));
        s_fnWlanFreeMemory  = reinterpret_cast<fnWlanFreeMemory_t>(GetProcAddress(s_hWlanApiModule, "WlanFreeMemory"));

        if (fnOpen && s_fnWlanCloseHandle && s_fnWlanEnumIfaces && s_fnWlanSetIface && s_fnWlanFreeMemory) {
            DWORD negotiatedVer = 0;
            if (fnOpen(2, nullptr, &negotiatedVer, &s_hWlanClientHandle) != ERROR_SUCCESS) {
                s_hWlanClientHandle = nullptr;
            }
        }
    }

    if (!s_hWlanClientHandle || !s_fnWlanEnumIfaces || !s_fnWlanSetIface || !s_fnWlanFreeMemory) {
        return;
    }

    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    if (s_fnWlanEnumIfaces(s_hWlanClientHandle, nullptr, &pIfList) != ERROR_SUCCESS || !pIfList) {
        return;
    }

    for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& info = pIfList->InterfaceInfo[i];
        if (info.isState != wlan_interface_state_connected) {
            continue;
        }

        BOOL prevBgScan = TRUE;
        BOOL prevMedia  = FALSE;
        if (s_fnWlanQueryIface) {
            DWORD sz = 0;
            PVOID pData = nullptr;
            WLAN_OPCODE_VALUE_TYPE vt = wlan_opcode_value_type_invalid;
            if (s_fnWlanQueryIface(s_hWlanClientHandle, &info.InterfaceGuid, wlan_intf_opcode_background_scan_enabled, nullptr, &sz, &pData, &vt) == ERROR_SUCCESS && pData) {
                prevBgScan = *reinterpret_cast<const BOOL*>(pData);
                s_fnWlanFreeMemory(pData);
            }
            pData = nullptr;
            if (s_fnWlanQueryIface(s_hWlanClientHandle, &info.InterfaceGuid, wlan_intf_opcode_media_streaming_mode, nullptr, &sz, &pData, &vt) == ERROR_SUCCESS && pData) {
                prevMedia = *reinterpret_cast<const BOOL*>(pData);
                s_fnWlanFreeMemory(pData);
            }
        }

        BOOL enableMedia   = TRUE;
        BOOL disableBgScan = FALSE;
        const DWORD rcMedia = s_fnWlanSetIface(s_hWlanClientHandle, &info.InterfaceGuid,
                                               wlan_intf_opcode_media_streaming_mode,
                                               sizeof(enableMedia), &enableMedia, nullptr);
        const DWORD rcScan  = s_fnWlanSetIface(s_hWlanClientHandle, &info.InterfaceGuid,
                                               wlan_intf_opcode_background_scan_enabled,
                                               sizeof(disableBgScan), &disableBgScan, nullptr);

        if (rcMedia == ERROR_SUCCESS || rcScan == ERROR_SUCCESS) {
            g_Telemetry.wifiGamingModeActive.store(true);
            if (verboseLog || prevBgScan != FALSE || prevMedia != TRUE) {
                char adapterDesc[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, info.strInterfaceDescription, -1, adapterDesc, sizeof(adapterDesc) - 1, nullptr, nullptr);
                LogNetcode("[WiFi-AntiLag] Active on \"%s\": BackgroundScan %d->%d (rc=%lu) | MediaStreaming %d->%d (rc=%lu)",
                           adapterDesc,
                           prevBgScan ? 1 : 0, (rcScan == ERROR_SUCCESS) ? 0 : (prevBgScan ? 1 : 0), rcScan,
                           prevMedia ? 1 : 0, (rcMedia == ERROR_SUCCESS) ? 1 : (prevMedia ? 1 : 0), rcMedia);
            }
        }
    }

    s_fnWlanFreeMemory(pIfList);
}

void ShutdownWifiGamingLowLatencyMode() noexcept {
    if (s_hWlanClientHandle && s_fnWlanCloseHandle) {
        s_fnWlanCloseHandle(s_hWlanClientHandle, nullptr);
        s_hWlanClientHandle = nullptr;
    }
    if (s_hWlanApiModule) {
        FreeLibrary(s_hWlanApiModule);
        s_hWlanApiModule = nullptr;
    }
    g_Telemetry.wifiGamingModeActive.store(false);
}

int32_t* GetGlobalNetManagerWorstRttPtr() {
    if (!g_BaseAddress) return nullptr;
    uintptr_t pNetMgr = *reinterpret_cast<uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
    if (!pNetMgr) return nullptr;
    return reinterpret_cast<int32_t*>(pNetMgr + 0x50);
}

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

// ============================================================================
// 1. Pre-Match RTT Negotiation & Peer Handshake Hooks (ccSceneNetworkBattleSyncBase)
// ============================================================================

uint8_t* GetGlobalNetManagerBytes() noexcept {
    if (!g_BaseAddress) return nullptr;
    return *reinterpret_cast<uint8_t* const*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
}

// Reads local member ID from g_pNetManager->m_pLocalInfo(+0x100)->m_memberId(+0x24) (exact match to 0x1406BAB76)
int32_t GetLocalMemberId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return -1;
    const uint8_t* pLocalInfo = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0x100);
    if (!pLocalInfo) return -1;
    return *reinterpret_cast<const int32_t*>(pLocalInfo + 0x24);
}

// Reads authoritative Host member ID from g_pNetManager->m_pSession(+0xF8) entries (+0x13C + i*0x48)
// where entry->isHostFlag(+0x24) != 0 (exact match to 0x1406BAA32..0x1406BAA69 & 0x1406BAB00).
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

// Reads first remote peer's CSteamID from g_pNetManager->m_pSession(+0xF8) to pair Phase 1 & Phase 2 syncs
uint64_t GetRemoteSessionPeerSteamId() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr) return 0;
    const uint8_t* pSession = *reinterpret_cast<const uint8_t* const*>(pNetMgr + 0xF8);
    if (!pSession) return 0;

    const int32_t localId = GetLocalMemberId();
    const int32_t count   = std::clamp(*reinterpret_cast<const int32_t*>(pSession + 0x2C), 0, 16);
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* entry    = pSession + 0x13C + static_cast<size_t>(i) * 0x48;
        const int32_t  memberId = *reinterpret_cast<const int32_t*>(entry + 0x40);
        const uint64_t steamId  = *reinterpret_cast<const uint64_t*>(entry + 0x38);
        if (steamId != 0 && memberId != localId) {
            return steamId;
        }
    }
    return 0;
}

// Exact wrapper around ccSceneNetworkBattleSyncBase::IsHost (VA 0x1406BAB60) with null-safe guard
bool IsLocalSessionHost() noexcept {
    const uint8_t* pNetMgr = GetGlobalNetManagerBytes();
    if (!pNetMgr || *reinterpret_cast<const void* const*>(pNetMgr + 0x100) == nullptr) {
        return false;
    }
    if (g_fnSyncBaseIsHost) {
        return g_fnSyncBaseIsHost(nullptr) != 0;
    }
    const int32_t localId = GetLocalMemberId();
    return (localId >= 0 && localId == GetSessionHostMemberId());
}

// Pre-sends the 24-byte Channel 3 SubCmd 0x09 authoritative Host RTT packet (matching 0x1406BAA06..0x1406BAA8F)
// BEFORE any Host SubCmd 0x10 packet hits the wire, guaranteeing an unmodded Client ALWAYS receives
// SyntheticRTT on Channel 3 before Channel 4's RecvClientRTT (0x1406BB01E) can complete PollSyncState!
void PreSendHostAuthoritativeRttPacket(void* pNetCtrl, int32_t syntheticRtt, int32_t targetDelay, int32_t rawRtt) noexcept {
    if (!pNetCtrl || !oNetReliableSendPacket) {
        return;
    }
    alignas(8) uint8_t hostRttPkt[0x18] = {};
    *reinterpret_cast<uint32_t*>(hostRttPkt + 0x00) = 0x18u;
    hostRttPkt[0x04] = 3u; // Channel 3 (RecvHostWorstRTT)
    hostRttPkt[0x05] = kHandshakeMagic0;
    hostRttPkt[0x06] = kHandshakeMagic1;
    hostRttPkt[0x07] = static_cast<uint8_t>(std::clamp(targetDelay, 1, 16));
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x08) = GetSessionHostMemberId();
    *reinterpret_cast<uint32_t*>(hostRttPkt + 0x0C) = 0x09u; // SubCmd 0x09
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x10) = syntheticRtt;
    *reinterpret_cast<int32_t*>(hostRttPkt + 0x14) = rawRtt;

    oNetReliableSendPacket(pNetCtrl, hostRttPkt, 0x18u, -1);
}

void __fastcall hk_SyncBase_Initialize(void* pSyncBase) {
    ResetPerMatchPacingState();

    oSyncBaseInit(pSyncBase);

    const uint64_t nowTick       = GetTickCount64();
    const uint64_t prevTick      = g_LastSyncInitTickMs.load();
    const uint64_t remoteSteamId = GetRemoteSessionPeerSteamId();
    const uint64_t prevPeerId    = g_LastNegotiatedPeerSteamId.load();
    const bool     isHost        = IsLocalSessionHost();

    // NSUNSC.exe runs TWO ccSceneNetworkBattleSyncBase phases ~3.2s apart before every battle
    // (Phase 1 pre-stage load, Phase 2 right before StartFrameSync). Preserve Phase 1 handshake
    // observations when entering Phase 2 against the same peer, while resetting cleanly on any new match.
    const bool isPhase2SameMatch =
        g_NegotiationPendingForMatch.load() &&
        (nowTick >= prevTick && (nowTick - prevTick) < 15000ULL) &&
        (remoteSteamId != 0 && remoteSteamId == prevPeerId);

    if (!isPhase2SameMatch) {
        // Evaluate previous match mid-fight stall ratio for rematch AutoTune before resetting
        if (remoteSteamId == 0 || remoteSteamId != prevPeerId) {
            g_PeerDynamicJitterBoostFrames.store(0);
            g_Telemetry.dynamicJitterBoostFrames.store(0);
        } else if (g_Config.enable_dynamic_peer_autotune && s_MatchPeakSyncFrame >= 450) {
            const double stallRatio = static_cast<double>(s_MatchStallsAtPeakFrame) /
                                      static_cast<double>(s_MatchPeakSyncFrame);
            if (stallRatio >= 0.030) {
                const int32_t stepBoost = (stallRatio >= 0.180) ? 2 : 1;
                const int32_t prevBoost = g_PeerDynamicJitterBoostFrames.load();
                const int32_t newBoost  = std::clamp(prevBoost + stepBoost, 0, 2);
                if (newBoost != prevBoost) {
                    g_PeerDynamicJitterBoostFrames.store(newBoost);
                    g_Telemetry.dynamicJitterBoostFrames.store(newBoost);
                    LogNetcode("[AutoTune] Rematch vs same peer had %0.2f%% mid-fight stalls (%llu/%d f) -> +%d frame cushion!",
                               stallRatio * 100.0,
                               static_cast<unsigned long long>(s_MatchStallsAtPeakFrame),
                               s_MatchPeakSyncFrame,
                               newBoost);
                }
            }
            s_MatchPeakSyncFrame     = 0;
            s_MatchStallsAtPeakFrame = 0;
        }

        g_NegotiatedTargetDelay.store(-1);
        g_PeerMinDelayFloor.store(2);
        g_AuthoritativeHostRttMs.store(0);
        g_HostHandshakeObserved.store(false);
        g_Telemetry.peerHasModHandshake.store(false);
        ApplyWifiGamingLowLatencyMode(false);
    }

    // Refresh Host raw RTT measurement per phase so Phase 2 captures up-to-date link latency
    g_CapturedHostRawRttMs.store(0);
    g_Telemetry.localIsHost.store(isHost);
    g_LastSyncInitTickMs.store(nowTick);
    g_LastNegotiatedPeerSteamId.store(remoteSteamId);
    g_NegotiationPendingForMatch.store(true);

    if (int32_t* pWorstRtt = GetGlobalNetManagerWorstRttPtr()) {
        const int32_t rawRtt = *pWorstRtt;
        if (rawRtt > 0) {
            g_CapturedHostRawRttMs.store(rawRtt);
            if (isHost || !isPhase2SameMatch || g_Telemetry.lastRawRttMs.load() <= 0) {
                g_Telemetry.lastRawRttMs.store(rawRtt);
            }
        } else if (!isHost && isPhase2SameMatch) {
            const int32_t prevAuthRtt = g_AuthoritativeHostRttMs.load();
            if (prevAuthRtt > 0) {
                *pWorstRtt = prevAuthRtt;
            }
        }
    }
}

void __fastcall hk_SyncBase_RecvClientRTT(void* pSyncBase, void* pArg2, uint8_t* pPacket) {
    const bool isHost = IsLocalSessionHost();
    if (pPacket) {
        const uint32_t subCmd = *reinterpret_cast<const uint32_t*>(pPacket + 0x0C);
        if (subCmd == 0x10) {
            const uint32_t relayedSeed = *reinterpret_cast<const uint32_t*>(pPacket + 0x14);
            const int32_t  senderId    = *reinterpret_cast<const int32_t*>(pPacket + 0x08);
            const int32_t  localId     = GetLocalMemberId();

            // ONLY inspect SubCmd 0x10 handshake & clientRtt when WE are the Host receiving a remote
            // Client's initial report (relayedSeed == 0 && senderId != localId). Never allow an unmodded
            // Host's relay of our own SubCmd 0x10 to trick a modded Client into self-detecting handshake!
            if (isHost && relayedSeed == 0 && (localId < 0 || senderId != localId)) {
                const int32_t clientRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                const int32_t prevRaw   = g_CapturedHostRawRttMs.load();
                if (clientRtt > prevRaw) {
                    g_CapturedHostRawRttMs.store(clientRtt);
                    g_Telemetry.lastRawRttMs.store(clientRtt);
                }

                if (pPacket[5] == kHandshakeMagic0 && pPacket[6] == kHandshakeMagic1) {
                    g_Telemetry.peerHasModHandshake.store(true);
                    const int32_t peerFloor = std::clamp<int32_t>(pPacket[7], 1, 16);
                    g_PeerMinDelayFloor.store(peerFloor);
                    LogNetcode("[Handshake] Remote client has StormNetcode mod (min_floor=%d, clientRTT=%d ms)",
                               peerFloor, clientRtt);
                } else {
                    g_Telemetry.peerHasModHandshake.store(false);
                }
            }
        }
    }

    oSyncBaseRecvClientRtt(pSyncBase, pArg2, pPacket);

    // Keep Host's local m_worstRttMs & pSyncBase->m_worstRttMs (+0x1A0) locked to SyntheticRTT
    // after oSyncBaseRecvClientRtt's `max(m_worstRttMs, clientRtt)` comparison at 0x1406BAF50.
    if (isHost && g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
        const int32_t negDelay = g_NegotiatedTargetDelay.load();
        if (negDelay >= 1) {
            const int32_t syntheticRtt = kSyntheticRttForFrames[std::clamp(negDelay, 2, 16)];
            if (int32_t* pMgrWorstRtt = GetGlobalNetManagerWorstRttPtr()) {
                *pMgrWorstRtt = syntheticRtt;
            }
            if (pSyncBase) {
                *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(pSyncBase) + 0x1A0) = syntheticRtt;
            }
        }
    }
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
                if (rawHostRtt > 0) {
                    g_Telemetry.lastRawRttMs.store(rawHostRtt);
                }
                g_NegotiatedTargetDelay.store(hostTargetDelay);
                LogNetcode("[Handshake] Host has StormNetcode mod! Raw RTT=%d ms -> Agreed Delay=%d frames",
                           rawHostRtt, hostTargetDelay);
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
                // Client -> Host initial RTT report (Channel 4, SubCmd 0x10)
                const int32_t myRawRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                if (myRawRtt > 0) {
                    g_Telemetry.lastRawRttMs.store(myRawRtt);
                }
                pPacket[5] = kHandshakeMagic0;
                pPacket[6] = kHandshakeMagic1;
                pPacket[7] = static_cast<uint8_t>(std::clamp(g_Config.min_delay_frames, 1, 16));
            } else {
                // Host relaying Client's SubCmd 0x10 (0x1406BB003) OR sending Host's own SubCmd 0x10 (0x1406BB1ED):
                // 1) Strip any client handshake bytes so a modded Client never sees its own echo.
                pPacket[5] = 0;
                pPacket[6] = 0;
                pPacket[7] = 0;
                g_Telemetry.localIsHost.store(true);

                // 2) Pre-send authoritative Channel 3 SubCmd 0x09 (SyntheticRTT) BEFORE SubCmd 0x10 hits the wire,
                //    AND clamp SubCmd 0x10's RTT field (+0x10) to SyntheticRTT so unmodded Client's RecvClientRTT
                //    (`if (m_worstRttMs < pktRtt) m_worstRttMs = pktRtt` @ 0x1406BAF50) can NEVER raise m_worstRttMs
                //    back to raw 300ms+ or exit PollSyncState before receiving SyntheticRTT!
                const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
                int32_t rawRtt = g_CapturedHostRawRttMs.load();
                if (currentPktRtt > rawRtt) {
                    rawRtt = currentPktRtt;
                    g_CapturedHostRawRttMs.store(rawRtt);
                }
                if (rawRtt > 0) {
                    g_Telemetry.lastRawRttMs.store(rawRtt);
                }

                if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1 && rawRtt > 0) {
                    const bool peerModded = g_Telemetry.peerHasModHandshake.load();
                    const int32_t minFloor = peerModded
                        ? std::max(1, std::max(g_Config.min_delay_frames, g_PeerMinDelayFloor.load()))
                        : std::max(2, g_Config.min_delay_frames);

                    const int32_t targetDelay  = ComputeHostAuthoritativeTargetDelay(rawRtt, minFloor, g_Config.max_delay_clamp);
                    const int32_t syntheticRtt = kSyntheticRttForFrames[std::clamp(targetDelay, 2, 16)];

                    *reinterpret_cast<int32_t*>(pPacket + 0x10) = syntheticRtt;
                    if (int32_t* pMgrWorstRtt = GetGlobalNetManagerWorstRttPtr()) {
                        *pMgrWorstRtt = syntheticRtt;
                    }

                    g_NegotiatedTargetDelay.store(targetDelay);
                    g_NegotiationPendingForMatch.store(true);
                    g_Telemetry.lastNegotiatedRttMs.store(syntheticRtt);

                    PreSendHostAuthoritativeRttPacket(pNetCtrl, syntheticRtt, targetDelay, rawRtt);
                }
            }
        }
        // Host -> Client Authoritative WorstRTT broadcast (Channel 3, SubCmd 0x09 @ 0x1406BAA8F / 0x1406BB340)
        else if (pktLen == 0x18 && chan == 3 && subCmd == 0x09) {
            g_Telemetry.localIsHost.store(true);
            const int32_t currentPktRtt = *reinterpret_cast<const int32_t*>(pPacket + 0x10);
            int32_t rawRtt = g_CapturedHostRawRttMs.load();
            if (rawRtt <= 0) {
                rawRtt = currentPktRtt;
                g_CapturedHostRawRttMs.store(rawRtt);
            }
            g_Telemetry.lastRawRttMs.store(rawRtt);

            if (g_Config.enable_custom_delay && g_Config.delay_mode >= 1) {
                const bool peerModded = g_Telemetry.peerHasModHandshake.load();
                const int32_t minFloor = peerModded
                    ? std::max(1, std::max(g_Config.min_delay_frames, g_PeerMinDelayFloor.load()))
                    : std::max(2, g_Config.min_delay_frames);

                const int32_t targetDelay  = ComputeHostAuthoritativeTargetDelay(rawRtt, minFloor, g_Config.max_delay_clamp);
                const int32_t syntheticRtt = kSyntheticRttForFrames[std::clamp(targetDelay, 2, 16)];

                // Update both the outgoing packet AND g_pNetManager->m_worstRttMs (+0x50)
                // so both Host and Client (even if unmodded!) compute the exact same frame delay!
                *reinterpret_cast<int32_t*>(pPacket + 0x10) = syntheticRtt;
                if (int32_t* pMgrWorstRtt = GetGlobalNetManagerWorstRttPtr()) {
                    *pMgrWorstRtt = syntheticRtt;
                }

                pPacket[5] = kHandshakeMagic0;
                pPacket[6] = kHandshakeMagic1;
                pPacket[7] = static_cast<uint8_t>(targetDelay);
                *reinterpret_cast<int32_t*>(pPacket + 0x14) = rawRtt;

                g_NegotiatedTargetDelay.store(targetDelay);
                g_NegotiationPendingForMatch.store(true);
                g_Telemetry.lastNegotiatedRttMs.store(syntheticRtt);

                LogNetcode("[RTT-Negotiation] Host authoritative: RawRTT=%d ms (OWD=%d ms, Vanilla=%d f) -> TargetDelay=%d f (%0.1f ms, SyntheticRTT=%d ms, PeerModded=%d)",
                           rawRtt, (rawRtt + 1) / 2, ComputeVanillaDelayFrames(rawRtt, 0),
                           targetDelay, targetDelay * 16.6667, syntheticRtt, peerModded ? 1 : 0);
            }
        }
    }
    return oNetReliableSendPacket(pNetCtrl, pPacket, packetSize, targetId);
}

// ============================================================================
// 2. Input Delay Calculation & Buffer Seeding Hooks (ccNetFreeBattleSync)
// ============================================================================

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
        // Direct Modded-Peer Override mode
        const int32_t minFloor = std::max(1, g_Config.min_delay_frames);
        return std::clamp(ComputeOptimalOneWayDelayFrames(rawRtt, minFloor, g_Config.max_delay_clamp) + extraOffset, 1, 16);
    }

    // delay_mode == 1: Smart Peer-Safe Handshake mode
    const bool isHost = IsLocalSessionHost();
    g_Telemetry.localIsHost.store(isHost);

    if (!isHost) {
        // When local machine is Client: ONLY apply custom OWD target delay if the Host explicitly proved
        // it has StormNetcode via SubCmd 0x09 handshake. Against an unmodded Host, ALWAYS compute the
        // exact vanilla delay from pSync->m_worstRttMs so initial seeded frames match 100% of the time!
        if (g_HostHandshakeObserved.load()) {
            const int32_t negotiated = g_NegotiatedTargetDelay.load();
            if (negotiated >= 1) {
                return std::clamp(negotiated + extraOffset, 1, 16);
            }
        }
        return ComputeVanillaDelayFrames(rttMs, extraOffset);
    }

    // Local machine is Host: use the authoritative target delay broadcast in SubCmd 0x09 & SubCmd 0x10
    const int32_t negotiated = g_NegotiatedTargetDelay.load();
    if (negotiated >= 1) {
        return std::clamp(negotiated + extraOffset, 1, 16);
    }

    const int32_t minFloor = g_Telemetry.peerHasModHandshake.load()
        ? std::max(1, std::max(g_Config.min_delay_frames, g_PeerMinDelayFloor.load()))
        : std::max(2, g_Config.min_delay_frames);
    return std::clamp(ComputeHostAuthoritativeTargetDelay(rawRtt, minFloor, g_Config.max_delay_clamp) + extraOffset, 1, 16);
}

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

    int32_t savedRtt = pSync->m_worstRttMs;
    int32_t targetDelay = ResolveTargetDelayForSync(pSync, savedRtt);
    targetDelay = std::clamp(targetDelay, 1, 16);
    g_NegotiationPendingForMatch.store(false);

    // Ensure vanilla SetupFrameSync allocates m_pSendFrameBuf (+0x110) with at least
    // max(targetDelay, 8) + 2 slots so redundancy & resends always have ample buffer space.
    int32_t allocFrames = std::clamp(std::max(targetDelay, 8), 2, 16);
    pSync->m_worstRttMs = kSyntheticRttForFrames[allocFrames];

    oSetupFrameSync(pSync);

    pSync->m_worstRttMs = savedRtt;

    // Now set the exact target m_netDelayFrames and re-seed m_pInputBuffer (+0x100)
    // using the game's own Reset (0x1410ED250) and SeedInitialFrames (0x1410ED330).
    if (pSync->m_netDelayFrames != targetDelay && pSync->m_pInputBuffer &&
        g_fnInputBufReset && g_fnInputBufSeedFrames) {
        pSync->m_netDelayFrames = targetDelay;
        g_fnInputBufReset(pSync->m_pInputBuffer);
        g_fnInputBufSeedFrames(pSync->m_pInputBuffer, targetDelay);
        pSync->m_sendFrameCounter = pSync->m_pInputBuffer->m_confirmedAllPlayersFrame;
    }

    s_MatchStartStallCount   = g_Telemetry.lockstepStallFrames.load();
    s_MatchPeakSyncFrame     = 0;
    s_MatchStallsAtPeakFrame = 0;

    g_Telemetry.activeDelayFrames.store(pSync->m_netDelayFrames);
    LogNetcode("[StartFrameSync] Match initialized: RawRTT=%d ms | VanillaDelay=%d f | ActiveDelay=%d f (%0.1f ms, AutoBoost=+%d f) | SendCounter=%d",
               g_Telemetry.lastRawRttMs.load(),
               g_Telemetry.lastVanillaDelayFrames.load(),
               pSync->m_netDelayFrames,
               pSync->m_netDelayFrames * 16.6667,
               g_Telemetry.dynamicJitterBoostFrames.load(),
               pSync->m_sendFrameCounter);
}

void __fastcall hk_UpdateMemberCountAndDelay(ccNetFreeBattleSync* pSync, int32_t memberCount) {
    if (!pSync) {
        oUpdateMemberDelay(pSync, memberCount);
        return;
    }

    int32_t savedRtt = pSync->m_worstRttMs;
    int32_t targetDelay = ResolveTargetDelayForSync(pSync, savedRtt);
    targetDelay = std::clamp(targetDelay, 1, 16);

    int32_t allocFrames = std::clamp(std::max(targetDelay, 8), 2, 16);
    pSync->m_worstRttMs = kSyntheticRttForFrames[allocFrames];

    oUpdateMemberDelay(pSync, memberCount);

    pSync->m_worstRttMs = savedRtt;
    pSync->m_netDelayFrames = targetDelay;
    g_Telemetry.activeDelayFrames.store(targetDelay);
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

    // 4. Build ascending sliding window [currentFrame - effectiveDepth + 1 .. currentFrame].
    //    Keep full redundancy window active across the first 6 stalled ticks (100ms) AND pulse every
    //    4th stalled tick (up to 120 ticks / 2.0s) so bidirectional Wi-Fi packet-loss bursts recover
    //    lost pre-stall frames [currentFrame-2, currentFrame-1] immediately without a 300ms NACK RTT!
    const int32_t configuredDepth = g_Config.enable_packet_redundancy
        ? std::clamp(g_Config.redundancy_frame_depth, 1, 6)
        : 1;
    const bool keepBurstWindow = (s_SameFrameSendCount <= 6) ||
                                 (s_SameFrameSendCount <= 120 && (s_SameFrameSendCount % 4) == 0);
    const int32_t effectiveDepth = keepBurstWindow ? configuredDepth : 1;

    int32_t windowFrames[6] = {};
    int32_t windowCount     = 0;
    for (int32_t k = effectiveDepth - 1; k >= 0; --k) {
        const int32_t f = currentFrame - k;
        if (IsLocalFrameReadyForSend(pSync, f)) {
            windowFrames[windowCount++] = f;
        }
    }

    // Serialize helper for a single frame into a 32-byte buffer (12..24 bytes output)
    auto serializeSingleFrame = [&](int32_t frameNum, uint8_t outBuf[32]) -> uint32_t {
        alignas(16) ccNetFrameInputPacket pkt = {};
        g_fnFreeSyncExtractFramePkt(pSync, &pkt, frameNum);
        if (pkt.frameNumber == 0 || pkt.checksum == 0) {
            return 0;
        }
        uint8_t* endPtr = g_fnFreeSyncSerializeOneFrm(outBuf, &pkt);
        return static_cast<uint32_t>(endPtr - outBuf);
    };

    // 5. Transmit any explicit NACK-requested resend frames (that aren't already in windowFrames)
    //    as individual 1-record UDP datagrams (remain == 0 <= 12 -> 100% immune to vanilla's lookahead bug!).
    int32_t auxFramesDelivered = 0;
    for (int32_t r = 0; r < resendCount; ++r) {
        bool inWindow = false;
        for (int32_t w = 0; w < windowCount; ++w) {
            if (windowFrames[w] == resendFrames[r]) {
                inWindow = true;
                break;
            }
        }
        if (inWindow) continue;

        uint8_t singleBuf[32] = {};
        const uint32_t singleLen = serializeSingleFrame(resendFrames[r], singleBuf);
        if (singleLen >= 12) {
            SendAuxiliaryDatagramToBattlers(pSync, singleBuf, singleLen);
            ++auxFramesDelivered;
        }
    }

    if (windowCount <= 0) {
        pSync->m_rawUdpSendSize = clampedPrefixBytes;
        return auxFramesDelivered;
    }

    // 6. Pre-serialize each frame in windowFrames
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

    // 7. Find the longest suffix winBlobs[bestStart .. validWinCount - 1] that, when appended after
    //    clampedPrefixBytes in m_pRawUdpSendBuf, passes IsDatagramSafeForVanillaPeer!
    //    Note: startIdx == (validWinCount - 1) has only ONE FrameInput record at the end of the packet
    //    (remain == 0 <= 12), so it is mathematically guaranteed to succeed 100% of the time!
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

    // 8. If an input-size transition or NACK prefix prevented packing the full window into the main
    //    datagram (bestStart > 0), send the displaced older window frame(s) via a Vanilla-Safe auxiliary
    //    UDP datagram on newly-advanced frames, on burst recovery tick 6, or when matching an explicit resend.
    if (bestStart > 0) {
        bool needDisplacedSend = (s_SameFrameSendCount == 1 || s_SameFrameSendCount == 6);
        if (!needDisplacedSend) {
            for (int32_t r = 0; r < resendCount; ++r) {
                for (int32_t w = 0; w < bestStart; ++w) {
                    if (windowFrames[w] == resendFrames[r]) {
                        needDisplacedSend = true;
                        break;
                    }
                }
            }
        }

        if (needDisplacedSend) {
            // Try combining winBlobs[0 .. bestStart - 1] if they share identical sizes (e.g. [16B, 16B])
            uint8_t combinedAux[192] = {};
            uint32_t combinedLen = 0;
            for (int32_t j = 0; j < bestStart; ++j) {
                std::memcpy(combinedAux + combinedLen, winBlobs[j], winLens[j]);
                combinedLen += winLens[j];
            }
            if (bestStart > 1 && IsDatagramSafeForVanillaPeer(combinedAux, combinedLen)) {
                SendAuxiliaryDatagramToBattlers(pSync, combinedAux, combinedLen);
                auxFramesDelivered += bestStart;
            } else {
                // Send each displaced frame as its own 1-record Vanilla-Safe datagram in ascending order
                for (int32_t j = 0; j < bestStart; ++j) {
                    SendAuxiliaryDatagramToBattlers(pSync, winBlobs[j], winLens[j]);
                    ++auxFramesDelivered;
                }
            }
        }

        // Write the chosen Vanilla-Safe suffix into m_pRawUdpSendBuf
        uint8_t* writePtr = rawBase + clampedPrefixBytes;
        for (int32_t j = bestStart; j < validWinCount; ++j) {
            std::memcpy(writePtr, winBlobs[j], winLens[j]);
            writePtr += winLens[j];
        }
        bestTotalSize = static_cast<uint32_t>(writePtr - rawBase);
    }

    pSync->m_rawUdpSendSize = bestTotalSize;

    const int32_t mainFramesPacked = validWinCount - bestStart;
    const int32_t totalDelivered   = mainFramesPacked + auxFramesDelivered;

    g_Telemetry.packetsBuilt.fetch_add(1, std::memory_order_relaxed);
    if (totalDelivered > 1) {
        g_Telemetry.redundantFramesSent.fetch_add(static_cast<uint64_t>(totalDelivered - 1), std::memory_order_relaxed);
    }

    return mainFramesPacked;
}

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
    // Run on tick 0 (up to 850 us) and tick 1 (up to 450 us) of a stall edge, spaced by ~45 us per poll,
    // catching packets right across 16.67ms VSync boundaries without burning CPU during sustained stalls.
    if (g_Config.enable_micro_jitter_poll &&
        g_Config.micro_jitter_spin_us > 0 &&
        pSync->m_stallFrameCounter <= 1 &&
        g_QpcFreq.QuadPart > 0) {

        const int64_t cfgSpinUs     = std::clamp<int64_t>(g_Config.micro_jitter_spin_us, 50, 1500);
        const int64_t spinUs        = (pSync->m_stallFrameCounter == 0) ? cfgSpinUs : std::min<int64_t>(cfgSpinUs, 450);
        const int64_t pollStepTicks = std::max<int64_t>(1, (g_QpcFreq.QuadPart * 45) / 1000000LL);
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

    // Paced Dual-Shot FastNACK:
    // Fire 1st accelerated NACK at 3 stalled ticks (50ms) and a single backup retry at 9 stalled ticks
    // (150ms) per distinct missing frame in case a Wi-Fi burst dropped the first NACK, then yield to vanilla's
    // normal 9-tick cadence so we NEVER flood or overflow an unmodded peer's 16-slot resend pool.
    if (g_Config.fast_nack_recovery) {
        const int32_t conf = pBuf->m_confirmedAllPlayersFrame;
        if (conf != s_LastStallConfirmedFrame) {
            s_LastStallConfirmedFrame = conf;
            s_StalledTicksOnConfFrame = 1;
        } else {
            ++s_StalledTicksOnConfFrame;
        }

        if ((s_StalledTicksOnConfFrame == 3 || s_StalledTicksOnConfFrame == 9) && pSync->m_waitCounter < 8) {
            pSync->m_waitCounter = 8; // CtrlFighting increments to 9 > 8 -> queues single NACK for (conf + 1)
            g_Telemetry.fastNacksTriggered.fetch_add(1, std::memory_order_relaxed);
        }
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

    // Track real stalled frames for telemetry & govern the 30-frame (0.5s) "Transmitting..." popup timer
    int32_t trueStallTimer = *pModalTimer;
    if (g_Config.suppress_short_overlay) {
        int32_t threshold = std::max(30, g_Config.overlay_threshold_frames);
        // Keep +0x504 below 0x1E (30) until trueStallTimer reaches threshold
        if (trueStallTimer < threshold && trueStallTimer >= 28) {
            *pModalTimer = 28;
        }
    }

    oCtrlBattleSync(pBattleBase);

    // Telemetry periodic report (every 300 battle ticks = 5 seconds at 60 FPS)
    uint64_t totalTicks = g_Telemetry.totalBattleFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    uintptr_t pSyncAddr = g_BaseAddress
        ? *reinterpret_cast<uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetFreeBattleSyncPtr)
        : 0;
    if (pSyncAddr) {
        auto* pSync = reinterpret_cast<ccNetFreeBattleSync*>(pSyncAddr);
        if (pSync->m_bIsStarted && pSync->m_bIsStalled) {
            g_Telemetry.lockstepStallFrames.fetch_add(1, std::memory_order_relaxed);
        }
        if (pSync->m_bIsStarted && pSync->m_pInputBuffer) {
            const int32_t curFrame = pSync->m_pInputBuffer->m_currentSyncFrame;
            if (curFrame > s_MatchPeakSyncFrame) {
                s_MatchPeakSyncFrame     = curFrame;
                s_MatchStallsAtPeakFrame = g_Telemetry.lockstepStallFrames.load(std::memory_order_relaxed) - s_MatchStartStallCount;
            }
        }
        if (pSync->m_bIsStarted && (totalTicks % 300 == 0)) {
            LogNetcode("[Telemetry] Frame=%d | RTT=%d ms | Delay=%d f | Pkts=%llu (Aux=%llu) | RedundantFrms=%llu | JitterSaved=%llu | FastNACKs=%llu | Stalls=%llu | CrashSaves=%llu",
                       pSync->m_pInputBuffer ? pSync->m_pInputBuffer->m_currentSyncFrame : -1,
                       g_Telemetry.lastRawRttMs.load(),
                       pSync->m_netDelayFrames,
                       static_cast<unsigned long long>(g_Telemetry.packetsBuilt.load()),
                       static_cast<unsigned long long>(g_Telemetry.auxDatagramsSent.load()),
                       static_cast<unsigned long long>(g_Telemetry.redundantFramesSent.load()),
                       static_cast<unsigned long long>(g_Telemetry.jitterPollsRecovered.load()),
                       static_cast<unsigned long long>(g_Telemetry.fastNacksTriggered.load()),
                       static_cast<unsigned long long>(g_Telemetry.lockstepStallFrames.load()),
                       static_cast<unsigned long long>(g_Telemetry.personaPageBoundarySaves.load()));
        }
    }
}

// ============================================================================
// Byte-Verified Engine Bug Patches & MinHook Installation
// ============================================================================

struct BytePatchRecord {
    const char* name;
    uintptr_t   rva;
    uint8_t     expected[8];
    uint8_t     patched[8];
    size_t      len;
    bool        applied;
};

BytePatchRecord g_EnginePatches[] = {
    // 1) PushResendRequest (0x1410F1425): `test rcx, rcx; je +0x28 (movups [0], xmm0)` -> `je +0x2E (test rcx, rcx)`
    {
        "ccNetFreeBattleSync::PushResendNullFix (+0x10F1425)",
        StormRva::kPatch_PushResendNullDeref,
        { 0x48, 0x85, 0xC9, 0x74, 0x28 },
        { 0x48, 0x85, 0xC9, 0x74, 0x2E },
        5,
        false
    },
    // 2) ReceivePackets NACK Resend (0x1410F28F9): `test rcx, rcx; je +0x27 (movups [0], xmm0)` -> `je +0x2D (test rcx, rcx)`
    {
        "ccNetFreeBattleSync::RecvResendNullFix (+0x10F28F9)",
        StormRva::kPatch_RecvResendNullDeref,
        { 0x48, 0x85, 0xC9, 0x74, 0x27 },
        { 0x48, 0x85, 0xC9, 0x74, 0x2D },
        5,
        false
    },
    // 3) ReceivePackets Lookahead Bug (0x1410F299B): `cmp r15d, 0xC; jbe +0x32` -> `cmp r15d, 0xC; jmp +0x32`
    //    Fixes CyberConnect2 indexing `[rsp+0x60]+remainBytes` instead of `recvSize-remainBytes` on multi-record packets!
    {
        "ccNetFreeBattleSync::RecvLookaheadFix  (+0x10F299B)",
        StormRva::kPatch_RecvLookaheadBug,
        { 0x41, 0x83, 0xFF, 0x0C, 0x76, 0x32 },
        { 0x41, 0x83, 0xFF, 0x0C, 0xEB, 0x32 },
        6,
        false
    }
};

bool ApplyVerifiedBytePatch(BytePatchRecord& rec) {
    if (!g_BaseAddress) return false;
    auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + rec.rva);

    if (std::memcmp(targetPtr, rec.expected, rec.len) != 0) {
        if (std::memcmp(targetPtr, rec.patched, rec.len) == 0) {
            rec.applied = true;
            return true;
        }
        LogNetcode("[ERROR] Byte mismatch for patch %s @ VA 0x%llX! Skipping.",
                   rec.name, static_cast<unsigned long long>(g_BaseAddress + rec.rva));
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(targetPtr, rec.len, PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogNetcode("[ERROR] VirtualProtect failed for patch %s @ VA 0x%llX!",
                   rec.name, static_cast<unsigned long long>(g_BaseAddress + rec.rva));
        return false;
    }

    std::memcpy(targetPtr, rec.patched, rec.len);
    FlushInstructionCache(GetCurrentProcess(), targetPtr, rec.len);

    DWORD dummyProt = 0;
    VirtualProtect(targetPtr, rec.len, oldProt, &dummyProt);
    rec.applied = true;

    LogNetcode("[Patch] Applied  %-42s @ VA 0x%llX (RVA 0x%07llX)",
               rec.name,
               static_cast<unsigned long long>(g_BaseAddress + rec.rva),
               static_cast<unsigned long long>(rec.rva));
    return true;
}

void RestoreVerifiedBytePatches() noexcept {
    if (!g_BaseAddress) return;
    for (auto& rec : g_EnginePatches) {
        if (!rec.applied) continue;
        auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + rec.rva);
        DWORD oldProt = 0;
        if (VirtualProtect(targetPtr, rec.len, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(targetPtr, rec.expected, rec.len);
            FlushInstructionCache(GetCurrentProcess(), targetPtr, rec.len);
            DWORD dummyProt = 0;
            VirtualProtect(targetPtr, rec.len, oldProt, &dummyProt);
        }
        rec.applied = false;
    }
}

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

void InitializeNetcodeHooks() {
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

    // Apply byte-verified local engine bug fixes
    if (g_Config.fix_online_persona_crash || g_Config.enable_netcode_overhaul) {
        ApplyVerifiedBytePatch(g_EnginePatches[0]); // PushResendRequest null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[1]); // ReceivePackets NACK null-deref fix
    }
    if (g_Config.enable_netcode_overhaul) {
        ApplyVerifiedBytePatch(g_EnginePatches[2]); // ReceivePackets multi-record lookahead fix
    }

    if (MH_Initialize() != MH_OK) {
        LogNetcode("[ERROR] MinHook initialization failed!");
        return;
    }

    // Exact .pdata-verified prologues from NSUNSC.exe
    static constexpr uint8_t kPrologue_CopyPersonaA[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_CopyPersonaB[]     = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };
    static constexpr uint8_t kPrologue_GetPersonaWrap[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x19 };

    static constexpr uint8_t kPrologue_CtrlBattleSync[]   = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x30 };
    static constexpr uint8_t kPrologue_CalcNetFrame[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_SetupFrameSync[]   = { 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_UpdateMemberDly[]  = { 0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x4C, 0x00 };
    static constexpr uint8_t kPrologue_BuildSendPacket[]  = { 0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60 };
    static constexpr uint8_t kPrologue_ReceivePackets[]   = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x55 };
    static constexpr uint8_t kPrologue_SyncBaseInit[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_RecvHostWorstRtt[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_RecvClientRtt[]    = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
    static constexpr uint8_t kPrologue_ReliableSend[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18 };
    static constexpr uint8_t kPrologue_NuccD3D11Present[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9 };

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
            { "Steam::GetFriendPersonaName (SafeRing)",   StormRva::kSteam_GetFriendPersonaName,   kPrologue_GetPersonaWrap, sizeof(kPrologue_GetPersonaWrap), reinterpret_cast<LPVOID>(&hk_SteamGetFriendPersonaName),     reinterpret_cast<LPVOID*>(&oSteamGetPersonaWrapper) },
            { "Steam::CopyFriendPersonaNameA (+0x10BAD24)",StormRva::kSteam_CopyFriendPersonaNameA, kPrologue_CopyPersonaA,   sizeof(kPrologue_CopyPersonaA),   reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameA),  reinterpret_cast<LPVOID*>(&oSteamCopyPersonaA) },
            { "Steam::CopyFriendPersonaNameB (+0x0847A67)",StormRva::kSteam_CopyFriendPersonaNameB, kPrologue_CopyPersonaB,   sizeof(kPrologue_CopyPersonaB),   reinterpret_cast<LPVOID>(&hk_SteamCopyFriendPersonaNameB),  reinterpret_cast<LPVOID*>(&oSteamCopyPersonaB) },
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
            { "BattleBase::CtrlBattleSync",                StormRva::kBattleBase_CtrlBattleSync, kPrologue_CtrlBattleSync,   sizeof(kPrologue_CtrlBattleSync),   reinterpret_cast<LPVOID>(&hk_CtrlBattleSync),            reinterpret_cast<LPVOID*>(&oCtrlBattleSync) },
            { "ccNetFreeBattleSync::CalcNetFrameByRtt",    StormRva::kFreeSync_CalcNetFrameByRtt,kPrologue_CalcNetFrame,     sizeof(kPrologue_CalcNetFrame),     reinterpret_cast<LPVOID>(&hk_CalcNetFrameByRtt),         reinterpret_cast<LPVOID*>(&oCalcNetFrameByRtt) },
            { "ccNetFreeBattleSync::SetupFrameSync",       StormRva::kFreeSync_SetupFrameSync,   kPrologue_SetupFrameSync,   sizeof(kPrologue_SetupFrameSync),   reinterpret_cast<LPVOID>(&hk_SetupFrameSyncAndBuffers),  reinterpret_cast<LPVOID*>(&oSetupFrameSync) },
            { "ccNetFreeBattleSync::UpdateMemberDelay",    StormRva::kFreeSync_UpdateMemberDelay,kPrologue_UpdateMemberDly,  sizeof(kPrologue_UpdateMemberDly),  reinterpret_cast<LPVOID>(&hk_UpdateMemberCountAndDelay), reinterpret_cast<LPVOID*>(&oUpdateMemberDelay) },
            { "ccNetFreeBattleSync::BuildSendPacket",      StormRva::kFreeSync_BuildSendPacket,  kPrologue_BuildSendPacket,  sizeof(kPrologue_BuildSendPacket),  reinterpret_cast<LPVOID>(&hk_BuildBattlerSendPacket),    reinterpret_cast<LPVOID*>(&oBuildSendPacket) },
            { "ccNetFreeBattleSync::ReceivePackets",       StormRva::kFreeSync_ReceivePackets,   kPrologue_ReceivePackets,   sizeof(kPrologue_ReceivePackets),   reinterpret_cast<LPVOID>(&hk_ReceivePackets),            reinterpret_cast<LPVOID*>(&oReceivePackets) },
            { "ccSceneNetworkBattleSyncBase::Initialize",  StormRva::kSyncBase_Initialize,       kPrologue_SyncBaseInit,     sizeof(kPrologue_SyncBaseInit),     reinterpret_cast<LPVOID>(&hk_SyncBase_Initialize),       reinterpret_cast<LPVOID*>(&oSyncBaseInit) },
            { "ccSceneNetworkBattleSyncBase::RecvHostRTT", StormRva::kSyncBase_RecvHostWorstRTT, kPrologue_RecvHostWorstRtt, sizeof(kPrologue_RecvHostWorstRtt), reinterpret_cast<LPVOID>(&hk_SyncBase_RecvHostWorstRTT), reinterpret_cast<LPVOID*>(&oSyncBaseRecvHostWorstRtt) },
            { "ccSceneNetworkBattleSyncBase::RecvPeerRTT", StormRva::kSyncBase_RecvClientRTT,    kPrologue_RecvClientRtt,    sizeof(kPrologue_RecvClientRtt),    reinterpret_cast<LPVOID>(&hk_SyncBase_RecvClientRTT),    reinterpret_cast<LPVOID*>(&oSyncBaseRecvClientRtt) },
            { "ccNetReliableSendPacket",                   StormRva::kNetReliableSendPacket,     kPrologue_ReliableSend,     sizeof(kPrologue_ReliableSend),     reinterpret_cast<LPVOID>(&hk_NetReliableSendPacket),     reinterpret_cast<LPVOID*>(&oNetReliableSendPacket) },
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
    ShutdownWifiGamingLowLatencyMode();
    ShutdownProcessGamingBoost();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    RestoreVerifiedBytePatches();
}
