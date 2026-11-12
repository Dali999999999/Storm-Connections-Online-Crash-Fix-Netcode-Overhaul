#include "netcode_internal.h"
#include <algorithm>
#include <cstring>

namespace StormInternal {

// ============================================================================
// 0. Online Crash Fix: SteamFriends017 Persona Name Page-Boundary Over-Read
//    Fixes deterministic 0xC0000005 Access Violation at NSUNSC.exe+0x010BAD24
//    (and twin bug at +0x0847A67) where 32-byte SSE `movups [rax + 0x10]` reads
//    past '\0' across a 4KB page boundary (e.g. RAX = 0x...0FE2 -> 0x...1000).
// ============================================================================

namespace {

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

// ============================================================================
// Byte-Verified Engine Bug Patches
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
    },
    // 4) CtrlWatcherInputAdvance Unsigned Underflow Bug (0x1410F43CF):
    //    `cmp r9d, 0xC8; jbe +0x18` -> `cmp r9d, 0xC8; jmp +0x18`
    //    When m_maxWatcherFrame == 0 and m_sendFrameCounter == 8, `0 - 8` unsigned-underflows to 0xFFFFFFF8 (> 200),
    //    adding +8 to m_waitCounter on tick 1 and flooding Battlers with 60 NACKs/sec!
    {
        "ccNetFreeBattleSync::WatcherUnderflow  (+0x10F43CF)",
        StormRva::kPatch_WatcherUnderflowBug,
        { 0x41, 0x81, 0xF9, 0xC8, 0x00, 0x00, 0x00, 0x76 },
        { 0x41, 0x81, 0xF9, 0xC8, 0x00, 0x00, 0x00, 0xEB },
        8,
        false
    },
    // 5) CtrlWatcherInputAdvance Stall-Counter Clobber (0x1410F463B):
    //    `mov eax, [rbx+0x44]; mov [rbx+0x48], eax` -> `mov eax, [rbx+0x44]; nop; nop; nop`
    //    Prevents m_waitCounter (+0x44) from overwriting m_stallFrameCounter (+0x48), which CtrlWatching (0x1410F1E51)
    //    already increments by +1 on stalled frames and zeroes on advancing frames.
    {
        "ccNetFreeBattleSync::WatcherStallFix   (+0x10F463B)",
        StormRva::kPatch_WatcherStallClobber,
        { 0x8B, 0x43, 0x44, 0x89, 0x43, 0x48 },
        { 0x8B, 0x43, 0x44, 0x90, 0x90, 0x90 },
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

// ============================================================================
// Lobby Slot Null Dereference Fix (NSUNSC.exe+0x113D7F2 crash fix)
// CyberConnect2 in ccStateMachineCustomMatchLobby::State1_Update checks rax == NULL
// at 0x113D711, but at 0x113D7F2 executes `mov rdx, [rdi + 0x38]` without testing rdi!
// When an unslotted peer or spectator communicates, FindSlot returns NULL (rdi = 0),
// crashing with 0xC0000005 at [0x38].
// ============================================================================

uint8_t* s_pLobbySlotTrampoline   = nullptr;
bool     s_LobbySlotBridgeApplied = false;

constexpr uint8_t kOrigLobbySlotBridgeBytes[21] = {
    0x48, 0x8B, 0x8E, 0x48, 0x05, 0x00, 0x00, // mov rcx, [rsi + 0x548]
    0x48, 0x85, 0xC9,                         // test rcx, rcx
    0x74, 0x09,                               // je 0x14113D7FB
    0x48, 0x8B, 0x57, 0x38,                   // mov rdx, [rdi + 0x38]
    0xE8, 0x55, 0x59, 0x00, 0x00              // call 0x141143150
};

extern "C" void hk_OnLobbySlotNullDeref(void* pLobbyState, void* pPacket) {
    (void)pLobbyState;
    g_Telemetry.lobbySlotNullDerefSaves.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_logThrottle{0};
    if (s_logThrottle.fetch_add(1, std::memory_order_relaxed) < 10) {
        uint64_t senderSteamId = 0;
        if (pPacket) {
            senderSteamId = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pPacket) + 0x28);
        }
        LogNetcode("[CrashFix] Prevented 0xC0000005 in LobbySlotNotify (+0x113D7F2)! Peer 0x%llX has no combat slot (spectator/transition). Safely skipped.",
                   static_cast<unsigned long long>(senderSteamId));
    }
}

} // namespace

const char* __fastcall hk_SteamGetFriendPersonaName(const uint64_t* pSteamId) {
    const char* rawName = oSteamGetPersonaWrapper
        ? oSteamGetPersonaWrapper(pSteamId)
        : QuerySteamFriendPersonaNameDirect(pSteamId);
    return SanitizeSteamPersonaPointer(rawName, "GetFriendPersonaName(0x10BAD40)");
}

const char* __fastcall hk_SteamCopyFriendPersonaNameA(char* outName33, const uint64_t* pSteamId) {
    const char* rawName = QuerySteamFriendPersonaNameDirect(pSteamId);
    const char* safePtr = SanitizeSteamPersonaPointer(rawName, "CopyFriendPersonaNameA(0x10BACF0)");
    if (outName33) {
        std::memcpy(outName33, safePtr, 32);
        outName33[32] = '\0';
    }
    return safePtr;
}

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

void ApplyCrashAndNetcodeBytePatches(bool applyCrashPatches, bool applyNetcodePatches) noexcept {
    if (applyCrashPatches || applyNetcodePatches) {
        ApplyVerifiedBytePatch(g_EnginePatches[0]); // PushResendRequest null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[1]); // ReceivePackets NACK null-deref fix
        InstallLobbySlotNullFixBridge();            // LobbySlotNullFix bridge (0xC0000005 @ +0x113D7F2)
    }
    if (applyNetcodePatches) {
        ApplyVerifiedBytePatch(g_EnginePatches[2]); // ReceivePackets multi-record lookahead fix
        ApplyVerifiedBytePatch(g_EnginePatches[3]); // CtrlWatcherInputAdvance unsigned underflow fix
        ApplyVerifiedBytePatch(g_EnginePatches[4]); // CtrlWatcherInputAdvance stall-counter clobber fix
    }
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

bool InstallLobbySlotNullFixBridge() {
    if (!g_BaseAddress) return false;
    auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + StormRva::kPatch_LobbySlotNullDeref);

    if (std::memcmp(targetPtr, kOrigLobbySlotBridgeBytes, sizeof(kOrigLobbySlotBridgeBytes)) != 0) {
        if (s_LobbySlotBridgeApplied) return true;
        LogNetcode("[ERROR] Byte mismatch for LobbySlotNullFix bridge @ VA 0x%llX! Skipping.",
                   static_cast<unsigned long long>(g_BaseAddress + StormRva::kPatch_LobbySlotNullDeref));
        return false;
    }

    if (!s_pLobbySlotTrampoline) {
        s_pLobbySlotTrampoline = reinterpret_cast<uint8_t*>(VirtualAlloc(
            nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!s_pLobbySlotTrampoline) {
            LogNetcode("[ERROR] VirtualAlloc failed for LobbySlotNullFix trampoline!");
            return false;
        }
    }

    const uintptr_t vaNotifyPeer  = g_BaseAddress + StormRva::kLobby_NotifyPeer;
    const uintptr_t vaReturnAddr  = g_BaseAddress + StormRva::kLobby_State1_Next;
    const uintptr_t vaNullHandler = reinterpret_cast<uintptr_t>(&hk_OnLobbySlotNullDeref);

    uint8_t* p = s_pLobbySlotTrampoline;
    // 0x00: test rdi, rdi
    *p++ = 0x48; *p++ = 0x85; *p++ = 0xFF;
    // 0x03: je +0x1E -> offset 0x23 (slot_null)
    *p++ = 0x74; *p++ = 0x1E;
    // 0x05: mov rcx, [rsi + 0x548]
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x8E; *p++ = 0x48; *p++ = 0x05; *p++ = 0x00; *p++ = 0x00;
    // 0x0C: test rcx, rcx
    *p++ = 0x48; *p++ = 0x85; *p++ = 0xC9;
    // 0x0F: je +0x2C -> offset 0x3D (done)
    *p++ = 0x74; *p++ = 0x2C;
    // 0x11: mov rdx, [rdi + 0x38]
    *p++ = 0x48; *p++ = 0x8B; *p++ = 0x57; *p++ = 0x38;
    // 0x15: mov rax, vaNotifyPeer
    *p++ = 0x48; *p++ = 0xB8;
    std::memcpy(p, &vaNotifyPeer, 8); p += 8;
    // 0x1F: call rax
    *p++ = 0xFF; *p++ = 0xD0;
    // 0x21: jmp +0x1A -> offset 0x3D (done)
    *p++ = 0xEB; *p++ = 0x1A;

    // 0x23: slot_null:
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xF1;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xEA;
    *p++ = 0x48; *p++ = 0xB8;
    std::memcpy(p, &vaNullHandler, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xD0;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;

    // 0x3D: done:
    *p++ = 0x48; *p++ = 0xB8;
    std::memcpy(p, &vaReturnAddr, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;

    const size_t stubLen = static_cast<size_t>(p - s_pLobbySlotTrampoline);
    FlushInstructionCache(GetCurrentProcess(), s_pLobbySlotTrampoline, stubLen);

    uint8_t bridgeBytes[21];
    bridgeBytes[0] = 0xFF;
    bridgeBytes[1] = 0x25;
    bridgeBytes[2] = 0x00;
    bridgeBytes[3] = 0x00;
    bridgeBytes[4] = 0x00;
    bridgeBytes[5] = 0x00;
    const uintptr_t trampolineVa = reinterpret_cast<uintptr_t>(s_pLobbySlotTrampoline);
    std::memcpy(&bridgeBytes[6], &trampolineVa, 8);
    for (int i = 14; i < 21; ++i) {
        bridgeBytes[i] = 0x90;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(targetPtr, sizeof(bridgeBytes), PAGE_EXECUTE_READWRITE, &oldProt)) {
        LogNetcode("[ERROR] VirtualProtect failed for LobbySlotNullFix bridge @ VA 0x%llX!",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targetPtr)));
        return false;
    }

    std::memcpy(targetPtr, bridgeBytes, sizeof(bridgeBytes));
    FlushInstructionCache(GetCurrentProcess(), targetPtr, sizeof(bridgeBytes));

    DWORD dummyProt = 0;
    VirtualProtect(targetPtr, sizeof(bridgeBytes), oldProt, &dummyProt);
    s_LobbySlotBridgeApplied = true;

    LogNetcode("[Patch] Applied  LobbySlotNullFix Bridge (+0x113D7E6) @ VA 0x%llX -> Trampoline 0x%llX",
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(targetPtr)),
               static_cast<unsigned long long>(trampolineVa));
    return true;
}

void RemoveLobbySlotNullFixBridge() noexcept {
    if (!g_BaseAddress || !s_LobbySlotBridgeApplied) return;
    auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + StormRva::kPatch_LobbySlotNullDeref);

    DWORD oldProt = 0;
    if (VirtualProtect(targetPtr, sizeof(kOrigLobbySlotBridgeBytes), PAGE_EXECUTE_READWRITE, &oldProt)) {
        std::memcpy(targetPtr, kOrigLobbySlotBridgeBytes, sizeof(kOrigLobbySlotBridgeBytes));
        FlushInstructionCache(GetCurrentProcess(), targetPtr, sizeof(kOrigLobbySlotBridgeBytes));
        DWORD dummyProt = 0;
        VirtualProtect(targetPtr, sizeof(kOrigLobbySlotBridgeBytes), oldProt, &dummyProt);
    }
    s_LobbySlotBridgeApplied = false;

    if (s_pLobbySlotTrampoline) {
        VirtualFree(s_pLobbySlotTrampoline, 0, MEM_RELEASE);
        s_pLobbySlotTrampoline = nullptr;
    }
}

} // namespace StormInternal
