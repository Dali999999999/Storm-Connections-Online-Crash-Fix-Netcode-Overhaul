#include "netcode_internal.h"
#include <algorithm>
#include <cstring>
#include <intrin.h>

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
    uint8_t     expected[32];
    uint8_t     patched[32];
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
    },
    // 6) nuccFree At-Exit Heap Corruption Fix (0x1412732D8):
    //    `test rax, rax; je +0x17 (call crt_free)` -> `test rax, rax; je +0x20 (safe epilogue)`
    //    Fixes 0xC0000374 STATUS_HEAP_CORRUPTION on exit when NUCC allocator is destroyed before static destructors.
    {
        "nuccFree::AtExitHeapCorruptionFix      (+0x12732D8)",
        StormRva::kPatch_NuccFreeAtExitCrash,
        { 0x48, 0x85, 0xC0, 0x74, 0x17 },
        { 0x48, 0x85, 0xC0, 0x74, 0x20 },
        5,
        false
    },
    // 7-9) ccStateChatWindow Exit Teardown Order Fixes (+0x16A795, +0x16A8CB, +0x16AAE1):
    //      Vanilla unloads "data/network/chatTemplateParam.bin.xfbin" BEFORE calling DestroySingleton (0x140150950),
    //      causing ClearDynamic (0x140150C90 -> +0x150CC8) to dereference freed .xfbin static entry pointers (UAF).
    //      Reorders each 24-byte block so DestroySingleton executes before UnloadFile (matching 0xE8029 & 0xBC734E).
    {
        "ccStateChatWindow::Exit1OrderFix       (+0x016A795)",
        StormRva::kPatch_ChatWindowExit1,
        { 0x48, 0x8B, 0x15, 0xF4, 0xFD, 0xE0, 0x01, 0x48, 0x8D, 0x0D, 0x8D, 0xEB,
          0x59, 0x09, 0xE8, 0x08, 0xA8, 0x12, 0x01, 0xE8, 0xA3, 0x61, 0xFE, 0xFF },
        { 0xE8, 0xB6, 0x61, 0xFE, 0xFF, 0x48, 0x8B, 0x15, 0xEF, 0xFD, 0xE0, 0x01,
          0x48, 0x8D, 0x0D, 0x88, 0xEB, 0x59, 0x09, 0xE8, 0x03, 0xA8, 0x12, 0x01 },
        24,
        false
    },
    {
        "ccStateChatWindow::Exit2OrderFix       (+0x016A8CB)",
        StormRva::kPatch_ChatWindowExit2,
        { 0x48, 0x8B, 0x15, 0xBE, 0xFC, 0xE0, 0x01, 0x48, 0x8D, 0x0D, 0x57, 0xEA,
          0x59, 0x09, 0xE8, 0xD2, 0xA6, 0x12, 0x01, 0xE8, 0x6D, 0x60, 0xFE, 0xFF },
        { 0xE8, 0x80, 0x60, 0xFE, 0xFF, 0x48, 0x8B, 0x15, 0xB9, 0xFC, 0xE0, 0x01,
          0x48, 0x8D, 0x0D, 0x52, 0xEA, 0x59, 0x09, 0xE8, 0xCD, 0xA6, 0x12, 0x01 },
        24,
        false
    },
    {
        "ccStateChatWindow::Exit3OrderFix       (+0x016AAE1)",
        StormRva::kPatch_ChatWindowExit3,
        { 0x48, 0x8B, 0x15, 0xA8, 0xFA, 0xE0, 0x01, 0x48, 0x8D, 0x0D, 0x41, 0xE8,
          0x59, 0x09, 0xE8, 0xBC, 0xA4, 0x12, 0x01, 0xE8, 0x57, 0x5E, 0xFE, 0xFF },
        { 0xE8, 0x6A, 0x5E, 0xFE, 0xFF, 0x48, 0x8B, 0x15, 0xA3, 0xFA, 0xE0, 0x01,
          0x48, 0x8D, 0x0D, 0x3C, 0xE8, 0x59, 0x09, 0xE8, 0xB7, 0xA4, 0x12, 0x01 },
        24,
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
bool     s_LobbyState1SortApplied = false;
uint8_t  s_OrigLobbyState1SortBytes[42] = {};

constexpr uint8_t kExpectedLobbyState1SortPrefix[16] = {
    0x48, 0x8B, 0x9E, 0x00, 0x05, 0x00, 0x00, // mov rbx, [rsi + 0x500]
    0x48, 0x8D, 0x8E, 0xC0, 0x02, 0x00, 0x00, // lea rcx, [rsi + 0x2C0]
    0x8B, 0x96                                // mov edx, [rsi + 0x1AC]
};

// Replaces the broken inlined qsort at +0x113D736..+0x113D797 inside State1_Update with:
//   mov rcx, rsi; call SortSlotsAndRefreshMySlot(0x14113FA50);
//   lea rcx, [rsi + 0x184]; lea rdx, [rbp + 0x28]; call FindSlotBySteamId(0x14113BDC0);
//   mov rdi, rax; test rax, rax; jne 0x14113D797; mov rbx, [rsp + 0x30]; jmp 0x14113D7FB
// This fixes both:
//   1) [rbx + 0x38] NULL crash when [rsi + 0x500] == NULL, and
//   2) Stale `rdi` pointer after qsort swapped the local player's slot into `rdi` (causing the
//      local player to be added to the Custom/Private Lobby multiple times!).
constexpr uint8_t kPatchedLobbyState1SortBytes[42] = {
    0x48, 0x89, 0xF1,                               // mov rcx, rsi
    0xE8, 0x12, 0x23, 0x00, 0x00,                   // call 0x14113FA50 (SortSlotsAndRefreshMySlot)
    0x48, 0x8D, 0x8E, 0x84, 0x01, 0x00, 0x00,       // lea rcx, [rsi + 0x184]
    0x48, 0x8D, 0x55, 0x28,                         // lea rdx, [rbp + 0x28]
    0xE8, 0x72, 0xE6, 0xFF, 0xFF,                   // call 0x14113BDC0 (FindSlotBySteamId)
    0x48, 0x89, 0xC7,                               // mov rdi, rax
    0x48, 0x85, 0xC0,                               // test rax, rax
    0x75, 0x41,                                     // jne 0x14113D797
    0x48, 0x8B, 0x5C, 0x24, 0x30,                   // mov rbx, [rsp + 0x30]
    0xE9, 0x9B, 0x00, 0x00, 0x00                    // jmp 0x14113D7FB
};

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

// Guards KeyboardMap queries (0x14120FF90 & 0x14120FEB0) against NULL/low pointers
// (ccVirtualKeyboard_Win64.cpp passes `0 + 0x60 = 0x60` when m_pKeyboard is NULL)
// and validates the std::_Tree head and input backend chain before entering original code.
bool IsKeyboardKeyMapSafeToQuery(const void* pKeyMap, const char* hookTag) noexcept {
    constexpr uintptr_t kMinValidUserPtr = 0x10000ULL;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(pKeyMap);
    if (addr < kMinValidUserPtr) {
        const uint64_t saveNum = g_Telemetry.virtualKeyboardNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum == 1 || (saveNum % 1800) == 0) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in %s! ptr=0x%llX (m_pKeyboard=NULL / no active keyboard, saves=%llu). Gamepad/mouse preserved.",
                       hookTag,
                       static_cast<unsigned long long>(addr),
                       static_cast<unsigned long long>(saveNum));
        }
        return false;
    }

    const uintptr_t treeHead = *reinterpret_cast<const uintptr_t*>(addr + 0x10);
    if (treeHead < kMinValidUserPtr || !g_BaseAddress) {
        g_Telemetry.virtualKeyboardNullSaves.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const uintptr_t backendRoot = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalInputBackendPtr);
    if (backendRoot < kMinValidUserPtr) {
        return false;
    }
    const uintptr_t backendSub = *reinterpret_cast<const uintptr_t*>(backendRoot + 0x08);
    if (backendSub < kMinValidUserPtr) {
        return false;
    }
    const uintptr_t keyboardBackend = *reinterpret_cast<const uintptr_t*>(backendSub + 0x18);
    if (keyboardBackend < kMinValidUserPtr) {
        return false;
    }
    const uintptr_t kbdVtable = *reinterpret_cast<const uintptr_t*>(keyboardBackend);
    if (kbdVtable < kMinValidUserPtr) {
        return false;
    }

    return true;
}

constexpr uintptr_t kMaxValidUserPtr = 0x00007FFFFFFFFFFFULL;

// Detaches [pVkImpl + 0xC0] (ccSimpleCommand<ccNetIdSearchState>*) if it targets pThis.
// Must run BEFORE closing ccVirtualKeyboard_Win64::Reset so STEP_END cannot invoke the
// callback on a ccNetIdSearchState currently being destroyed.
bool DetachVirtualKeyboardCallbackSeh(void* pThis, void** outVkImpl, void** outCmd, bool* outWasOpen) noexcept {
    *outVkImpl  = nullptr;
    *outCmd     = nullptr;
    *outWasOpen = false;
    if (!g_BaseAddress) {
        return false;
    }
    const uintptr_t targetAddr = reinterpret_cast<uintptr_t>(pThis);
    if (targetAddr < 0x10000ULL || targetAddr > kMaxValidUserPtr) {
        return false;
    }
    __try {
        const uintptr_t pVkWrapper = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalVirtualKeyboardPtr);
        if (pVkWrapper < 0x10000ULL || pVkWrapper > kMaxValidUserPtr) {
            return false;
        }
        const uintptr_t pVkImpl = *reinterpret_cast<const uintptr_t*>(pVkWrapper + 0x18);
        if (pVkImpl < 0x10000ULL || pVkImpl > kMaxValidUserPtr) {
            return false;
        }
        const uintptr_t pCmd = *reinterpret_cast<const uintptr_t*>(pVkImpl + 0xC0);
        if (pCmd < 0x10000ULL || pCmd > kMaxValidUserPtr) {
            return false;
        }
        const uintptr_t cmdVtbl   = *reinterpret_cast<const uintptr_t*>(pCmd + 0x00);
        const uintptr_t cmdTarget = *reinterpret_cast<const uintptr_t*>(pCmd + 0x08);
        if (cmdVtbl != (g_BaseAddress + StormRva::kVtbl_SimpleCmdNetIdSearch) || cmdTarget != targetAddr) {
            return false;
        }
        const bool wasOpen = (*reinterpret_cast<const int32_t*>(pVkImpl + 0x18) == 0);
        *reinterpret_cast<uintptr_t*>(pVkImpl + 0xC0) = 0;
        *outVkImpl  = reinterpret_cast<void*>(pVkImpl);
        *outCmd     = reinterpret_cast<void*>(pCmd);
        *outWasOpen = wasOpen;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CloseVirtualKeyboardImplSeh(void* pVkImpl) noexcept {
    if (!pVkImpl) {
        return;
    }
    __try {
        if (g_fnVirtualKeyboardReset) {
            // [pVkImpl + 0xC0] is already NULL, so Reset safely frees m_buffer and sets state = -1
            // without invoking OnVirtualKeyboardFinish.
            g_fnVirtualKeyboardReset(pVkImpl);
        } else {
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(pVkImpl) + 0x18) = -1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Mirrors ccVirtualKeyboard_Win64::Reset (+0x10C78AE..+0x10C78FD): pushes pVkImpl's NUCC heap,
// calls ccSimpleCommand<ccNetIdSearchState>'s scalar deleting dtor (vtable[0](cmd, 1)), then pops heap.
void DestroyDetachedSimpleCommandSeh(void* pVkImpl, void* pCmd) noexcept {
    if (!pVkImpl || !pCmd) {
        return;
    }
    __try {
        const auto* implVtbl = *reinterpret_cast<uintptr_t* const*>(pVkImpl);
        const auto* cmdVtbl  = *reinterpret_cast<uintptr_t* const*>(pCmd);
        if (!implVtbl || !cmdVtbl) {
            return;
        }
        const auto fnPushHeap = reinterpret_cast<void(__fastcall*)(void*)>(implVtbl[1]);
        const auto fnPopHeap  = reinterpret_cast<void(__fastcall*)(void*)>(implVtbl[2]);
        const auto fnCmdDtor  = reinterpret_cast<void*(__fastcall*)(void*, uint32_t)>(cmdVtbl[0]);
        if (reinterpret_cast<uintptr_t>(fnPushHeap) < 0x10000ULL ||
            reinterpret_cast<uintptr_t>(fnPopHeap)  < 0x10000ULL ||
            reinterpret_cast<uintptr_t>(fnCmdDtor)  < 0x10000ULL) {
            return;
        }
        fnPushHeap(pVkImpl);
        fnCmdDtor(pCmd, 1u);
        fnPopHeap(pVkImpl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Belt-and-suspenders guard for ccNetIdSearchState::OnVirtualKeyboardFinish (+0x170840 -> faults @ +0x170878).
bool ValidateNetIdSearchStateForVkFinishSeh(void* pThis, const char** outReason) noexcept {
    *outReason = nullptr;
    if (!g_BaseAddress) {
        return true;
    }
    const uintptr_t objAddr = reinterpret_cast<uintptr_t>(pThis);
    if (objAddr < 0x10000ULL || objAddr > kMaxValidUserPtr) {
        *outReason = "invalid pThis pointer";
        return false;
    }
    __try {
        const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(objAddr);
        if (vtbl != (g_BaseAddress + StormRva::kVtbl_NetIdSearchState)) {
            *outReason = "freed/reused object (vtable mismatch)";
            return false;
        }
        // Validate embedded std::string m_inputString @ +0x230 (_Ptr/_Buf @ +0x230, _Mysize @ +0x240, _Myres @ +0x248)
        const uint64_t myRes = *reinterpret_cast<const uint64_t*>(objAddr + 0x248);
        if (myRes >= 0x10ULL) {
            if (myRes > 0x100000ULL) {
                *outReason = "corrupt m_inputString._Myres";
                return false;
            }
            const uintptr_t bufPtr = *reinterpret_cast<const uintptr_t*>(objAddr + 0x230);
            if (bufPtr < 0x10000ULL || bufPtr > kMaxValidUserPtr) {
                *outReason = "non-canonical/corrupt m_inputString._Ptr";
                return false;
            }
            const volatile uint8_t probe = *reinterpret_cast<const volatile uint8_t*>(bufPtr);
            (void)probe;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outReason = "SEH access violation probing ccNetIdSearchState";
        return false;
    }
}

// Re-acquires m_pKeyboard ([pVkImpl + 0x38]) if GetKeyboardDevice(0) previously returned NULL
// when ccVirtualKeyboard_Win64::Start ran (otherwise keyboard stays mute until box is reopened).
const void* TryRecoverVirtualKeyboardDeviceSeh() noexcept {
    if (!g_BaseAddress || !g_fnGetKeyboardDevice) {
        return nullptr;
    }
    __try {
        const uintptr_t pInputMgr = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalInputManagerPtr);
        if (pInputMgr < 0x10000ULL || pInputMgr > kMaxValidUserPtr) {
            return nullptr;
        }
        void* pKbd = g_fnGetKeyboardDevice(reinterpret_cast<void*>(pInputMgr), 0);
        const uintptr_t kbdAddr = reinterpret_cast<uintptr_t>(pKbd);
        if (kbdAddr < 0x10000ULL || kbdAddr > kMaxValidUserPtr) {
            return nullptr;
        }
        const uintptr_t pVkWrapper = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalVirtualKeyboardPtr);
        if (pVkWrapper >= 0x10000ULL && pVkWrapper <= kMaxValidUserPtr) {
            const uintptr_t pVkImpl = *reinterpret_cast<const uintptr_t*>(pVkWrapper + 0x18);
            if (pVkImpl >= 0x10000ULL && pVkImpl <= kMaxValidUserPtr &&
                *reinterpret_cast<const uintptr_t*>(pVkImpl + 0x38) == 0) {
                *reinterpret_cast<uintptr_t*>(pVkImpl + 0x38) = kbdAddr;
            }
        }
        return reinterpret_cast<const void*>(kbdAddr + 0x60);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Returns true when ccVirtualKeyboard_Win64 is open (state == 0) in numeric-only mode (mode == 1,
// used by ccNetIdSearchState for Session ID entry).
bool IsVirtualKeyboardNumericModeActiveSeh() noexcept {
    if (!g_BaseAddress) {
        return false;
    }
    __try {
        const uintptr_t pVkWrapper = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalVirtualKeyboardPtr);
        if (pVkWrapper < 0x10000ULL || pVkWrapper > kMaxValidUserPtr) {
            return false;
        }
        const uintptr_t pVkImpl = *reinterpret_cast<const uintptr_t*>(pVkWrapper + 0x18);
        if (pVkImpl < 0x10000ULL || pVkImpl > kMaxValidUserPtr) {
            return false;
        }
        return (*reinterpret_cast<const int32_t*>(pVkImpl + 0x18) == 0) &&
               (*reinterpret_cast<const int32_t*>(pVkImpl + 0x20) == 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Maps USB HID top-row digits (0x1E..0x27) & Numpad digits (0x59..0x62) directly to ASCII '0'..'9',
// fixing French AZERTY Shift+digit and Shifted Numpad producing '!'..')' / '\0' in numeric ID input.
uint8_t MapNumericHidKeyToAsciiDigit(uint32_t hidKey) noexcept {
    if (hidKey >= 0x1Eu && hidKey <= 0x26u) {
        return static_cast<uint8_t>('1' + (hidKey - 0x1Eu));
    }
    if (hidKey == 0x27u) {
        return static_cast<uint8_t>('0');
    }
    if (hidKey >= 0x59u && hidKey <= 0x61u) {
        return static_cast<uint8_t>('1' + (hidKey - 0x59u));
    }
    if (hidKey == 0x62u) {
        return static_cast<uint8_t>('0');
    }
    return 0;
}

// ============================================================================
// Session Abrupt-Disconnect Null-Deref & Custom/Private Lobby Duplicate-Player
// Helpers & State
// ============================================================================

constexpr uintptr_t kMinValidPtr = 0x10000ULL;
thread_local alignas(64) uint8_t s_SafeDummySessionSlot[0x48] = {};
std::atomic<uint64_t> s_LastKnownLocalSteamId{0};
std::atomic<uint64_t> s_LastAssignedMemberSteamId{0};
std::atomic<int32_t>  s_LastAssignedMemberUId{0};
std::atomic<bool>     s_LastAssignedMemberWasDuplicate{false};

// Exact match to NSUNSC.exe!CSteamID::IsValid (0x14113C9E0)
} // namespace

bool IsValidSteamId(uint64_t steamId) noexcept {
    const uint32_t high = static_cast<uint32_t>(steamId >> 32);
    const uint32_t low  = static_cast<uint32_t>(steamId & 0xFFFFFFFFULL);
    const uint32_t acctTypeField = high & 0x00F00000U;
    if (acctTypeField == 0 || acctTypeField >= 0x00B00000U) {
        return false;
    }
    const int32_t universe = static_cast<int32_t>(high) >> 24;
    if (universe < 1 || universe > 4) {
        return false;
    }
    if (acctTypeField == 0x00100000U) {
        return (low != 0 && (high & 0x000FFFFFU) == 1U);
    }
    if (acctTypeField == 0x00700000U) {
        return (low != 0 && (high & 0x000FFFFFU) == 0U);
    }
    if (acctTypeField == 0x00300000U) {
        return (low != 0);
    }
    return true;
}

namespace {

uint8_t* PrepareSafeDummySessionSlot() noexcept {
    std::memset(s_SafeDummySessionSlot, 0, sizeof(s_SafeDummySessionSlot));
    const uint64_t sid = s_LastKnownLocalSteamId.load(std::memory_order_relaxed);
    *reinterpret_cast<uint64_t*>(s_SafeDummySessionSlot + 0x00) = sid;
    *reinterpret_cast<uint64_t*>(s_SafeDummySessionSlot + 0x38) = sid;
    *reinterpret_cast<int32_t*>(s_SafeDummySessionSlot + 0x40)  = 0;
    return s_SafeDummySessionSlot;
}

// Checks whether the instruction at `retAddr` immediately dereferences `[rax + 0x40]` or `[rax + 0x24]`
// without testing `rax` for NULL first (39 out of 51 callers of GetMySlot 0x1410B8E20).
bool CallerImmediatelyDereferencesSlotField(const uint8_t* ret) noexcept {
    if (reinterpret_cast<uintptr_t>(ret) < kMinValidPtr) {
        return false;
    }
    // Direct `mov reg32, [rax + 0x40]` or `mov reg32, [rax + 0x24]` (8B 40/48/50/58/68/70/78 40/24)
    if (ret[0] == 0x8B && (ret[2] == 0x40 || ret[2] == 0x24) && (ret[1] & 0xC7) == 0x40) {
        return true;
    }
    // REX `mov r8d..r15d, [rax + 0x40]` or `[rax + 0x24]` (44 8B xx 40/24)
    if (ret[0] == 0x44 && ret[1] == 0x8B && (ret[3] == 0x40 || ret[3] == 0x24) && (ret[2] & 0xC7) == 0x40) {
        return true;
    }
    // `movzx reg32, byte ptr [rax + 0x40]` (0F B6 xx 40)
    if (ret[0] == 0x0F && ret[1] == 0xB6 && ret[3] == 0x40 && (ret[2] & 0xC7) == 0x40) {
        return true;
    }
    // `mov rbx, rax` (48 8B D8) followed immediately by `mov reg32, [rax/rbx + 0x40/0x24]`
    if (ret[0] == 0x48 && ret[1] == 0x8B && ret[2] == 0xD8 &&
        ret[3] == 0x8B && (ret[5] == 0x40 || ret[5] == 0x24)) {
        return true;
    }
    return false;
}

// Validates a TeamContainer* RB-tree (`[rcx + 0x48]` = `_Myhead`, `[rcx + 0x50]` = `_Mysize`)
// before any node traversal in GetByIndex (+0x094B3E0), FindByCharId (+0x094B830), or FindByIndex (+0x094B8C0).
bool ValidateTeamContainerTree(const void* pContainer, bool requireNonEmpty) noexcept {
    constexpr uintptr_t kMaxUserPtr = 0x00007FFFFFFF0000ULL;
    const uintptr_t base = reinterpret_cast<uintptr_t>(pContainer);
    if (base < kMinValidPtr || base >= kMaxUserPtr || (base & 0x7ULL) != 0) {
        return false;
    }

    __try {
        // Check allocator vtable at +0x30: when ~TeamContainer (0x094AA70/0x094AB70) runs,
        // it frees `_Myhead` and resets `[rcx + 0x30]` to base `nuccStlAllocator` (0x193AE40)
        // WITHOUT zeroing `_Myhead` (+0x48) or `_Mysize` (+0x50)!
        if (g_BaseAddress != 0) {
            const uintptr_t vtbl0  = *reinterpret_cast<const uintptr_t*>(base + 0x00);
            const uintptr_t vtbl30 = *reinterpret_cast<const uintptr_t*>(base + 0x30);
            if (vtbl0 < g_BaseAddress || vtbl30 == (g_BaseAddress + 0x193AE40ULL)) {
                return false;
            }
        }

        const uint64_t mySize = *reinterpret_cast<const uint64_t*>(base + 0x50);
        if (mySize > 64ULL || (requireNonEmpty && mySize == 0ULL)) {
            return false;
        }

        const uintptr_t myHead = *reinterpret_cast<const uintptr_t*>(base + 0x48);
        if (myHead < kMinValidPtr || myHead >= kMaxUserPtr || (myHead & 0x7ULL) != 0) {
            return false;
        }

        // In MSVC std::_Tree, the sentinel head node always has `_Isnil == 1` at +0x19
        const uint8_t headIsNil = *reinterpret_cast<const uint8_t*>(myHead + 0x19);
        if (headIsNil != 1) {
            return false;
        }

        const uintptr_t leftMost = *reinterpret_cast<const uintptr_t*>(myHead + 0x00);
        if (leftMost < kMinValidPtr || leftMost >= kMaxUserPtr || (leftMost & 0x7ULL) != 0) {
            return false;
        }
        if (requireNonEmpty && (leftMost == myHead || *reinterpret_cast<const uint8_t*>(leftMost + 0x19) != 0)) {
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Looks up SteamID (0 if none) for a valid slot matching `uId` in m_slots[0..7] (stride 0x48)
uint64_t FindSlotSteamIdByUId(const uint8_t* pSessionSlots, int32_t uId) noexcept {
    if (uId < 0 || reinterpret_cast<uintptr_t>(pSessionSlots) < kMinValidPtr) {
        return 0ULL;
    }
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* slotI = pSessionSlots + static_cast<size_t>(i) * 0x48;
        if (*reinterpret_cast<const int32_t*>(slotI + 0x40) == uId) {
            const uint64_t sid = *reinterpret_cast<const uint64_t*>(slotI + 0x38);
            if (IsValidSteamId(sid)) {
                return sid;
            }
        }
    }
    return 0ULL;
}

// Finds the canonical slot uId (>= 0) for a given SteamID in m_slots[0..7]
int32_t FindCanonicalUIdBySteamId(const uint8_t* pSessionSlots, uint64_t steamId) noexcept {
    if (!IsValidSteamId(steamId) || reinterpret_cast<uintptr_t>(pSessionSlots) < kMinValidPtr) {
        return -1;
    }
    for (int32_t i = 0; i < 8; ++i) {
        const uint8_t* slotI = pSessionSlots + static_cast<size_t>(i) * 0x48;
        if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == steamId) {
            const int32_t uid = *reinterpret_cast<const int32_t*>(slotI + 0x40);
            if (uid >= 0) {
                return uid;
            }
        }
    }
    return -1;
}

// Tracks recently scrubbed duplicate uIds (so BattleTable rows referencing them are wiped immediately)
// and per-row orphan timers (so stale uIds missing from m_slots for >= 2s are cleared to -1,
// preventing UpdateUserList @ 0x00FF810 from skipping the row and leaving a frozen duplicate UI card).
constexpr size_t kMaxScrubbedOrphanUIds = 16;
SRWLOCK  s_OrphanTrackLock = SRWLOCK_INIT;
int32_t  s_ScrubbedOrphanUIds[kMaxScrubbedOrphanUIds] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
size_t   s_ScrubbedOrphanCursor = 0;
int32_t  s_RowOrphanUId[8]      = { -1,-1,-1,-1,-1,-1,-1,-1 };
uint64_t s_RowOrphanSinceMs[8]  = {};

void RecordScrubbedOrphanUId(int32_t uId) noexcept {
    if (uId < 0) return;
    AcquireSRWLockExclusive(&s_OrphanTrackLock);
    for (int32_t existing : s_ScrubbedOrphanUIds) {
        if (existing == uId) {
            ReleaseSRWLockExclusive(&s_OrphanTrackLock);
            return;
        }
    }
    s_ScrubbedOrphanUIds[s_ScrubbedOrphanCursor % kMaxScrubbedOrphanUIds] = uId;
    ++s_ScrubbedOrphanCursor;
    ReleaseSRWLockExclusive(&s_OrphanTrackLock);
}

bool IsScrubbedOrphanUId(int32_t uId) noexcept {
    if (uId < 0) return false;
    bool found = false;
    AcquireSRWLockShared(&s_OrphanTrackLock);
    for (int32_t existing : s_ScrubbedOrphanUIds) {
        if (existing == uId) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&s_OrphanTrackLock);
    return found;
}

void ClearScrubbedOrphanUIdIfActive(int32_t uId) noexcept {
    if (uId < 0) return;
    AcquireSRWLockExclusive(&s_OrphanTrackLock);
    for (int32_t& existing : s_ScrubbedOrphanUIds) {
        if (existing == uId) {
            existing = -1;
        }
    }
    ReleaseSRWLockExclusive(&s_OrphanTrackLock);
}

// Scrubs duplicate SteamID slots in m_slots[0..7] (stride 0x48, +0x00/+0x38 = steamId, +0x40 = uId).
// Matches OnMemberLeft (0x14113D486..0x14113D55A) & qsort comparator (0x1411407B0):
//   - Empty slots MUST have all 0x48 bytes zeroed (uId == 0, NEVER -1, otherwise qsort sorts -1 to m_slots[0]!),
//   - Valid slots are compacted into m_slots[0 .. validCount - 1] with m_pMySlot ([pSession + 0x500]) updated,
//   - m_memberCount ([pData + 0x2C]) is synchronized to validCount when any duplicate/corrupt slot is scrubbed.
void SanitizeSessionMemberSlots(uint8_t* pObj, bool isSessionObj, const char* contextTag) noexcept {
    if (reinterpret_cast<uintptr_t>(pObj) < kMinValidPtr) {
        return;
    }
    __try {
        uint8_t* const pData     = isSessionObj ? (pObj + 0x184) : pObj;
        uint8_t* const slotsBase = pData + 0x13C;

        uint8_t* pSession = isSessionObj ? pObj : nullptr;
        if (!pSession && g_BaseAddress) {
            const uintptr_t pNetMgr = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
            if (pNetMgr >= kMinValidPtr) {
                const uintptr_t pHolder = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0x88);
                if (pHolder >= kMinValidPtr) {
                    const uintptr_t pSessCand = *reinterpret_cast<const uintptr_t*>(pHolder + 0x20);
                    if (pSessCand >= kMinValidPtr && (pSessCand + 0x184ULL) == reinterpret_cast<uintptr_t>(pData)) {
                        pSession = reinterpret_cast<uint8_t*>(pSessCand);
                    }
                }
            }
        }

        uint8_t* mySlotPtr = pSession ? *reinterpret_cast<uint8_t**>(pSession + 0x500) : nullptr;
        bool     modified  = false;

        for (int32_t i = 0; i < 8; ++i) {
            uint8_t* slotI = slotsBase + static_cast<size_t>(i) * 0x48;
            const uint64_t sidI = *reinterpret_cast<const uint64_t*>(slotI + 0x38);
            if (!IsValidSteamId(sidI)) {
                if (*reinterpret_cast<const int32_t*>(slotI + 0x40) < 0) {
                    std::memset(slotI, 0, 0x48);
                    modified = true;
                }
                continue;
            }
            for (int32_t j = i + 1; j < 8; ++j) {
                uint8_t* slotJ = slotsBase + static_cast<size_t>(j) * 0x48;
                const uint64_t sidJ = *reinterpret_cast<const uint64_t*>(slotJ + 0x38);
                if (sidJ != sidI) {
                    continue;
                }

                // Prefer keeping m_pMySlot data if j is local player; move into slotI so lower slot stays canonical
                const bool jIsMine = (mySlotPtr == slotJ && mySlotPtr != slotI);
                const int32_t uidI = *reinterpret_cast<const int32_t*>(slotI + 0x40);
                const int32_t uidJ = *reinterpret_cast<const int32_t*>(slotJ + 0x40);
                const int32_t keepUId = jIsMine ? uidJ : uidI;
                const int32_t dropUId = jIsMine ? uidI : uidJ;

                if (jIsMine) {
                    std::memcpy(slotI, slotJ, 0x48);
                    mySlotPtr = slotI;
                    if (pSession) {
                        *reinterpret_cast<uint8_t**>(pSession + 0x500) = slotI;
                    }
                }

                if (keepUId <= 0 && dropUId > 0) {
                    *reinterpret_cast<int32_t*>(slotI + 0x40) = dropUId;
                } else if (dropUId > 0 && dropUId != keepUId) {
                    RecordScrubbedOrphanUId(dropUId);
                }
                std::memset(slotJ, 0, 0x48); // Must be 0 (uId == 0), never -1!
                modified = true;

                const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                if (saveNum <= 32) {
                    LogNetcode("[LobbyFix] Scrubbed duplicate m_slots[%d] (kept slot %d, uId=%d, dropped uId=%d) for SteamID 0x%llX in %s!",
                               j, i, *reinterpret_cast<const int32_t*>(slotI + 0x40), dropUId,
                               static_cast<unsigned long long>(sidI), contextTag);
                }
            }
        }

        if (modified) {
            int32_t writeIdx = 0;
            for (int32_t readIdx = 0; readIdx < 8; ++readIdx) {
                uint8_t* srcSlot = slotsBase + static_cast<size_t>(readIdx) * 0x48;
                if (!IsValidSteamId(*reinterpret_cast<const uint64_t*>(srcSlot + 0x38))) {
                    continue;
                }
                if (writeIdx != readIdx) {
                    uint8_t* dstSlot = slotsBase + static_cast<size_t>(writeIdx) * 0x48;
                    std::memcpy(dstSlot, srcSlot, 0x48);
                    if (pSession && *reinterpret_cast<uint8_t**>(pSession + 0x500) == srcSlot) {
                        *reinterpret_cast<uint8_t**>(pSession + 0x500) = dstSlot;
                    }
                }
                ++writeIdx;
            }
            for (int32_t k = writeIdx; k < 8; ++k) {
                std::memset(slotsBase + static_cast<size_t>(k) * 0x48, 0, 0x48);
            }
            *reinterpret_cast<int32_t*>(pData + 0x2C) = writeIdx;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Deduplicates entries in Custom/Endless Lobby BattleTable[0..7] (stride 0x258, +0x00 = int32_t uId, +0x04 = int32_t status)
// by identical uId, by distinct uIds belonging to the same SteamID, AND (when pruneOrphans is true) wipes
// orphan uIds missing from m_slots[0..7] so UpdateUserList (0x00FF810) never skips the row and freezes a ghost clone.
void DeduplicateCustomLobbyBattleTable(
    uint8_t* pBattleTable, int32_t maxEntries, const uint8_t* pSessionSlots, const char* contextTag,
    bool pruneOrphans = false) noexcept {
    if (reinterpret_cast<uintptr_t>(pBattleTable) < kMinValidPtr) {
        return;
    }
    const int32_t limit = std::clamp(maxEntries, 1, 8);
    __try {
        bool hasAnyValidSessionSlot = false;
        if (reinterpret_cast<uintptr_t>(pSessionSlots) >= kMinValidPtr) {
            for (int32_t s = 0; s < 8; ++s) {
                const uint8_t* slotS = pSessionSlots + static_cast<size_t>(s) * 0x48;
                if (IsValidSteamId(*reinterpret_cast<const uint64_t*>(slotS + 0x38)) &&
                    *reinterpret_cast<const int32_t*>(slotS + 0x40) >= 0) {
                    hasAnyValidSessionSlot = true;
                    ClearScrubbedOrphanUIdIfActive(*reinterpret_cast<const int32_t*>(slotS + 0x40));
                }
            }
        }

        // Pass 1: Clear orphan BattleTable rows (uId not found in m_slots[0..7]) when pruneOrphans is enabled.
        // Immediate wipe if uId was a known scrubbed duplicate; 2000ms grace period for unknown uIds (late NewMember).
        if (pruneOrphans && hasAnyValidSessionSlot) {
            const uint64_t nowMs = GetTickCount64();
            for (int32_t i = 0; i < limit; ++i) {
                uint8_t* entryI = pBattleTable + static_cast<size_t>(i) * 0x258;
                const int32_t uidI    = *reinterpret_cast<const int32_t*>(entryI + 0x00);
                const int32_t statusI = *reinterpret_cast<const int32_t*>(entryI + 0x04);
                if (uidI < 0 || statusI < 0) {
                    AcquireSRWLockExclusive(&s_OrphanTrackLock);
                    s_RowOrphanUId[i]     = -1;
                    s_RowOrphanSinceMs[i] = 0;
                    ReleaseSRWLockExclusive(&s_OrphanTrackLock);
                    continue;
                }
                const uint64_t sidI = FindSlotSteamIdByUId(pSessionSlots, uidI);
                if (IsValidSteamId(sidI)) {
                    AcquireSRWLockExclusive(&s_OrphanTrackLock);
                    s_RowOrphanUId[i]     = -1;
                    s_RowOrphanSinceMs[i] = 0;
                    ReleaseSRWLockExclusive(&s_OrphanTrackLock);
                    continue;
                }

                bool shouldWipe = IsScrubbedOrphanUId(uidI);
                if (!shouldWipe) {
                    AcquireSRWLockExclusive(&s_OrphanTrackLock);
                    if (s_RowOrphanUId[i] != uidI) {
                        s_RowOrphanUId[i]     = uidI;
                        s_RowOrphanSinceMs[i] = nowMs;
                    } else if (nowMs >= s_RowOrphanSinceMs[i] && (nowMs - s_RowOrphanSinceMs[i]) >= 2000ULL) {
                        shouldWipe            = true;
                        s_RowOrphanUId[i]     = -1;
                        s_RowOrphanSinceMs[i] = 0;
                    }
                    ReleaseSRWLockExclusive(&s_OrphanTrackLock);
                }

                if (shouldWipe) {
                    *reinterpret_cast<int32_t*>(entryI + 0x00) = -1;
                    *reinterpret_cast<int32_t*>(entryI + 0x04) = -1;
                    const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (saveNum <= 32) {
                        LogNetcode("[LobbyFix] Wiped orphan BattleTable[%d] (uId=%d absent from m_slots) in %s -> ghost UI card cleared!",
                                   i, uidI, contextTag);
                    }
                }
            }
        }

        // Pass 2: Deduplicate rows with identical uId or distinct uIds pointing to the same SteamID.
        for (int32_t i = 0; i < limit; ++i) {
            uint8_t* entryI = pBattleTable + static_cast<size_t>(i) * 0x258;
            int32_t uidI    = *reinterpret_cast<const int32_t*>(entryI + 0x00);
            int32_t statusI = *reinterpret_cast<const int32_t*>(entryI + 0x04);
            if (uidI < 0 || statusI < 0) {
                continue;
            }
            const uint64_t sidI = FindSlotSteamIdByUId(pSessionSlots, uidI);

            for (int32_t j = i + 1; j < limit; ++j) {
                uint8_t* entryJ = pBattleTable + static_cast<size_t>(j) * 0x258;
                const int32_t uidJ    = *reinterpret_cast<const int32_t*>(entryJ + 0x00);
                const int32_t statusJ = *reinterpret_cast<const int32_t*>(entryJ + 0x04);
                if (uidJ < 0 || statusJ < 0) {
                    continue;
                }

                const bool sameUId = (uidJ == uidI);
                const uint64_t sidJ = (!sameUId && IsValidSteamId(sidI)) ? FindSlotSteamIdByUId(pSessionSlots, uidJ) : sidI;
                const bool sameSteamId = !sameUId && IsValidSteamId(sidI) && (sidJ == sidI);
                if (!sameUId && !sameSteamId) {
                    continue;
                }

                bool clearI = false;
                if (sameUId) {
                    // If entryJ has a newer active status (e.g. 0=fighter or 5=waiting vs 6=initial join), keep it in entryI
                    if (statusI == 6 && statusJ != 6) {
                        *reinterpret_cast<int32_t*>(entryI + 0x04) = statusJ;
                        statusI = statusJ;
                    }
                } else {
                    // Two distinct uIds point to the same SteamID! Preserve the row matching m_slots' canonical uId.
                    const int32_t canonUId = FindCanonicalUIdBySteamId(pSessionSlots, sidI);
                    if (canonUId >= 0 && uidJ == canonUId && uidI != canonUId) {
                        clearI = true;
                    }
                }

                uint8_t* const cleared = clearI ? entryI : entryJ;
                *reinterpret_cast<int32_t*>(cleared + 0x00) = -1;
                *reinterpret_cast<int32_t*>(cleared + 0x04) = -1;

                const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                if (saveNum <= 32) {
                    LogNetcode("[LobbyFix] Cleared duplicate BattleTable row (%d:uId=%d & %d:uId=%d, sid=0x%llX) in %s!",
                               i, uidI, j, uidJ, static_cast<unsigned long long>(sidI), contextTag);
                }
                if (clearI) {
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsUserRangeAccessible(uintptr_t addr, size_t size, bool requireWritable) noexcept {
    constexpr uintptr_t kMaxUserPtr = 0x00007FFFFFFF0000ULL;
    if (size == 0 || addr < kMinValidPtr || addr >= kMaxUserPtr || addr > (kMaxUserPtr - size)) {
        return false;
    }

    uintptr_t cursor = addr;
    const uintptr_t end = addr + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        const DWORD prot = mbi.Protect & 0xFFU;
        const bool writable =
            prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        const bool readable = writable || prot == PAGE_READONLY || prot == PAGE_EXECUTE_READ;
        if (requireWritable ? !writable : !readable) {
            return false;
        }
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor) {
            return false;
        }
        cursor = regionEnd;
    }
    return true;
}

void SanitizeChatTemplateVector(uintptr_t mgrAddr, size_t vecOffset, size_t entrySize, bool compactVector,
                                const char* vecTag, uintptr_t callerAddr) noexcept {
    auto* hdr = reinterpret_cast<uintptr_t*>(mgrAddr + vecOffset);
    const uintptr_t first = hdr[0];
    const uintptr_t last  = hdr[1];

    if (first == 0 && last == 0) {
        return;
    }

    const uintptr_t byteLen = (last >= first) ? (last - first) : 0;
    const size_t count = static_cast<size_t>(byteLen / sizeof(uintptr_t));
    if (first < kMinValidPtr || last < first ||
        (first & 0x7ULL) != 0 || (last & 0x7ULL) != 0 || count > 65536 ||
        (byteLen > 0 && !IsUserRangeAccessible(first, static_cast<size_t>(byteLen), true))) {
        hdr[0] = 0;
        hdr[1] = 0;
        hdr[2] = 0;
        const uint64_t saveNum = g_Telemetry.chatTemplateCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ChatTemplate::ClearDynamic (+0x0150CC8)! Reset invalid %s [0x%llX..0x%llX] caller=0x%llX.",
                       vecTag,
                       static_cast<unsigned long long>(first),
                       static_cast<unsigned long long>(last),
                       static_cast<unsigned long long>(callerAddr));
        }
        return;
    }

    auto* entries = reinterpret_cast<uintptr_t*>(first);
    size_t writeIdx = 0;
    size_t scrubbed = 0;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t elem = entries[i];
        if (!compactVector && elem == 0) {
            continue;
        }
        const bool valid = (elem & 0x7ULL) == 0 && IsUserRangeAccessible(elem, entrySize, false);
        if (compactVector) {
            if (valid) {
                entries[writeIdx++] = elem;
            } else {
                ++scrubbed;
            }
        } else if (!valid) {
            entries[i] = 0;
            ++scrubbed;
        }
    }

    if (scrubbed > 0) {
        if (compactVector) {
            hdr[1] = first + writeIdx * sizeof(uintptr_t);
        }
        const uint64_t saveNum = g_Telemetry.chatTemplateCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ChatTemplate::ClearDynamic (+0x0150CC8)! Scrubbed %zu/%zu dangling/null entries in %s (caller=0x%llX).",
                       scrubbed, count, vecTag, static_cast<unsigned long long>(callerAddr));
        }
    }
}

void CallChatTemplateClearDynamicSafe(void* pManager, bool* pFaulted) noexcept {
    __try {
        oChatTemplateClearDynamic(pManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (pFaulted) {
            *pFaulted = true;
        }
    }
}

// ============================================================================
// ccSpSkillCustomizeParam (data/spc/spSkillCustomizeParam.xfbin) &
// ccPreSetSaveData OOB spAtkIndex Crash Fixes (NSUNSC.exe+0x0702447 & twins)
// ============================================================================
// Each 0x98-byte RB-tree node in ccSpSkillCustomizeParam (loader @ +0x0701F70)
// stores:
//   +0x2C..+0x3F : float param1[5]  (indices 0..4, accessed by +0x0702220)
//   +0x40..+0x53 : float param2[5]  (indices 0..4, accessed by +0x0702270)
//   +0x58..+0x97 : { const char* msgId; uint32_t param; uint32_t pad; }[4]
//                  (indices 0..3, accessed by +0x07022C0, +0x0702310, +0x0702400)
// Unlike its sibling ccSkillCustomizeParam::GetSkillMessageId (+0x07015B0, which
// checks `cmp r8d, 5`), none of these 5 indexed accessors bounds-check `r8d`.
// A corrupted local or remote preset (e.g. spAtkIndex=0xB4800001) sign-extends
// `movsxd rax, r8d` and reads `[rdx + rax*16 + 0x58]` -> 0xC0000005.

constexpr uint32_t kMaxSpSkillFloatSlots = 5U;
constexpr uint32_t kMaxSpSkillMsgSlots   = 4U;

inline int32_t RecoverSafeSpSkillIndex(int32_t rawIndex, uint32_t maxSlots) noexcept {
    const uint32_t u = static_cast<uint32_t>(rawIndex);
    if (u < maxSlots) {
        return rawIndex;
    }
    // If upper 16 bits were corrupted while preserving a valid low-16 slot (e.g. 0xB4800001 -> 1,
    // 0xB4100000 -> 0), recover the original Ultimate Jutsu slot; otherwise fall back to slot 0
    // (matching CC2's own ccSkillCustomizeParam +0x07015D7 clamping convention).
    const uint32_t low16 = u & 0xFFFFU;
    if ((u & 0xFFFF0000U) != 0U && low16 < maxSlots) {
        return static_cast<int32_t>(low16);
    }
    return 0;
}

bool ValidateSpSkillParamTree(const void* pParamMgr) noexcept {
    constexpr uintptr_t kMaxUserPtr = 0x00007FFFFFFF0000ULL;
    const uintptr_t base = reinterpret_cast<uintptr_t>(pParamMgr);
    if (base < kMinValidPtr || base >= kMaxUserPtr || (base & 0x7ULL) != 0) {
        return false;
    }
    __try {
        const uintptr_t myHead = *reinterpret_cast<const uintptr_t*>(base + 0x30);
        if (myHead < kMinValidPtr || myHead >= kMaxUserPtr || (myHead & 0x7ULL) != 0) {
            return false;
        }
        if (*reinterpret_cast<const uint8_t*>(myHead + 0x19) != 1) {
            return false;
        }
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(myHead + 0x08);
        return (root >= kMinValidPtr && root < kMaxUserPtr && (root & 0x7ULL) == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline int32_t SanitizeSpSkillIndex(
    int32_t rawIndex, uint32_t maxSlots, int32_t charCode, const char* fnTag, uintptr_t callerAddr) noexcept {
    if (static_cast<uint32_t>(rawIndex) < maxSlots) {
        return rawIndex;
    }
    const int32_t safeIndex = RecoverSafeSpSkillIndex(rawIndex, maxSlots);
    const uint64_t saveNum = g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed) + 1;
    if (saveNum <= 16) {
        LogNetcode("[CrashFix] Prevented 0xC0000005 in %s! Out-of-bounds spAtkIndex=0x%08X (%d) -> %d (max=%u, charCode=%d, caller=0x%llX).",
                   fnTag,
                   static_cast<uint32_t>(rawIndex),
                   rawIndex,
                   safeIndex,
                   maxSlots,
                   charCode,
                   static_cast<unsigned long long>(callerAddr));
    }
    return safeIndex;
}

void SanitizeMainSavePreSetSpAtkIndices() noexcept {
    if (!g_BaseAddress) {
        return;
    }
    constexpr size_t kPresetCount  = 24;
    constexpr size_t kPresetStride = 0x1C0;
    constexpr size_t kCharCount    = 3;
    constexpr size_t kCharStride   = 0x14;
    constexpr size_t kSpAtkOffset  = 0x08;

    auto* pPresetsBase = reinterpret_cast<uint8_t*>(g_BaseAddress + StormRva::kData_MainSavePreSetArray);
    uint32_t sanitizedCount = 0;

    __try {
        for (size_t presetIdx = 0; presetIdx < kPresetCount; ++presetIdx) {
            uint8_t* pPreset = pPresetsBase + presetIdx * kPresetStride;
            for (size_t slotIdx = 0; slotIdx < kCharCount; ++slotIdx) {
                uint8_t* pChar = pPreset + slotIdx * kCharStride;
                const int32_t charId = *reinterpret_cast<const int32_t*>(pChar + 0x00);
                auto* pSpAtkIdx = reinterpret_cast<int32_t*>(pChar + kSpAtkOffset);
                const int32_t rawVal = *pSpAtkIdx;
                if (static_cast<uint32_t>(rawVal) >= kMaxSpSkillMsgSlots) {
                    const int32_t safeVal = RecoverSafeSpSkillIndex(rawVal, kMaxSpSkillMsgSlots);
                    *pSpAtkIdx = safeVal;
                    ++sanitizedCount;
                    g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
                    if (sanitizedCount <= 24) {
                        LogNetcode("[CrashFix] Sanitized ccPreSetSaveData Preset[%zu].c%zu (charId=0x%X): spAtkIndex 0x%08X -> %d.",
                                   presetIdx,
                                   slotIdx,
                                   static_cast<uint32_t>(charId),
                                   static_cast<uint32_t>(rawVal),
                                   safeVal);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if (sanitizedCount > 0) {
        LogNetcode("[CrashFix] Cleaned %u corrupted spAtkIndex slot(s) in ccPreSetSaveData prior to SaveData::SyncShadow.",
                   sanitizedCount);
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

uint8_t __fastcall hk_KeyboardGetPressedChar(const void* pKeyMap) {
    if (!IsKeyboardKeyMapSafeToQuery(pKeyMap, "KeyboardGetPressedChar (+0x120FEE0)")) {
        return 0;
    }
    uint8_t ch = oKeyboardGetPressedChar ? oKeyboardGetPressedChar(pKeyMap) : 0;
    // In numeric-only mode (Session ID), vanilla's hardcoded US-QWERTY table (0x1413F58A0)
    // maps Shift+top-row (AZERTY digit entry!) to '!'..')' and Shift+Numpad to '\0', which
    // OnVirtualKeyboardFinish strips while still consuming m_maxLen (9) buffer slots until
    // the input box silently locks up! Normalize any HID digit key directly to ASCII '0'..'9'.
    if ((ch < '0' || ch > '9') && IsVirtualKeyboardNumericModeActiveSeh()) {
        const uint32_t hidKey = oKeyboardGetPressedKey ? oKeyboardGetPressedKey(pKeyMap) : 0u;
        const uint8_t  digit  = MapNumericHidKeyToAsciiDigit(hidKey);
        if (digit != 0) {
            ch = digit;
        }
    }
    return ch;
}

uint32_t __fastcall hk_KeyboardGetPressedKey(const void* pKeyMap) {
    if (reinterpret_cast<uintptr_t>(pKeyMap) == 0x60ULL) {
        if (const void* recoveredKeyMap = TryRecoverVirtualKeyboardDeviceSeh()) {
            if (IsKeyboardKeyMapSafeToQuery(recoveredKeyMap, "KeyboardGetPressedKey (+0x120FFB5)")) {
                return oKeyboardGetPressedKey ? oKeyboardGetPressedKey(recoveredKeyMap) : 0;
            }
        }
    }
    if (!IsKeyboardKeyMapSafeToQuery(pKeyMap, "KeyboardGetPressedKey (+0x120FFB5)")) {
        return 0;
    }
    return oKeyboardGetPressedKey ? oKeyboardGetPressedKey(pKeyMap) : 0;
}

// Fixes vanilla Use-After-Free crash @ NSUNSC.exe+0x00170878 (NSUNSC.exe.22936.dmp):
// ~ccNetIdSearchState (+0x016FBB0) never closes ccVirtualKeyboard or clears [pVkImpl + 0xC0].
// Detach the callback (and close the virtual keyboard if still active) BEFORE freeing `this`.
void __fastcall hk_NetIdSearchStateDtor(void* pThis) {
    void* pVkImpl = nullptr;
    void* pCmd    = nullptr;
    bool  wasOpen = false;
    if (DetachVirtualKeyboardCallbackSeh(pThis, &pVkImpl, &pCmd, &wasOpen)) {
        if (wasOpen) {
            CloseVirtualKeyboardImplSeh(pVkImpl);
            const uint64_t saveNum = g_Telemetry.virtualKeyboardNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
            LogNetcode("[CrashFix] Closed active ccVirtualKeyboard & detached callback in ~ccNetIdSearchState (+0x16FBB0) "
                       "for pThis=0x%llX — prevented 0xC0000005 UAF @ +0x170878 (saves=%llu).",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pThis)),
                       static_cast<unsigned long long>(saveNum));
        }
        DestroyDetachedSimpleCommandSeh(pVkImpl, pCmd);
    }
    if (oNetIdSearchDtor) {
        oNetIdSearchDtor(pThis);
    }
}

// Second-layer guard on ccNetIdSearchState::OnVirtualKeyboardFinish (+0x0170840 -> faults @ +0x0170878).
void __fastcall hk_NetIdSearchStateOnVkFinish(void* pThis) {
    const char* failReason = nullptr;
    if (!ValidateNetIdSearchStateForVkFinishSeh(pThis, &failReason)) {
        const uint64_t saveNum = g_Telemetry.virtualKeyboardNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        LogNetcode("[CrashFix] Prevented 0xC0000005 UAF in ccNetIdSearchState::OnVirtualKeyboardFinish (+0x170878)! "
                   "pThis=0x%llX (%s, saves=%llu).",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pThis)),
                   failReason ? failReason : "invalid state",
                   static_cast<unsigned long long>(saveNum));
        void* pVkImpl = nullptr;
        void* pCmd    = nullptr;
        bool  wasOpen = false;
        if (DetachVirtualKeyboardCallbackSeh(pThis, &pVkImpl, &pCmd, &wasOpen)) {
            DestroyDetachedSimpleCommandSeh(pVkImpl, pCmd);
        }
        return;
    }
    if (oNetIdSearchOnVkFinish) {
        oNetIdSearchOnVkFinish(pThis);
    }
}

namespace {

bool PreCheckCommDestroySocketsSeh(void* pCommInner, uint64_t* outCookie, int32_t* outSockCount) noexcept {
    *outCookie    = 0;
    *outSockCount = 0;
    if (reinterpret_cast<uintptr_t>(pCommInner) < kMinValidPtr) {
        return false;
    }
    __try {
        auto* pBytes = reinterpret_cast<uint8_t*>(pCommInner);
        const uintptr_t pSockets = *reinterpret_cast<const uintptr_t*>(pBytes + 0x08);
        const int32_t   sockCnt  = *reinterpret_cast<const int32_t*>(pBytes + 0x10);
        const int32_t   isArray  = *reinterpret_cast<const int32_t*>(pBytes + 0x14);
        if (isArray != 0 && pSockets >= kMinValidPtr && pSockets <= kMaxValidUserPtr) {
            const uint64_t cookie = *reinterpret_cast<const uint64_t*>(pSockets - 8);
            if (sockCnt <= 0 || sockCnt > 64 || cookie != static_cast<uint64_t>(sockCnt)) {
                *reinterpret_cast<int32_t*>(pBytes + 0x14) = 0; // Force scalar delete (edx=1)
                *outCookie    = cookie;
                *outSockCount = sockCnt;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

void PostResetCommDestroySocketsSeh(void* pCommInner) noexcept {
    if (reinterpret_cast<uintptr_t>(pCommInner) < kMinValidPtr) {
        return;
    }
    __try {
        auto* pBytes = reinterpret_cast<uint8_t*>(pCommInner);
        *reinterpret_cast<int32_t*>(pBytes + 0x10) = 0;
        *reinterpret_cast<int32_t*>(pBytes + 0x14) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool TryTriggerSelfDisconnectOnMemberLeftSeh(void* pSession, uint64_t leavingSteamId) noexcept {
    if (reinterpret_cast<uintptr_t>(pSession) < kMinValidPtr || !IsValidSteamId(leavingSteamId)) {
        return false;
    }
    __try {
        auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
        const uintptr_t pMySlot = *reinterpret_cast<const uintptr_t*>(pSessBytes + 0x500);
        if (pMySlot < kMinValidPtr || pMySlot > kMaxValidUserPtr) {
            return false;
        }
        const uint64_t mySteamId = *reinterpret_cast<const uint64_t*>(pMySlot + 0x38);
        if (IsValidSteamId(mySteamId) && mySteamId == leavingSteamId) {
            // Exact mirror of ccNetSession::DisconnectSelf (0x0113BC20)
            if (*reinterpret_cast<const int32_t*>(pSessBytes + 0x530) == 0) {
                *reinterpret_cast<int32_t*>(pSessBytes + 0x530) = 1;
                *reinterpret_cast<int32_t*>(pSessBytes + 0x538) = 0x1E;
            }
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool CheckAndCloseZeroMemberZombieSessionSeh(void* pSession) noexcept {
    if (reinterpret_cast<uintptr_t>(pSession) < kMinValidPtr) {
        return false;
    }
    __try {
        auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
        const int32_t memberCount = *reinterpret_cast<const int32_t*>(pSessBytes + 0x1B0);
        if (memberCount <= 0 && *reinterpret_cast<const int32_t*>(pSessBytes + 0x530) == 0) {
            *reinterpret_cast<int32_t*>(pSessBytes + 0x530) = 1;
            *reinterpret_cast<int32_t*>(pSessBytes + 0x538) = 0x1E;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

} // namespace

// Fixes vanilla new vs delete[] mismatch crash @ NSUNSC.exe+0x0117116C (NSUNSC.exe.15168.dmp):
// ConnectToHost (+0x01169480 / +0x01169500) allocates a scalar `new ccNetSessionTCPSocketBase`
// (0x10 bytes, no 8-byte array count cookie) after DestroySockets (+0x01168E20) WITHOUT clearing
// m_isArray (+0x14 = 1 left over from an earlier Host new[] allocation). On teardown, delete[]
// reads the NUCC heap allocator header at [m_pSockets - 8] (e.g. 0x23 = 35!) and walks OOB!
void __fastcall hk_SessionCommDestroySockets(void* pCommInner) {
    if (!oSessionCommDestroySockets) {
        return;
    }
    uint64_t badCookie = 0;
    int32_t  sockCnt   = 0;
    if (PreCheckCommDestroySocketsSeh(pCommInner, &badCookie, &sockCnt)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        LogNetcode("[CrashFix] Prevented 0xC0000005 new/delete[] mismatch in Comm::DestroySockets (+0x1168E20 -> +0x117116C)! "
                   "pCommInner=0x%llX sockCount=%d badCookie=0x%llX -> forced scalar delete (saves=%llu).",
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pCommInner)),
                   sockCnt,
                   static_cast<unsigned long long>(badCookie),
                   static_cast<unsigned long long>(saveNum));
    }
    oSessionCommDestroySockets(pCommInner);
    PostResetCommDestroySocketsSeh(pCommInner);
}

// Fixes vanilla 0-member "zombie lobby" (4 empty dark-grey cards) in Custom/Endless lobbies:
// Steam callback OnSteamLobbyChatUpdate (+0x0113DE70) jumps directly to OnMemberLeft (+0x0113D1E0)
// WITHOUT checking `leavingSteamId == m_pMySlot->steamId` (unlike +0x116DDD1 & +0x116EE80!).
// When Steam drops the local Host, OnMemberLeft zeroes m_slots[0], drops memberCount to 0, and
// skips DisconnectSelf (+0x0113BC20) because host migration (+0x16C) is enabled.
void __fastcall hk_SessionOnMemberLeft(void* pSession, uint64_t leavingSteamId) {
    if (TryTriggerSelfDisconnectOnMemberLeftSeh(pSession, leavingSteamId)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        LogNetcode("[LobbyFix] Prevented zombie 0-member lobby in ccNetSession::OnMemberLeft (+0x113D1E0)! "
                   "Steam dropped local SteamID 0x%llX -> triggered clean DisconnectSelf (saves=%llu).",
                   static_cast<unsigned long long>(leavingSteamId),
                   static_cast<unsigned long long>(saveNum));
        return;
    }
    if (oSessionOnMemberLeft) {
        oSessionOnMemberLeft(pSession, leavingSteamId);
    }
    if (CheckAndCloseZeroMemberZombieSessionSeh(pSession)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        LogNetcode("[LobbyFix] Closed empty 0-member zombie session after ccNetSession::OnMemberLeft (+0x113D1E0) (saves=%llu).",
                   static_cast<unsigned long long>(saveNum));
    }
}

// Guards ccNetSessionCommunicator::SendPacket (0x141169290, faults @ +0x11692A3 in 14496.dmp)
// when [pSession + 0x518] (m_pCommunicator) or [pComm + 0x08] becomes NULL during abrupt disconnect.
int32_t __fastcall hk_SessionCommSendPacket(void* pComm, void* pPacket, uint32_t packetSize, int32_t targetUId) {
    const uintptr_t commAddr = reinterpret_cast<uintptr_t>(pComm);
    const uintptr_t pktAddr  = reinterpret_cast<uintptr_t>(pPacket);
    uintptr_t innerComm      = 0;
    if (commAddr >= kMinValidPtr && pktAddr >= kMinValidPtr) {
        __try {
            innerComm = *reinterpret_cast<const uintptr_t*>(commAddr + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            innerComm = 0;
        }
    }
    if (innerComm < kMinValidPtr) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in SessionComm::SendPacket (+0x11692A3)! pComm=0x%llX pPacket=0x%llX targetUId=%d (abrupt disconnect).",
                       static_cast<unsigned long long>(commAddr),
                       static_cast<unsigned long long>(pktAddr),
                       targetUId);
        }
        return -1;
    }
    return oSessionCommSendPkt ? oSessionCommSendPkt(pComm, pPacket, packetSize, targetUId) : -1;
}

// Guards ccNetSessionCommunicator::BroadcastPacket (0x141169310, faults @ +0x1169320)
// when [pSession + 0x518] (m_pCommunicator) or [pComm + 0x08] becomes NULL during abrupt disconnect.
int32_t __fastcall hk_SessionCommBroadcastPacket(void* pComm, void* pPacket, uint32_t packetSize, int32_t excludeUId) {
    const uintptr_t commAddr = reinterpret_cast<uintptr_t>(pComm);
    const uintptr_t pktAddr  = reinterpret_cast<uintptr_t>(pPacket);
    uintptr_t innerComm      = 0;
    if (commAddr >= kMinValidPtr && pktAddr >= kMinValidPtr) {
        __try {
            innerComm = *reinterpret_cast<const uintptr_t*>(commAddr + 0x08);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            innerComm = 0;
        }
    }
    if (innerComm < kMinValidPtr) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in SessionComm::BroadcastPacket (+0x1169320)! pComm=0x%llX pPacket=0x%llX excludeUId=%d (abrupt disconnect).",
                       static_cast<unsigned long long>(commAddr),
                       static_cast<unsigned long long>(pktAddr),
                       excludeUId);
        }
        return -1;
    }
    return oSessionCommBroadcastPkt ? oSessionCommBroadcastPkt(pComm, pPacket, packetSize, excludeUId) : -1;
}

// Guards GetMySlot (0x1410B8E20 -> jmp 0x14113C270):
// 1) Vanilla `0x10B8E20` executes:
//      mov rcx, [rcx + 0x20]
//      xor edx, edx
//      lea rax, [rcx + 0x18]
//      test rax, rax
//      cmove rcx, rdx
//      jmp 0x113C270  ; mov rdx, [rcx + 0x500]
//    When `[holder + 0x20]` is NULL (0), `rax = 0 + 0x18 = 0x18 != 0`, so `cmove` does NOT zero `rcx`,
//    and `0x113C270` dereferences `[0 + 0x500]` -> 0xC0000005!
//    Note that `rcx` passed to `0x113C270` is `[holder + 0x20]` (WITHOUT `+0x18`).
// 2) When `[pSession + 0x500]` is NULL during an abrupt disconnect, callers that immediately read
//    `[rax + 0x40]` or `[rax + 0x24]` without a NULL check would fault; we return `s_SafeDummySessionSlot`
//    ONLY for those immediate-field-dereference callers, and `nullptr` for all pointer-checking/caching callers.
void* __fastcall hk_SessionGetMySlot(void* pSessionHolder) {
    const uintptr_t holderAddr = reinterpret_cast<uintptr_t>(pSessionHolder);
    void* pSlot = nullptr;

    if (holderAddr >= kMinValidPtr) {
        __try {
            const uintptr_t sessionAddr = *reinterpret_cast<const uintptr_t*>(holderAddr + 0x20);
            if (sessionAddr >= kMinValidPtr) {
                auto* pSession = reinterpret_cast<uint8_t*>(sessionAddr);
                pSlot = *reinterpret_cast<void**>(pSession + 0x500);
                if (reinterpret_cast<uintptr_t>(pSlot) >= kMinValidPtr) {
                    const uint64_t sid = *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pSlot) + 0x38);
                    if (IsValidSteamId(sid)) {
                        s_LastKnownLocalSteamId.store(sid, std::memory_order_relaxed);
                    }
                    return pSlot;
                }
                pSlot = nullptr;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            pSlot = nullptr;
        }
    }

    const auto* ret = reinterpret_cast<const uint8_t*>(_ReturnAddress());
    if (CallerImmediatelyDereferencesSlotField(ret)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in Session::GetMySlot (+0x10B8E20) for unchecked caller @ 0x%llX!",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret)));
        }
        return PrepareSafeDummySessionSlot();
    }

    return nullptr;
}

// Guards SortSlotsAndRefreshMySlot (0x14113FA50):
// Vanilla dereferences `mov rbx, [rcx + 0x500]; mov rbx, [rbx + 0x38]` at +0x113FA7E without checking
// whether `m_pMySlot` (`[rcx + 0x500]`) is NULL, and only searches `0..m_maxMember-1` after `qsort`.
void __fastcall hk_SessionSortSlotsAndRefreshMySlot(void* pSession) {
    auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
    if (reinterpret_cast<uintptr_t>(pSessBytes) < kMinValidPtr || !oSessionSortSlots) {
        return;
    }

    uint8_t* const slotsBase = pSessBytes + 0x2C0;
    uint8_t* mySlot = *reinterpret_cast<uint8_t**>(pSessBytes + 0x500);
    uint64_t mySteamId = 0;

    if (reinterpret_cast<uintptr_t>(mySlot) >= kMinValidPtr) {
        mySteamId = *reinterpret_cast<const uint64_t*>(mySlot + 0x38);
        if (IsValidSteamId(mySteamId)) {
            s_LastKnownLocalSteamId.store(mySteamId, std::memory_order_relaxed);
        }
    } else {
        // Attempt to recover m_pMySlot from m_slots[0..7] using last known local SteamID
        const uint64_t knownSid = s_LastKnownLocalSteamId.load(std::memory_order_relaxed);
        if (IsValidSteamId(knownSid)) {
            for (int i = 0; i < 8; ++i) {
                uint8_t* slotI = slotsBase + i * 0x48;
                if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == knownSid) {
                    mySlot = slotI;
                    mySteamId = knownSid;
                    *reinterpret_cast<uint8_t**>(pSessBytes + 0x500) = slotI;
                    break;
                }
            }
        }
        if (!mySlot) {
            g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    SanitizeSessionMemberSlots(pSessBytes, true, "Session::SortSlots");

    __try {
        oSessionSortSlots(pSession);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
    }

    // Ensure m_pMySlot ([pSession + 0x500]) is refreshed across all 8 slots even if m_maxMember < 8
    if (IsValidSteamId(mySteamId)) {
        uint8_t* postSlot = *reinterpret_cast<uint8_t**>(pSessBytes + 0x500);
        if (reinterpret_cast<uintptr_t>(postSlot) < kMinValidPtr ||
            *reinterpret_cast<const uint64_t*>(postSlot + 0x38) != mySteamId) {
            for (int i = 0; i < 8; ++i) {
                uint8_t* slotI = slotsBase + i * 0x48;
                if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == mySteamId) {
                    *reinterpret_cast<uint8_t**>(pSessBytes + 0x500) = slotI;
                    break;
                }
            }
        }
    }
}

// Guards ProcessReceivedSessionMessages (0x14116DAE0) where [pSession + 0x500] (m_pMySlot) is
// dereferenced unchecked at 9 locations (+0x116DDD1, +0x116DE54, +0x116E3B0, +0x116E5B2, +0x116E8BB,
// +0x116EA34, +0x116EC41, +0x116ED82, +0x116EE8F).
void __fastcall hk_SessionProcessReceivedMessages(void* pSession) {
    auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
    if (reinterpret_cast<uintptr_t>(pSessBytes) < kMinValidPtr || !oSessionProcessMsgs) {
        return;
    }

    // In vanilla 0x116DAE0 (+0x116DB2C), if [pSession + 0x518] (m_pCommunicator) is NULL, it returns immediately.
    // Checking this upfront avoids false-positive NULL m_pMySlot saves during idle/lobby creation frames.
    if (*reinterpret_cast<void**>(pSessBytes + 0x518) == nullptr) {
        return;
    }

    bool usedDummySlot = false;
    if (*reinterpret_cast<void**>(pSessBytes + 0x500) == nullptr) {
        // First check if local player's slot is already in m_slots[0..7]
        uint8_t* const slotsBase = pSessBytes + 0x2C0;
        const uint64_t knownSid  = s_LastKnownLocalSteamId.load(std::memory_order_relaxed);
        if (IsValidSteamId(knownSid)) {
            for (int i = 0; i < 8; ++i) {
                uint8_t* slotI = slotsBase + i * 0x48;
                if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == knownSid) {
                    *reinterpret_cast<void**>(pSessBytes + 0x500) = slotI;
                    break;
                }
            }
        }
        if (*reinterpret_cast<void**>(pSessBytes + 0x500) == nullptr) {
            *reinterpret_cast<void**>(pSessBytes + 0x500) = PrepareSafeDummySessionSlot();
            usedDummySlot = true;
            const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
            if (saveNum <= 16) {
                LogNetcode("[CrashFix] Prevented 0xC0000005 in ProcessReceivedSessionMessages (+0x116DAE0)! m_pMySlot was NULL during packet drain.");
            }
        }
    }

    __try {
        oSessionProcessMsgs(pSession);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
    }

    SanitizeSessionMemberSlots(pSessBytes, true, "Session::ProcessMessages");

    if (usedDummySlot && *reinterpret_cast<void**>(pSessBytes + 0x500) == s_SafeDummySessionSlot) {
        *reinterpret_cast<void**>(pSessBytes + 0x500) = nullptr;
    }
}

// Fixes ccNetSessionData::AddMemberToTable (0x14113AF60) which stops at the first empty slot (e.g. slot 0)
// and inserts a duplicate SteamID without checking whether that SteamID already exists in slots[1..7]!
void __fastcall hk_SessionAddMemberToTable(void* pSessionData, const uint64_t* pMemberSteamId, const uint64_t* pLobbySteamId) {
    auto* pDataBytes = reinterpret_cast<uint8_t*>(pSessionData);
    if (reinterpret_cast<uintptr_t>(pDataBytes) < kMinValidPtr ||
        reinterpret_cast<uintptr_t>(pMemberSteamId) < kMinValidPtr ||
        reinterpret_cast<uintptr_t>(pLobbySteamId) < kMinValidPtr ||
        !oSessionAddMember) {
        return;
    }

    const uint64_t memberSid = *pMemberSteamId;
    if (!IsValidSteamId(memberSid)) {
        return;
    }

    uint8_t* const slotsBase = pDataBytes + 0x13C;
    const int32_t  maxMember = *reinterpret_cast<const int32_t*>(pDataBytes + 0x28);
    int32_t existingIdx = -1;
    for (int i = 0; i < 8; ++i) {
        const uint8_t* slotI = slotsBase + i * 0x48;
        const uint64_t sidI  = *reinterpret_cast<const uint64_t*>(slotI + 0x38);
        if (sidI == memberSid) {
            existingIdx = i;
            break;
        }
    }

    // Only skip oSessionAddMember if the session metadata (m_maxMember at +0x28) has already been
    // initialized by a prior AddMemberToTable call and this member's slot already has a valid uId (> 0)
    // or is at index > 0 with an empty earlier slot that would cause vanilla 0x113AF60 to duplicate it.
    if (existingIdx >= 0 && maxMember > 0) {
        const uint8_t* existingSlot = slotsBase + existingIdx * 0x48;
        const int32_t  existingUId  = *reinterpret_cast<const int32_t*>(existingSlot + 0x40);
        if (existingUId > 0 || existingIdx > 0) {
            const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
            if (saveNum <= 16) {
                LogNetcode("[LobbyFix] Blocked duplicate AddMemberToTable for SteamID 0x%llX (already in slot %d, uId=%d)",
                           static_cast<unsigned long long>(memberSid),
                           existingIdx,
                           existingUId);
            }
            return;
        }
    }

    oSessionAddMember(pSessionData, pMemberSteamId, pLobbySteamId);
}

// Fixes ccNetSessionImpl::AssignNextUId (0x14113B3C0):
// Prevents overwriting an already-assigned valid uId (>= 10) on retransmitted JoinRequests,
// and records the joining member's SteamID so hk_SessionInvokeCallback can re-resolve the slot after sorting.
void __fastcall hk_SessionAssignNextUId(void* pSession, int32_t* pSlotUId) {
    auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
    if (reinterpret_cast<uintptr_t>(pSessBytes) < kMinValidPtr ||
        reinterpret_cast<uintptr_t>(pSlotUId) < kMinValidPtr) {
        return;
    }

    const auto* pSlotBytes = reinterpret_cast<const uint8_t*>(pSlotUId) - 0x40;
    const uint64_t slotSteamId = *reinterpret_cast<const uint64_t*>(pSlotBytes + 0x38);
    s_LastAssignedMemberSteamId.store(slotSteamId, std::memory_order_relaxed);

    const int32_t currentUId = *pSlotUId;
    if (currentUId >= 10) {
        // Verify this uId does not collide with another distinct member
        const uint8_t* const slotsBase = pSessBytes + 0x2C0;
        bool collides = false;
        for (int i = 0; i < 8; ++i) {
            const uint8_t* slotI = slotsBase + i * 0x48;
            if (slotI == pSlotBytes) continue;
            if (IsValidSteamId(*reinterpret_cast<const uint64_t*>(slotI + 0x38)) &&
                *reinterpret_cast<const int32_t*>(slotI + 0x40) == currentUId) {
                collides = true;
                break;
            }
        }
        if (!collides) {
            s_LastAssignedMemberUId.store(currentUId, std::memory_order_relaxed);
            s_LastAssignedMemberWasDuplicate.store(true, std::memory_order_relaxed);
            if (*reinterpret_cast<int32_t*>(pSessBytes + 0x168) < currentUId) {
                *reinterpret_cast<int32_t*>(pSessBytes + 0x168) = currentUId;
            }
            const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
            if (saveNum <= 16) {
                LogNetcode("[LobbyFix] Preserved existing uId=%d for SteamID 0x%llX in AssignNextUId (ignored duplicate JoinRequest)",
                           currentUId, static_cast<unsigned long long>(slotSteamId));
            }
            return;
        }
    }

    s_LastAssignedMemberWasDuplicate.store(false, std::memory_order_relaxed);
    if (oSessionAssignNextUId) {
        oSessionAssignNextUId(pSession, pSlotUId);
    } else {
        const int32_t nextUId = ++(*reinterpret_cast<int32_t*>(pSessBytes + 0x168));
        *pSlotUId = nextUId;
    }
    s_LastAssignedMemberUId.store(*pSlotUId, std::memory_order_relaxed);
}

// Fixes InvokeSessionCallback (0x14113D840, called at +0x116EB0E after SortSlotsAndRefreshMySlot):
// Re-resolves `pSlot` (`r8`) by SteamID after sorting so `OnMemberJoined` is never passed a stale
// pre-sort slot pointer (which pointed to the Host's own slot when sorting swapped slots!).
void __fastcall hk_SessionInvokeCallback(void* pSession, int32_t callbackId, void* pSlot) {
    if (!oSessionInvokeCallback || reinterpret_cast<uintptr_t>(pSession) < kMinValidPtr) {
        return;
    }

    if (callbackId == 0) {
        const bool     wasDuplicate = s_LastAssignedMemberWasDuplicate.exchange(false, std::memory_order_relaxed);
        const uint64_t expectedSid  = s_LastAssignedMemberSteamId.exchange(0ULL, std::memory_order_relaxed);
        const int32_t  expectedUId  = s_LastAssignedMemberUId.exchange(0, std::memory_order_relaxed);

        // If pSlot is NULL (e.g. lobby full 8/8 -> 0x14116EACF skipped AssignNextUId) or duplicate, abort immediately
        // before attempting slot re-resolution so a prior player's SteamID can never hijack a NULL callback.
        if (wasDuplicate || reinterpret_cast<uintptr_t>(pSlot) < kMinValidPtr) {
            return;
        }

        auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
        uint8_t* const slotsBase = pSessBytes + 0x2C0;

        if (IsValidSteamId(expectedSid)) {
            const bool slotMatches =
                *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(pSlot) + 0x38) == expectedSid;
            if (!slotMatches) {
                for (int i = 0; i < 8; ++i) {
                    uint8_t* slotI = slotsBase + i * 0x48;
                    if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == expectedSid) {
                        pSlot = slotI;
                        g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed);
                        LogNetcode("[LobbyFix] Corrected stale pre-sort slot pointer in InvokeSessionCallback -> slot %d (SteamID=0x%llX, uId=%d)",
                                   i, static_cast<unsigned long long>(expectedSid), expectedUId);
                        break;
                    }
                }
            }
        }
    }

    if (reinterpret_cast<uintptr_t>(pSlot) < kMinValidPtr) {
        return;
    }
    oSessionInvokeCallback(pSession, callbackId, pSlot);
}

// Fixes State1_Update existing-member handler (0x14113C8B0):
// Vanilla returns 1 (skipping AddMemberToTable + duplicate OnMemberJoined callback) when SteamID matches m_slots[0..7],
// EXCEPT it unconditionally overwrites `slot->uId = newMemberEntry->uId` at +0x113C99C even when slot already has
// an active uId (>= 10), desyncing BattleTable vs m_slots. Preserve the existing valid uId when non-colliding.
int32_t __fastcall hk_SessionCheckExistingNewMember(void* pSession, void* pNewMemberEntry) {
    if (!oSessionCheckNewMember) {
        return 0;
    }
    auto* pSessBytes  = reinterpret_cast<uint8_t*>(pSession);
    auto* pEntryBytes = reinterpret_cast<const uint8_t*>(pNewMemberEntry);
    int32_t matchedIdx = -1;
    int32_t savedUId   = -1;
    uint64_t entrySid  = 0ULL;

    if (reinterpret_cast<uintptr_t>(pSessBytes) >= kMinValidPtr &&
        reinterpret_cast<uintptr_t>(pEntryBytes) >= kMinValidPtr) {
        __try {
            entrySid = *reinterpret_cast<const uint64_t*>(pEntryBytes + 0x28);
            if (IsValidSteamId(entrySid)) {
                const uint8_t* slotsBase = pSessBytes + 0x2C0;
                for (int32_t i = 0; i < 8; ++i) {
                    const uint8_t* slotI = slotsBase + static_cast<size_t>(i) * 0x48;
                    if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) == entrySid) {
                        const int32_t uidI = *reinterpret_cast<const int32_t*>(slotI + 0x40);
                        if (uidI >= 10) {
                            matchedIdx = i;
                            savedUId   = uidI;
                        }
                        break;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            matchedIdx = -1;
        }
    }

    const int32_t result = oSessionCheckNewMember(pSession, pNewMemberEntry);

    if (result != 0 && matchedIdx >= 0 && savedUId >= 10) {
        __try {
            uint8_t* const slotsBase = pSessBytes + 0x2C0;
            uint8_t* const slotM     = slotsBase + static_cast<size_t>(matchedIdx) * 0x48;
            if (*reinterpret_cast<const uint64_t*>(slotM + 0x38) == entrySid &&
                *reinterpret_cast<const int32_t*>(slotM + 0x40) != savedUId) {
                bool collides = false;
                for (int32_t i = 0; i < 8; ++i) {
                    if (i == matchedIdx) continue;
                    const uint8_t* slotI = slotsBase + static_cast<size_t>(i) * 0x48;
                    if (IsValidSteamId(*reinterpret_cast<const uint64_t*>(slotI + 0x38)) &&
                        *reinterpret_cast<const int32_t*>(slotI + 0x40) == savedUId) {
                        collides = true;
                        break;
                    }
                }
                if (!collides) {
                    const int32_t overwrittenUId = *reinterpret_cast<int32_t*>(slotM + 0x40);
                    *reinterpret_cast<int32_t*>(slotM + 0x40) = savedUId;
                    if (*reinterpret_cast<int32_t*>(pSessBytes + 0x168) < savedUId) {
                        *reinterpret_cast<int32_t*>(pSessBytes + 0x168) = savedUId;
                    }
                    const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (saveNum <= 16) {
                        LogNetcode("[LobbyFix] Preserved slot[%d].uId=%d (blocked stale NewMember uId=%d) for SteamID 0x%llX!",
                                   matchedIdx, savedUId, overwrittenUId, static_cast<unsigned long long>(entrySid));
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return result;
}

// Guards NewMemberTbl::Add (0x141169E20, called only with pSession + 0x520 from ProcessMessages / RecvNewMember):
// Prevents duplicate queued entries for the same SteamID and ignores redundant NewMember arrivals for already-slotted peers.
uint8_t __fastcall hk_SessionNewMemberTblAdd(void* pNewMemberContainer, uint64_t memberSteamId, const void* pMeta, int32_t uId) {
    if (!oSessionNewMemberTblAdd) {
        return 0;
    }
    if (reinterpret_cast<uintptr_t>(pNewMemberContainer) >= kMinValidPtr && IsValidSteamId(memberSteamId)) {
        bool suppress = false;
        __try {
            auto* pCont = reinterpret_cast<uint8_t*>(pNewMemberContainer);
            uint8_t* inner = *reinterpret_cast<uint8_t**>(pCont + 0x08);
            if (reinterpret_cast<uintptr_t>(inner) >= kMinValidPtr) {
                uint8_t* sentinel = *reinterpret_cast<uint8_t**>(inner + 0x20);
                if (reinterpret_cast<uintptr_t>(sentinel) >= kMinValidPtr) {
                    uint8_t* node = *reinterpret_cast<uint8_t**>(sentinel);
                    for (int hops = 0; hops < 32 && reinterpret_cast<uintptr_t>(node) >= kMinValidPtr && node != sentinel; ++hops) {
                        uint8_t* elem = node + 0x10;
                        if (*reinterpret_cast<const uint64_t*>(elem + 0x28) == memberSteamId) {
                            auto* queuedUId = reinterpret_cast<int32_t*>(elem + 0x24);
                            if (uId > 0 && *queuedUId <= 0) {
                                *queuedUId = uId;
                            }
                            suppress = true;
                            break;
                        }
                        node = *reinterpret_cast<uint8_t**>(node);
                    }
                }
            }

            if (!suppress) {
                // `pNewMemberContainer` is `pSession + 0x520` at both verified callers (+0x116DDC7 & +0x116EF75)
                uint8_t* pSessBytes = pCont - 0x520;
                const uint8_t* slotsBase = pSessBytes + 0x2C0;
                const uint8_t* mySlot    = *reinterpret_cast<const uint8_t* const*>(pSessBytes + 0x500);
                for (int32_t i = 0; i < 8; ++i) {
                    const uint8_t* slotI = slotsBase + static_cast<size_t>(i) * 0x48;
                    if (*reinterpret_cast<const uint64_t*>(slotI + 0x38) != memberSteamId) {
                        continue;
                    }
                    const int32_t existingUId = *reinterpret_cast<const int32_t*>(slotI + 0x40);
                    if (existingUId >= 10 && (existingUId == uId || slotI == mySlot)) {
                        suppress = true;
                        const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                        if (saveNum <= 16) {
                            LogNetcode("[LobbyFix] Suppressed redundant NewMemberTbl::Add for SteamID 0x%llX (already in slot %d, uId=%d)",
                                       static_cast<unsigned long long>(memberSteamId), i, existingUId);
                        }
                    }
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            suppress = false;
        }
        if (suppress) {
            return 1;
        }
    }
    return oSessionNewMemberTblAdd(pNewMemberContainer, memberSteamId, pMeta, uId);
}

// Fixes UpdateBattleTableFromChangeQueue (0x1400F4310):
// At +0x00F4439..+0x00F4479 (Loop 1, status == 6) and +0x00F4750..+0x00F4847 (Loop 3, remaining statuses),
// vanilla searches BattleTable[0..7] ONLY for empty slots without checking whether uId (or SteamID) is already present.
void __fastcall hk_CustomLobbyUpdateBattleTable(void* pLobbyState) {
    if (!oCustomLobbyUpdateBattleTbl || reinterpret_cast<uintptr_t>(pLobbyState) < kMinValidPtr || !g_BaseAddress) {
        return;
    }

    uint8_t* pBattleTable  = nullptr;
    uint8_t* pSessionData  = nullptr;
    const uint8_t* pSlots  = nullptr;
    __try {
        const uintptr_t pNetMgr = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
        if (pNetMgr >= kMinValidPtr) {
            const uintptr_t pLocalInfo = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0x100);
            const uintptr_t pSessData  = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0xF8);
            if (pLocalInfo >= kMinValidPtr) {
                pBattleTable = reinterpret_cast<uint8_t*>(pLocalInfo + 0xC8);
            }
            if (pSessData >= kMinValidPtr) {
                // Verified at NSUNSC.exe+0x00F43A0..+0x00F43C6 & +0x10F39B0..+0x10F39EA:
                // [pNetMgr + 0xF8] is ccNetSessionData* (+0x28=m_maxMember, +0x2C=m_memberCount, +0x13C=m_slots[8]),
                // NOT ccNetSession* (which embeds ccNetSessionData at +0x184 -> m_slots at +0x2C0).
                pSessionData = reinterpret_cast<uint8_t*>(pSessData);
                pSlots       = pSessionData + 0x13C;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    if (pBattleTable) {
        // 1. Deduplicate BEFORE scrubbing m_slots so distinct uIds sharing a SteamID are caught immediately
        DeduplicateCustomLobbyBattleTable(pBattleTable, 8, pSlots, "Pre-UpdateBattleTable", false);
    }

    if (pSessionData) {
        SanitizeSessionMemberSlots(pSessionData, false, "Pre-UpdateBattleTable");
    }

    if (pBattleTable) {
        // 1b. Wipe any scrubbed/orphan uId rows left in BattleTable
        DeduplicateCustomLobbyBattleTable(pBattleTable, 8, pSlots, "Pre-UpdateBattleTable", true);

        // 2. Sanitize m_changeQueue ([pLobbyState + 0x448] .. [pLobbyState + 0x450], stride 0x258)
        __try {
            auto* pLobbyBytes = reinterpret_cast<uint8_t*>(pLobbyState);
            uint8_t* qBegin   = *reinterpret_cast<uint8_t**>(pLobbyBytes + 0x448);
            uint8_t* qEnd     = *reinterpret_cast<uint8_t**>(pLobbyBytes + 0x450);
            if (reinterpret_cast<uintptr_t>(qBegin) >= kMinValidPtr && qEnd > qBegin &&
                ((qEnd - qBegin) % 0x258) == 0 && ((qEnd - qBegin) / 0x258) <= 64) {
                const size_t qCount = static_cast<size_t>((qEnd - qBegin) / 0x258);
                for (size_t qi = 0; qi < qCount; ++qi) {
                    uint8_t* qEntry       = qBegin + qi * 0x258;
                    const int32_t qUId    = *reinterpret_cast<const int32_t*>(qEntry + 0x00);
                    const int32_t qStatus = *reinterpret_cast<const int32_t*>(qEntry + 0x04);
                    if (qUId < 0 || qStatus < 0) {
                        continue;
                    }

                    if (qStatus == 6) {
                        const uint64_t qSid = FindSlotSteamIdByUId(pSlots, qUId);
                        int32_t existingBtIdx = -1;
                        for (int32_t bi = 0; bi < 8; ++bi) {
                            const uint8_t* btEntry = pBattleTable + static_cast<size_t>(bi) * 0x258;
                            const int32_t btUId    = *reinterpret_cast<const int32_t*>(btEntry + 0x00);
                            const int32_t btStatus = *reinterpret_cast<const int32_t*>(btEntry + 0x04);
                            if (btUId < 0 || btStatus < 0) {
                                continue;
                            }
                            if (btUId == qUId ||
                                (IsValidSteamId(qSid) && FindSlotSteamIdByUId(pSlots, btUId) == qSid)) {
                                existingBtIdx = bi;
                                break;
                            }
                        }

                        if (existingBtIdx >= 0) {
                            *reinterpret_cast<int32_t*>(qEntry + 0x00) = -1;
                            *reinterpret_cast<int32_t*>(qEntry + 0x04) = -1;
                            const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                            if (saveNum <= 16) {
                                LogNetcode("[LobbyFix] Suppressed duplicate status=6 changeQueue insertion for uId=%d (already in BattleTable[%d])",
                                           qUId, existingBtIdx);
                            }
                            continue;
                        }
                    }

                    // Suppress duplicate (qUId, qStatus) entries in the same changeQueue batch (protects Loop 1 & Loop 3)
                    for (size_t qj = qi + 1; qj < qCount; ++qj) {
                        uint8_t* qNext = qBegin + qj * 0x258;
                        if (*reinterpret_cast<const int32_t*>(qNext + 0x00) == qUId &&
                            *reinterpret_cast<const int32_t*>(qNext + 0x04) == qStatus) {
                            *reinterpret_cast<int32_t*>(qNext + 0x00) = -1;
                            *reinterpret_cast<int32_t*>(qNext + 0x04) = -1;
                            g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    oCustomLobbyUpdateBattleTbl(pLobbyState);

    if (pBattleTable) {
        DeduplicateCustomLobbyBattleTable(pBattleTable, 8, pSlots, "Post-UpdateBattleTable", true);
    }
}

// Guards CustomLobby::BattleTable_ReceiveCallback (0x1400F6F30):
// Sanitizes m_slots & deduplicates LocalInfo->BattleTable[0..7] right after SetBattleTable (0x1406C39E0)
// so duplicate rows sent by an unmodded Host are cleared immediately on arrival.
void __fastcall hk_CustomLobbyRecvBattleTable(void* pLobbyState, void* pArg2, const void* pPacket) {
    if (!oCustomLobbyRecvBattleTbl) {
        return;
    }

    uint8_t* pBattleTable = nullptr;
    uint8_t* pSessionData = nullptr;
    const uint8_t* pSlots = nullptr;
    if (g_BaseAddress) {
        __try {
            const uintptr_t pNetMgr = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
            if (pNetMgr >= kMinValidPtr) {
                const uintptr_t pLocalInfo = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0x100);
                const uintptr_t pSessData  = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0xF8);
                if (pLocalInfo >= kMinValidPtr) {
                    pBattleTable = reinterpret_cast<uint8_t*>(pLocalInfo + 0xC8);
                }
                if (pSessData >= kMinValidPtr) {
                    // [pNetMgr + 0xF8] is ccNetSessionData* (m_slots[8] at +0x13C, NOT ccNetSession* +0x2C0).
                    pSessionData = reinterpret_cast<uint8_t*>(pSessData);
                    pSlots       = pSessionData + 0x13C;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    if (pBattleTable) {
        DeduplicateCustomLobbyBattleTable(pBattleTable, 8, pSlots, "Pre-RecvBattleTable", false);
    }
    if (pSessionData) {
        SanitizeSessionMemberSlots(pSessionData, false, "Pre-RecvBattleTable");
    }

    oCustomLobbyRecvBattleTbl(pLobbyState, pArg2, pPacket);

    if (pBattleTable) {
        DeduplicateCustomLobbyBattleTable(pBattleTable, 8, pSlots, "Post-RecvBattleTable", true);
    }
}

// Guards ccStateMachineCustomMatchLobbyUi::UpdateUserList (0x1400FF810) so duplicate uId/SteamID rows
// and orphan uId ghost cards can never be rendered in the Custom/Endless Lobby UI list.
// Note: 0x00FF810 takes 6 arguments (rcx=pLobbyUi, rdx=pSessionData, r8=pBattleTable, r9=pArg4,
// [rsp+0x28]=selectedIdx, [rsp+0x30]=pArg6); all 6 must be forwarded intact!
void __fastcall hk_CustomLobbyUiUpdateUserList(
    void* pLobbyUi, void* pSessionData, void* pBattleTable, void* pArg4, uint64_t selectedIdx, void* pArg6) {
    const uint8_t* pSlots = nullptr;
    auto* pDataBytes = (reinterpret_cast<uintptr_t>(pSessionData) >= kMinValidPtr)
        ? reinterpret_cast<uint8_t*>(pSessionData)
        : nullptr;
    if (pDataBytes) {
        pSlots = pDataBytes + 0x13C;
    }
    if (reinterpret_cast<uintptr_t>(pBattleTable) >= kMinValidPtr) {
        // Deduplicate BEFORE scrubbing m_slots so distinct uIds sharing a SteamID still match
        DeduplicateCustomLobbyBattleTable(reinterpret_cast<uint8_t*>(pBattleTable),
                                          8,
                                          pSlots,
                                          "CustomLobbyUi::UpdateUserList(pre)",
                                          false);
    }
    if (pDataBytes) {
        SanitizeSessionMemberSlots(pDataBytes, false, "CustomLobbyUi::UpdateUserList");
    }
    if (reinterpret_cast<uintptr_t>(pBattleTable) >= kMinValidPtr) {
        // Prune scrubbed/stale orphan uIds so UpdateUserList (0x00FF9E5) wipes frozen ghost cards cleanly!
        DeduplicateCustomLobbyBattleTable(reinterpret_cast<uint8_t*>(pBattleTable),
                                          8,
                                          pSlots,
                                          "CustomLobbyUi::UpdateUserList(0x00FF810)",
                                          true);
    }
    if (oCustomLobbyUiUpdateUsers) {
        __try {
            oCustomLobbyUiUpdateUsers(pLobbyUi, pSessionData, pBattleTable, pArg4, selectedIdx, pArg6);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Guards TeamContainer::GetCharacterByIndex (0x14094B3E0, faults @ +0x094B3ED `mov rax, [rax]`):
// Prevents 0xC0000005 when `pContainer` is out-of-bounds (e.g. player index == -1 at +0x0A3BA8D),
// destroyed (`~TeamContainer` freed `_Myhead` without zeroing `_Mysize`), or has `_Myhead == NULL`.
void* __fastcall hk_TeamContainerGetByIndex(void* pContainer, uint32_t index) {
    if (!oTeamContainerGetByIndex || index >= 64U) {
        return nullptr;
    }
    if (!ValidateTeamContainerTree(pContainer, true)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in TeamContainer::GetByIndex (+0x094B3ED)! pContainer=0x%llX index=%u",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pContainer)),
                       index);
        }
        return nullptr;
    }
    __try {
        return oTeamContainerGetByIndex(pContainer, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

// Guards TeamContainer::FindEntryByCharId (0x14094B830, faults @ +0x094B837 `mov rax, [rcx]`).
void* __fastcall hk_TeamContainerFindByCharId(void* pContainer, uint32_t charId) {
    if (!oTeamContainerFindByCharId) {
        return nullptr;
    }
    if (!ValidateTeamContainerTree(pContainer, false)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in TeamContainer::FindByCharId (+0x094B837)! pContainer=0x%llX charId=%u",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pContainer)),
                       charId);
        }
        return nullptr;
    }
    __try {
        return oTeamContainerFindByCharId(pContainer, charId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

// Guards TeamContainer::FindEntryByIndex (0x14094B8C0, faults @ +0x094B8D8 `mov rbx, [rax]`).
void* __fastcall hk_TeamContainerFindByIndex(void* pContainer, uint32_t index) {
    if (!oTeamContainerFindByIndex || index >= 64U) {
        return nullptr;
    }
    if (!ValidateTeamContainerTree(pContainer, false)) {
        const uint64_t saveNum = g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in TeamContainer::FindByIndex (+0x094B8D8)! pContainer=0x%llX index=%u",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pContainer)),
                       index);
        }
        return nullptr;
    }
    __try {
        return oTeamContainerFindByIndex(pContainer, index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.sessionDisconnectNullSaves.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

// Guards ccChatTemplateParamManager::ClearDynamic (0x140150C90, faults @ +0x0150CC8):
// 1) Unchecked caller LobbyClearDynamic (+0x19BE1E) may pass g_ChatTemplateMgr == NULL.
// 2) If chatTemplateParam.bin.xfbin was unloaded prior to ClearDynamic, m_ptrVec (+0x30..+0x38) holds
//    dangling pointers (and +0x150CC8 dereferences [rcx+4] BEFORE checking rcx == NULL at +0x150CCE).
void __fastcall hk_ChatTemplateClearDynamic(void* pManager) {
    const uintptr_t callerAddr = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t mgrAddr    = reinterpret_cast<uintptr_t>(pManager);
    if ((mgrAddr & 0x7ULL) != 0 || !IsUserRangeAccessible(mgrAddr, 0xA8, true) || !oChatTemplateClearDynamic) {
        const uint64_t saveNum = g_Telemetry.chatTemplateCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ChatTemplate::ClearDynamic (+0x0150CC8)! pManager=0x%llX caller=0x%llX.",
                       static_cast<unsigned long long>(mgrAddr),
                       static_cast<unsigned long long>(callerAddr));
        }
        return;
    }

    SanitizeChatTemplateVector(mgrAddr, 0x30, 0x20, true,  "m_ptrVec", callerAddr);
    SanitizeChatTemplateVector(mgrAddr, 0x90, 0x38, false, "m_dynVec", callerAddr);

    bool faulted = false;
    CallChatTemplateClearDynamicSafe(pManager, &faulted);
    if (faulted) {
        const uint64_t saveNum = g_Telemetry.chatTemplateCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Caught exception in ChatTemplate::ClearDynamic (+0x0150CC8)! pManager=0x%llX caller=0x%llX.",
                       static_cast<unsigned long long>(mgrAddr),
                       static_cast<unsigned long long>(callerAddr));
        }
    }
}

float __fastcall hk_SpSkillGetFloatParam1(void* pParamMgr, int32_t charCode, int32_t spAtkIndex) {
    if (!oSpSkillGetFloatParam1 || !ValidateSpSkillParamTree(pParamMgr)) {
        return 0.0f;
    }
    const int32_t safeIdx = SanitizeSpSkillIndex(
        spAtkIndex, kMaxSpSkillFloatSlots, charCode, "SpSkillCustomize::GetFloatParam1 (+0x070225B)", reinterpret_cast<uintptr_t>(_ReturnAddress()));
    __try {
        return oSpSkillGetFloatParam1(pParamMgr, charCode, safeIdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
        return 0.0f;
    }
}

float __fastcall hk_SpSkillGetFloatParam2(void* pParamMgr, int32_t charCode, int32_t spAtkIndex) {
    if (!oSpSkillGetFloatParam2 || !ValidateSpSkillParamTree(pParamMgr)) {
        return 0.0f;
    }
    const int32_t safeIdx = SanitizeSpSkillIndex(
        spAtkIndex, kMaxSpSkillFloatSlots, charCode, "SpSkillCustomize::GetFloatParam2 (+0x07022AB)", reinterpret_cast<uintptr_t>(_ReturnAddress()));
    __try {
        return oSpSkillGetFloatParam2(pParamMgr, charCode, safeIdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
        return 0.0f;
    }
}

const char* __fastcall hk_SpSkillGetMsgIdPtr(void* pParamMgr, int32_t charCode, int32_t spAtkIndex) {
    if (!oSpSkillGetMsgIdPtr || !ValidateSpSkillParamTree(pParamMgr)) {
        return nullptr;
    }
    const int32_t safeIdx = SanitizeSpSkillIndex(
        spAtkIndex, kMaxSpSkillMsgSlots, charCode, "SpSkillCustomize::GetMsgIdPtr (+0x07022FE)", reinterpret_cast<uintptr_t>(_ReturnAddress()));
    __try {
        return oSpSkillGetMsgIdPtr(pParamMgr, charCode, safeIdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
}

uint32_t __fastcall hk_SpSkillGetDwordParam(void* pParamMgr, int32_t charCode, int32_t spAtkIndex) {
    if (!oSpSkillGetDwordParam || !ValidateSpSkillParamTree(pParamMgr)) {
        return 0U;
    }
    const int32_t safeIdx = SanitizeSpSkillIndex(
        spAtkIndex, kMaxSpSkillMsgSlots, charCode, "SpSkillCustomize::GetDwordParam (+0x0702352)", reinterpret_cast<uintptr_t>(_ReturnAddress()));
    __try {
        return oSpSkillGetDwordParam(pParamMgr, charCode, safeIdx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
        return 0U;
    }
}

int32_t __fastcall hk_SpSkillGetMessageId(void* pParamMgr, int32_t charCode, int32_t spAtkIndex, void* pOutNuccString) {
    if (!oSpSkillGetMessageId || !pOutNuccString || !ValidateSpSkillParamTree(pParamMgr)) {
        return 0;
    }
    const int32_t safeIdx = SanitizeSpSkillIndex(
        spAtkIndex, kMaxSpSkillMsgSlots, charCode, "SpSkillCustomize::GetMessageId (+0x0702447)", reinterpret_cast<uintptr_t>(_ReturnAddress()));
    __try {
        return oSpSkillGetMessageId(pParamMgr, charCode, safeIdx, pOutNuccString);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.spSkillOobSaves.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
}

void* __fastcall hk_SaveDataSyncShadow() {
    SanitizeMainSavePreSetSpAtkIndices();
    return oSaveDataSyncShadow ? oSaveDataSyncShadow() : nullptr;
}

void ApplyCrashAndNetcodeBytePatches(bool applyCrashPatches, bool applyNetcodePatches) noexcept {
    if (applyCrashPatches || applyNetcodePatches) {
        ApplyVerifiedBytePatch(g_EnginePatches[0]); // PushResendRequest null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[1]); // ReceivePackets NACK null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[5]); // nuccFree at-exit heap corruption fix (+0x12732D8)
        ApplyVerifiedBytePatch(g_EnginePatches[6]); // ccStateChatWindow exit1 teardown order fix (+0x016A795)
        ApplyVerifiedBytePatch(g_EnginePatches[7]); // ccStateChatWindow exit2 teardown order fix (+0x016A8CB)
        ApplyVerifiedBytePatch(g_EnginePatches[8]); // ccStateChatWindow exit3 teardown order fix (+0x016AAE1)
        InstallLobbySlotNullFixBridge();            // LobbySlotNullFix bridge (+0x113D7E6) & State1SortFix (+0x113D736)
        InstallVfxCoordNullFixBridges();            // VFX m_pCoord TOCTOU null-deref guards (+0x132C99B + 6 twins)
        InstallNetFesLambdaVtableGuards();          // NetFes async HTTP lambda UAF & m_pUpdateTask guards (+0x0B6530B + 16 twins)
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
            nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!s_pLobbySlotTrampoline) {
            LogNetcode("[ERROR] VirtualAlloc failed for LobbySlotNullFix trampoline!");
            return false;
        }
    } else {
        DWORD rwProt = 0;
        if (!VirtualProtect(s_pLobbySlotTrampoline, 0x1000, PAGE_READWRITE, &rwProt)) {
            LogNetcode("[ERROR] VirtualProtect(RW) failed for LobbySlotNullFix trampoline!");
            return false;
        }
    }
    std::memset(s_pLobbySlotTrampoline, 0xCC, 128);

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
    // Entered via JMP from 0x14113D7E6 where RSP % 16 == 0; subtract 0x20 (32B shadow space)
    // so RSP remains 16-byte aligned before `call rax` (strict Windows x64 ABI compliance).
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x20;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xF1;
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xEA;
    *p++ = 0x48; *p++ = 0xB8;
    std::memcpy(p, &vaNullHandler, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xD0;
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x20;

    // 0x3D: done:
    *p++ = 0x48; *p++ = 0xB8;
    std::memcpy(p, &vaReturnAddr, 8); p += 8;
    *p++ = 0xFF; *p++ = 0xE0;

    const size_t stubLen = static_cast<size_t>(p - s_pLobbySlotTrampoline);
    DWORD rxProt = 0;
    if (!VirtualProtect(s_pLobbySlotTrampoline, 0x1000, PAGE_EXECUTE_READ, &rxProt)) {
        LogNetcode("[ERROR] VirtualProtect(RX) failed for LobbySlotNullFix trampoline!");
        VirtualFree(s_pLobbySlotTrampoline, 0, MEM_RELEASE);
        s_pLobbySlotTrampoline = nullptr;
        return false;
    }
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

    // Also patch the inlined qsort at +0x113D736 inside State1_Update so `rdi` is re-resolved
    // via FindSlotBySteamId after SortSlotsAndRefreshMySlot (fixing duplicate local player in lobby)
    auto* sortPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + StormRva::kPatch_LobbyState1SortFix);
    if (!s_LobbyState1SortApplied &&
        std::memcmp(sortPtr, kExpectedLobbyState1SortPrefix, sizeof(kExpectedLobbyState1SortPrefix)) == 0) {
        DWORD sortOldProt = 0;
        if (VirtualProtect(sortPtr, sizeof(kPatchedLobbyState1SortBytes), PAGE_EXECUTE_READWRITE, &sortOldProt)) {
            std::memcpy(s_OrigLobbyState1SortBytes, sortPtr, sizeof(s_OrigLobbyState1SortBytes));
            std::memcpy(sortPtr, kPatchedLobbyState1SortBytes, sizeof(kPatchedLobbyState1SortBytes));
            FlushInstructionCache(GetCurrentProcess(), sortPtr, sizeof(kPatchedLobbyState1SortBytes));
            DWORD sortDummyProt = 0;
            VirtualProtect(sortPtr, sizeof(kPatchedLobbyState1SortBytes), sortOldProt, &sortDummyProt);
            s_LobbyState1SortApplied = true;
            LogNetcode("[Patch] Applied  LobbyState1SortFix      (+0x113D736) @ VA 0x%llX (fixes stale rdi after sort)",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(sortPtr)));
        }
    }

    return true;
}

void RemoveLobbySlotNullFixBridge() noexcept {
    if (!g_BaseAddress) return;

    if (s_LobbyState1SortApplied) {
        auto* sortPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + StormRva::kPatch_LobbyState1SortFix);
        DWORD sortOldProt = 0;
        if (VirtualProtect(sortPtr, sizeof(s_OrigLobbyState1SortBytes), PAGE_EXECUTE_READWRITE, &sortOldProt)) {
            std::memcpy(sortPtr, s_OrigLobbyState1SortBytes, sizeof(s_OrigLobbyState1SortBytes));
            FlushInstructionCache(GetCurrentProcess(), sortPtr, sizeof(s_OrigLobbyState1SortBytes));
            DWORD sortDummyProt = 0;
            VirtualProtect(sortPtr, sizeof(s_OrigLobbyState1SortBytes), sortOldProt, &sortDummyProt);
        }
        s_LobbyState1SortApplied = false;
    }

    if (!s_LobbySlotBridgeApplied) return;
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

// ============================================================================
// VFX nuccCoord TOCTOU Null-Deref Bridges (fixes 0xC0000005 @ +0x0132C99B + 6 twins)
// In nuccTrailForceField::Update (+0x0132C830 -> +0x0132C99B in NSUNSC.exe.19008.dmp)
// and 6 identical VFX forcefield/particle sites, the game tests `m_pCoord != NULL`,
// calls `m_pCoord->UpdateWorldMatrix()` (`call [rax+0x28]`), then reloads `m_pCoord`
// from memory WITHOUT re-checking NULL before `movzx ..., byte ptr [reg + 0x120]`.
// When nuccTrailBase::Detach (+0x01328210 -> DetachCoord +0x0132CEE0) zeroes m_pCoord
// concurrently on another thread, the worker reads [0 + 0x120] -> 0xC0000005.
// Solution: 2-page W^X near-2GB trampoline (Page 0 = RX stubs, Page 1 = RW atomic
// counters) with 5-byte `jmp rel32` detours checking `cmp reg, 0x10000; jb nullSkip`.
// ============================================================================

namespace {

struct VfxCoordGuardSite {
    const char* name;
    uintptr_t   patchRva;
    uintptr_t   resumeRva;
    uintptr_t   nullSkipRva;
    bool        checkRdx; // false = check RCX, true = check RDX
    uint8_t     reloadLen;
    uint8_t     reloadBytes[8];
    uint8_t     derefLen;
    uint8_t     derefBytes[16];
    bool        applied;
};

VfxCoordGuardSite g_VfxCoordSites[] = {
    // 0) nuccTrailForceField::Update (+0x0132C997, faults @ +0x0132C99B in NSUNSC.exe.19008.dmp)
    {
        "nuccTrailForceField::Update (+0x132C99B)",
        0x0132C997, 0x0132C9A2, 0x0132C9E5, false,
        4, { 0x49, 0x8B, 0x4F, 0x28 },
        7, { 0x0F, 0xB6, 0x81, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 1) VFX m_pCoord Twin #1 (+0x0131C6CD -> +0x0131C6D0)
    {
        "VfxCoordGuard Twin1 (+0x131C6D0)",
        0x0131C6CD, 0x0131C6D7, 0x0131C84B, false,
        3, { 0x48, 0x8B, 0x0B },
        7, { 0x0F, 0xB6, 0x81, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 2) VFX m_pCoord Twin #2 (+0x0131C990 -> +0x0131C993; nullSkip -> +0x0131C9C5 restores xmm1..xmm3!)
    {
        "VfxCoordGuard Twin2 (+0x131C993)",
        0x0131C990, 0x0131C99A, 0x0131C9C5, false,
        3, { 0x48, 0x8B, 0x0B },
        7, { 0x0F, 0xB6, 0x81, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 3) VFX m_pCoord Twin #3 (+0x0131CD7A -> +0x0131CD7D)
    {
        "VfxCoordGuard Twin3 (+0x131CD7D)",
        0x0131CD7A, 0x0131CD84, 0x0131CDCC, false,
        3, { 0x48, 0x8B, 0x0E },
        7, { 0x0F, 0xB6, 0x99, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 4) VFX m_pCoord Twin #4 (+0x01385040 -> +0x01385048)
    {
        "VfxCoordGuard Twin4 (+0x1385048)",
        0x01385040, 0x0138504F, 0x01385090, false,
        4, { 0x48, 0x8B, 0x4B, 0x40 },
        11, { 0x48, 0x8D, 0x55, 0xB7, 0x0F, 0xB6, 0xB9, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 5) VFX m_pCoord Twin #5 (+0x01387B5C -> +0x01387B60)
    {
        "VfxCoordGuard Twin5 (+0x1387B60)",
        0x01387B5C, 0x01387B67, 0x01387B91, true,
        4, { 0x48, 0x8B, 0x53, 0x40 },
        7, { 0x0F, 0xB6, 0x8A, 0x20, 0x01, 0x00, 0x00 },
        false
    },
    // 6) VFX m_pCoord Twin #6 (+0x01387BDB -> +0x01387BDE)
    {
        "VfxCoordGuard Twin6 (+0x1387BDE)",
        0x01387BDB, 0x01387BE5, 0x01387C09, true,
        3, { 0x48, 0x8B, 0x13 },
        7, { 0x0F, 0xB6, 0x8A, 0x20, 0x01, 0x00, 0x00 },
        false
    },
};

uint8_t*  s_pVfxCoordTrampoline = nullptr; // 2 pages: [0..0xFFF] = RX code, [0x1000..0x1FFF] = RW counters
uint64_t* s_pVfxCoordCounters   = nullptr;

uint8_t* AllocateNear2GbTwoPages(uintptr_t anchorVa) noexcept {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran    = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000ULL;
    const uintptr_t sysMin  = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t sysMax  = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    constexpr uintptr_t kMaxSpan = 0x60000000ULL; // ±1.5 GB keeps every rel32 well inside ±2 GB
    const uintptr_t minAddr = (anchorVa > sysMin + kMaxSpan) ? (anchorVa - kMaxSpan) : sysMin;
    const uintptr_t maxAddr = (anchorVa + kMaxSpan < sysMax) ? (anchorVa + kMaxSpan) : sysMax;

    auto tryAllocRange = [gran](uintptr_t start, uintptr_t end, bool downward) -> uint8_t* {
        if (!downward) {
            uintptr_t addr = (start + gran - 1) & ~(gran - 1);
            while (addr + 0x2000ULL <= end) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
                    break;
                }
                const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                const uintptr_t regionEnd  = regionBase + mbi.RegionSize;
                if (mbi.State == MEM_FREE) {
                    const uintptr_t aligned = (addr + gran - 1) & ~(gran - 1);
                    if (aligned + 0x2000ULL <= regionEnd && aligned + 0x2000ULL <= end) {
                        void* p = VirtualAlloc(reinterpret_cast<LPVOID>(aligned), 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                        if (p) return reinterpret_cast<uint8_t*>(p);
                    }
                }
                if (regionEnd <= addr) break;
                addr = (regionEnd + gran - 1) & ~(gran - 1);
            }
        } else {
            uintptr_t addr = end & ~(gran - 1);
            while (addr >= start + gran) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
                    break;
                }
                const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                if (mbi.State == MEM_FREE && mbi.RegionSize >= 0x2000ULL) {
                    const uintptr_t aligned = (std::min(addr, regionBase + mbi.RegionSize - 0x2000ULL)) & ~(gran - 1);
                    if (aligned >= regionBase && aligned >= start) {
                        void* p = VirtualAlloc(reinterpret_cast<LPVOID>(aligned), 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                        if (p) return reinterpret_cast<uint8_t*>(p);
                    }
                }
                if (regionBase < start + gran) break;
                addr = (regionBase - gran) & ~(gran - 1);
            }
        }
        return nullptr;
    };

    if (uint8_t* p = tryAllocRange(anchorVa, maxAddr, false)) return p;
    return tryAllocRange(minAddr, anchorVa, true);
}

bool FitsRel32(uintptr_t srcNextVa, uintptr_t dstVa, int32_t* outRel) noexcept {
    const int64_t diff = static_cast<int64_t>(dstVa) - static_cast<int64_t>(srcNextVa);
    if (diff < INT32_MIN || diff > INT32_MAX) {
        return false;
    }
    *outRel = static_cast<int32_t>(diff);
    return true;
}

} // namespace

bool InstallVfxCoordNullFixBridges() {
    if (!g_BaseAddress) return false;
    if (s_pVfxCoordTrampoline) return true;

    s_pVfxCoordTrampoline = AllocateNear2GbTwoPages(g_BaseAddress + 0x0132C997ULL);
    if (!s_pVfxCoordTrampoline) {
        LogNetcode("[ERROR] Failed to allocate near-2GB trampoline pages for VfxCoordNullFix!");
        return false;
    }
    std::memset(s_pVfxCoordTrampoline, 0xCC, 0x1000);
    std::memset(s_pVfxCoordTrampoline + 0x1000, 0x00, 0x1000);
    s_pVfxCoordCounters = reinterpret_cast<uint64_t*>(s_pVfxCoordTrampoline + 0x1000);

    // Emit all 7 stubs on Page 0 first, verify every byte & rel32 before flipping to RX
    uint8_t* stubCursor = s_pVfxCoordTrampoline;
    uintptr_t stubVas[std::size(g_VfxCoordSites)] = {};
    bool siteReady[std::size(g_VfxCoordSites)]    = {};

    for (size_t i = 0; i < std::size(g_VfxCoordSites); ++i) {
        auto& site = g_VfxCoordSites[i];
        const size_t totalLen = static_cast<size_t>(site.reloadLen) + static_cast<size_t>(site.derefLen);
        const auto* targetPtr = reinterpret_cast<const uint8_t*>(g_BaseAddress + site.patchRva);
        if (std::memcmp(targetPtr, site.reloadBytes, site.reloadLen) != 0 ||
            std::memcmp(targetPtr + site.reloadLen, site.derefBytes, site.derefLen) != 0) {
            LogNetcode("[WARN] Byte mismatch for %s @ RVA 0x%llX; skipping.",
                       site.name, static_cast<unsigned long long>(site.patchRva));
            continue;
        }

        // Align each stub to 16 bytes on Page 0
        const uintptr_t alignedAddr = (reinterpret_cast<uintptr_t>(stubCursor) + 15ULL) & ~15ULL;
        uint8_t* p = reinterpret_cast<uint8_t*>(alignedAddr);
        const uintptr_t stubVa     = alignedAddr;
        const uintptr_t resumeVa   = g_BaseAddress + site.resumeRva;
        const uintptr_t nullSkipVa = g_BaseAddress + site.nullSkipRva;
        const uintptr_t counterVa  = reinterpret_cast<uintptr_t>(&s_pVfxCoordCounters[i]);

        int32_t relToStub = 0;
        if (!FitsRel32(g_BaseAddress + site.patchRva + 5ULL, stubVa, &relToStub)) {
            LogNetcode("[WARN] Rel32 out of range for %s; skipping.", site.name);
            continue;
        }

        // 1. Original reload instruction (e.g. `mov rcx, [r15 + 0x28]`)
        std::memcpy(p, site.reloadBytes, site.reloadLen); p += site.reloadLen;
        // 2. `cmp rcx/rdx, 0x10000` (catches NULL and invalid low pointers with 0 GPR clobbers)
        *p++ = 0x48; *p++ = 0x81; *p++ = site.checkRdx ? 0xFA : 0xF9;
        *p++ = 0x00; *p++ = 0x00; *p++ = 0x01; *p++ = 0x00;
        // 3. `jb null_path` (skips derefBytes + 5-byte jmp resume)
        *p++ = 0x72; *p++ = static_cast<uint8_t>(site.derefLen + 5u);
        // 4. Original deref bytes (e.g. `movzx eax, byte ptr [rcx + 0x120]`)
        std::memcpy(p, site.derefBytes, site.derefLen); p += site.derefLen;
        // 5. `jmp rel32 resumeVa`
        int32_t relResume = 0;
        if (!FitsRel32(reinterpret_cast<uintptr_t>(p) + 5ULL, resumeVa, &relResume)) {
            continue;
        }
        *p++ = 0xE9;
        std::memcpy(p, &relResume, 4); p += 4;
        // 6. null_path: `lock inc qword ptr [rip + disp32]` (8 bytes, 0 GPR clobbers)
        int32_t dispCounter = 0;
        if (!FitsRel32(reinterpret_cast<uintptr_t>(p) + 8ULL, counterVa, &dispCounter)) {
            continue;
        }
        *p++ = 0xF0; *p++ = 0x48; *p++ = 0xFF; *p++ = 0x05;
        std::memcpy(p, &dispCounter, 4); p += 4;
        // 7. `jmp rel32 nullSkipVa`
        int32_t relNullSkip = 0;
        if (!FitsRel32(reinterpret_cast<uintptr_t>(p) + 5ULL, nullSkipVa, &relNullSkip)) {
            continue;
        }
        *p++ = 0xE9;
        std::memcpy(p, &relNullSkip, 4); p += 4;

        stubCursor   = p;
        stubVas[i]   = stubVa;
        siteReady[i] = true;
        (void)totalLen;
    }

    // Strict W^X: switch Page 0 to PAGE_EXECUTE_READ before patching live code
    DWORD rxOldProt = 0;
    if (!VirtualProtect(s_pVfxCoordTrampoline, 0x1000, PAGE_EXECUTE_READ, &rxOldProt)) {
        LogNetcode("[ERROR] VirtualProtect(RX) failed for VfxCoordNullFix trampoline!");
        VirtualFree(s_pVfxCoordTrampoline, 0, MEM_RELEASE);
        s_pVfxCoordTrampoline = nullptr;
        s_pVfxCoordCounters   = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), s_pVfxCoordTrampoline, 0x1000);

    int appliedCount = 0;
    for (size_t i = 0; i < std::size(g_VfxCoordSites); ++i) {
        if (!siteReady[i]) continue;
        auto& site = g_VfxCoordSites[i];
        const size_t totalLen = static_cast<size_t>(site.reloadLen) + static_cast<size_t>(site.derefLen);
        auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + site.patchRva);

        int32_t relToStub = 0;
        if (!FitsRel32(g_BaseAddress + site.patchRva + 5ULL, stubVas[i], &relToStub)) {
            continue;
        }

        uint8_t patchBuf[24];
        patchBuf[0] = 0xE9;
        std::memcpy(&patchBuf[1], &relToStub, 4);
        for (size_t b = 5; b < totalLen; ++b) {
            patchBuf[b] = 0x90;
        }

        DWORD oldProt = 0;
        if (VirtualProtect(targetPtr, totalLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(targetPtr, patchBuf, totalLen);
            FlushInstructionCache(GetCurrentProcess(), targetPtr, totalLen);
            DWORD dummyProt = 0;
            VirtualProtect(targetPtr, totalLen, oldProt, &dummyProt);
            site.applied = true;
            ++appliedCount;
        }
    }

    LogNetcode("[Patch] Applied  VfxCoordNullFix Bridges (%d/%zu TOCTOU sites guarded: +0x132C99B + 6 twins -> RX 0x%llX)",
               appliedCount, std::size(g_VfxCoordSites),
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s_pVfxCoordTrampoline)));
    return appliedCount > 0;
}

void RemoveVfxCoordNullFixBridges() noexcept {
    if (!g_BaseAddress) return;
    for (auto& site : g_VfxCoordSites) {
        if (!site.applied) continue;
        const size_t totalLen = static_cast<size_t>(site.reloadLen) + static_cast<size_t>(site.derefLen);
        auto* targetPtr = reinterpret_cast<uint8_t*>(g_BaseAddress + site.patchRva);
        DWORD oldProt = 0;
        if (VirtualProtect(targetPtr, totalLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
            std::memcpy(targetPtr, site.reloadBytes, site.reloadLen);
            std::memcpy(targetPtr + site.reloadLen, site.derefBytes, site.derefLen);
            FlushInstructionCache(GetCurrentProcess(), targetPtr, totalLen);
            DWORD dummyProt = 0;
            VirtualProtect(targetPtr, totalLen, oldProt, &dummyProt);
        }
        site.applied = false;
    }
    if (s_pVfxCoordTrampoline) {
        VirtualFree(s_pVfxCoordTrampoline, 0, MEM_RELEASE);
        s_pVfxCoordTrampoline = nullptr;
        s_pVfxCoordCounters   = nullptr;
    }
}

uint64_t GetVfxCoordNullSaves() noexcept {
    if (!s_pVfxCoordCounters) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < std::size(g_VfxCoordSites); ++i) {
        sum += reinterpret_cast<const volatile uint64_t*>(s_pVfxCoordCounters)[i];
    }
    return sum;
}

// ============================================================================
// ccTaskManager Iterator-Invalidation UAF, Parallel Lock & ccTask::Exec Armor
// Fixes 0xC0000005 @ NSUNSC.exe+0x0109C8E0 (`cmp dword ptr [rcx + 0x20], 0`):
//   1) UpdateRange (+0x10D6A6C) advances `mov rbx, [rbx]` AFTER `call ccTask::Exec`,
//      while RemoveTask (+0x10D7060) immediately zeroes `[node]` and frees `node`.
//   2) AddTask (+0x10D6200) & RemoveTask (+0x10D6FE0) never lock `[pMgr + 0x1470]`
//      (`nuccCriticalSection`), racing against parallel workers in ExecParallelTask
//      (+0x10D6640) and invalidating `m_pParallelIterNode` (`[pMgr + 0x14C8]`).
// Solution:
//   - Synchronize AddTask/RemoveTask/DeleteAllTasks with `[pMgr + 0x1470]`.
//   - While UpdateRange / UpdateSingleParallel / m_bParallelActive is active,
//     RemoveTask replaces `node->pTask` with a per-priority static tombstone whose
//     `m_bActive` (+0x20) == 0 (skipped in 2 cycles by ccTask::Exec) WITHOUT freeing
//     the list node, then reaps dirty buckets natively via oTaskMgrRemoveTask once
//     iteration exits (0 leaks, handles self-removal & sibling-removal & ABA cleanly).
//   - Guard ccTask::Exec (+0x109C8E0) against NULL / dangling task or vtable ptrs.
// ============================================================================

namespace {

struct alignas(16) TombstoneTask {
    uintptr_t vtable;     // +0x00
    uint8_t   pad08[16];  // +0x08..+0x17
    uint32_t  priority;   // +0x18 (read by vanilla RemoveTask at +0x10D7004)
    uint32_t  pauseBits;  // +0x1C
    int32_t   activeFlag; // +0x20 (0 -> ccTask::Exec skips immediately)
    uint32_t  pad24;      // +0x24
    uint64_t  pad28;      // +0x28..+0x2F (explicit 16-byte alignment multiple = 0x30)
};
static_assert(sizeof(TombstoneTask) == 0x30, "TombstoneTask size mismatch");
static_assert(offsetof(TombstoneTask, priority) == 0x18, "TombstoneTask::priority offset mismatch");
static_assert(offsetof(TombstoneTask, activeFlag) == 0x20, "TombstoneTask::activeFlag offset mismatch");

void* __fastcall TombstoneNoOpScalarDtor(void* pThis, uint32_t) noexcept {
    return pThis;
}

void __fastcall TombstoneNoOpVirtual(void*) noexcept {}

const uintptr_t kSafeTombstoneVtable[8] = {
    reinterpret_cast<uintptr_t>(&TombstoneNoOpScalarDtor),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
    reinterpret_cast<uintptr_t>(&TombstoneNoOpVirtual),
};

TombstoneTask         s_TombstoneTasks[128]    = {};
std::atomic<int32_t>  s_TaskMgrUpdateDepth{0};
std::atomic<int32_t>  s_TaskMgrActiveDeferrals{0};
std::atomic<void*>    s_LastTombstonedMgr{nullptr};
std::atomic<uint64_t> s_TaskMgrDirtyMask[2]    = {};
std::atomic<uint64_t> s_TaskMgrDeferredCount{0};

bool TryLockTaskMgrCs(void* pMgr) noexcept {
    if (!g_fnNuccCsLock || !g_fnNuccCsUnlock || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) {
        return false;
    }
    auto* pBytes = reinterpret_cast<uint8_t*>(pMgr);
    if (*reinterpret_cast<const int32_t*>(pBytes + 0x1498) == 0) {
        return false; // nuccCriticalSection (+0x1470) not initialized yet
    }
    g_fnNuccCsLock(pBytes + 0x1470);
    return true;
}

void UnlockTaskMgrCs(void* pMgr, bool locked) noexcept {
    if (locked && g_fnNuccCsUnlock && reinterpret_cast<uintptr_t>(pMgr) >= kMinValidPtr) {
        g_fnNuccCsUnlock(reinterpret_cast<uint8_t*>(pMgr) + 0x1470);
    }
}

void ReapTombstonedTaskNodesLocked(void* pMgr) noexcept {
    if (!oTaskMgrRemoveTask || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) {
        return;
    }
    void* const ownerMgr = s_LastTombstonedMgr.load(std::memory_order_acquire);
    if (ownerMgr != nullptr && ownerMgr != pMgr) {
        return;
    }
    const uint64_t mask0 = s_TaskMgrDirtyMask[0].exchange(0ULL, std::memory_order_acq_rel);
    const uint64_t mask1 = s_TaskMgrDirtyMask[1].exchange(0ULL, std::memory_order_acq_rel);
    if ((mask0 | mask1) == 0ULL) {
        return;
    }

    auto reapWord = [&](uint64_t bits, uint32_t basePrio) noexcept {
        unsigned long bitIdx = 0;
        while (_BitScanForward64(&bitIdx, bits)) {
            bits &= (bits - 1ULL);
            const uint32_t prio = basePrio + static_cast<uint32_t>(bitIdx);
            oTaskMgrRemoveTask(pMgr, &s_TombstoneTasks[prio]);
        }
    };
    reapWord(mask0, 0u);
    reapWord(mask1, 64u);
}

void ReapTombstonedTaskNodes(void* pMgr) noexcept {
    // Dekker-ordered wait: if a concurrent RemoveTask observed s_TaskMgrUpdateDepth > 0 just before
    // our fetch_sub(1), let it finish setting s_TaskMgrDirtyMask (~50ns) so we reap on this exact frame.
    while (s_TaskMgrActiveDeferrals.load(std::memory_order_seq_cst) > 0) {
        _mm_pause();
    }
    if ((s_TaskMgrDirtyMask[0].load(std::memory_order_acquire) |
         s_TaskMgrDirtyMask[1].load(std::memory_order_acquire)) == 0ULL) {
        return;
    }
    void* const ownerMgr = s_LastTombstonedMgr.load(std::memory_order_acquire);
    if (ownerMgr != nullptr && ownerMgr != pMgr) {
        return;
    }
    const bool locked = TryLockTaskMgrCs(pMgr);
    ReapTombstonedTaskNodesLocked(pMgr);
    UnlockTaskMgrCs(pMgr, locked);
}

bool TombstoneTaskInBucketSeh(void* pMgr, void* pTask, uint32_t prio) noexcept {
    bool tombstoned = false;
    __try {
        auto* pMgrBytes  = reinterpret_cast<uint8_t*>(pMgr);
        uint8_t* pHead   = *reinterpret_cast<uint8_t**>(pMgrBytes + static_cast<size_t>(prio) * 0x28 + 0x30);
        if (reinterpret_cast<uintptr_t>(pHead) >= kMinValidPtr) {
            uint8_t* node = *reinterpret_cast<uint8_t**>(pHead + 0x00);
            for (uint32_t guard = 0;
                 guard < 65536u && reinterpret_cast<uintptr_t>(node) >= kMinValidPtr && node != pHead;
                 ++guard) {
                uint8_t* nextNode = *reinterpret_cast<uint8_t**>(node + 0x00);
                if (*reinterpret_cast<void**>(node + 0x10) == pTask) {
                    *reinterpret_cast<void**>(node + 0x10) = &s_TombstoneTasks[prio];
                    tombstoned = true;
                }
                node = nextNode;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Telemetry.taskManagerCrashSaves.fetch_add(1, std::memory_order_relaxed);
    }
    return tombstoned;
}

bool ReadTaskExecTargetSeh(void* pTask, uint32_t pauseMask, uintptr_t* outFn, bool* outFaulted) noexcept {
    *outFn      = 0;
    *outFaulted = false;
    __try {
        const auto* pBytes = reinterpret_cast<const uint8_t*>(pTask);
        if (*reinterpret_cast<const int32_t*>(pBytes + 0x20) == 0) {
            return false; // Inactive task or deferred tombstone
        }
        if ((*reinterpret_cast<const uint32_t*>(pBytes + 0x1C) & pauseMask) != 0u) {
            return false; // Task paused by current pause mask
        }
        const uintptr_t vtbl = *reinterpret_cast<const uintptr_t*>(pBytes + 0x00);
        if (vtbl < kMinValidPtr) {
            *outFaulted = true;
            return false;
        }
        const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(vtbl + 0x18);
        if (fn < kMinValidPtr) {
            *outFaulted = true;
            return false;
        }
        *outFn = fn;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outFaulted = true;
        return false;
    }
}

bool ReadTaskPrioritySeh(void* pTask, uint32_t* outPrio) noexcept {
    *outPrio = 0xFFFFFFFFu;
    __try {
        *outPrio = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pTask) + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void InitTaskManagerTombstones() noexcept {
    for (uint32_t i = 0; i < 128u; ++i) {
        s_TombstoneTasks[i].vtable     = reinterpret_cast<uintptr_t>(kSafeTombstoneVtable);
        std::memset(s_TombstoneTasks[i].pad08, 0, sizeof(s_TombstoneTasks[i].pad08));
        s_TombstoneTasks[i].priority   = i;
        s_TombstoneTasks[i].pauseBits  = 0;
        s_TombstoneTasks[i].activeFlag = 0;
        s_TombstoneTasks[i].pad24      = 0;
    }
}

void __fastcall hk_TaskExec(void* pTask, uint32_t pauseMask) {
    const uintptr_t taskAddr = reinterpret_cast<uintptr_t>(pTask);
    if (taskAddr < kMinValidPtr) {
        const uint64_t saveNum = g_Telemetry.taskManagerCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ccTask::Exec (+0x0109C8E0)! Invalid pTask=0x%llX pauseMask=0x%X",
                       static_cast<unsigned long long>(taskAddr), pauseMask);
        }
        return;
    }

    uintptr_t execFn  = 0;
    bool      faulted = false;
    if (ReadTaskExecTargetSeh(pTask, pauseMask, &execFn, &faulted)) {
        reinterpret_cast<void(__fastcall*)(void*, uint32_t)>(execFn)(pTask, pauseMask);
        return;
    }

    if (faulted) {
        const uint64_t saveNum = g_Telemetry.taskManagerCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ccTask::Exec (+0x0109C8E0)! Dangling/corrupt pTask=0x%llX pauseMask=0x%X",
                       static_cast<unsigned long long>(taskAddr), pauseMask);
        }
    }
}

uint32_t __fastcall hk_TaskMgrAddTask(void* pMgr, void* pTask) {
    if (!oTaskMgrAddTask || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr ||
        reinterpret_cast<uintptr_t>(pTask) < kMinValidPtr) {
        return 0u;
    }
    const bool locked = TryLockTaskMgrCs(pMgr);
    const uint32_t rc = oTaskMgrAddTask(pMgr, pTask);
    UnlockTaskMgrCs(pMgr, locked);
    return rc;
}

void __fastcall hk_TaskMgrUpdateRange(void* pMgr, uint32_t startPrio, uint32_t endPrio, uint32_t pauseFlag) {
    if (!oTaskMgrUpdateRange || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) {
        return;
    }
    s_TaskMgrUpdateDepth.fetch_add(1, std::memory_order_seq_cst);
    oTaskMgrUpdateRange(pMgr, startPrio, endPrio, pauseFlag);
    if (s_TaskMgrUpdateDepth.fetch_sub(1, std::memory_order_seq_cst) == 1) {
        ReapTombstonedTaskNodes(pMgr);
    }
}

void __fastcall hk_TaskMgrUpdateSingleParallel(void* pMgr, uint32_t priority, uint32_t pauseFlag) {
    if (!oTaskMgrUpdateSinglePar || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) {
        return;
    }
    s_TaskMgrUpdateDepth.fetch_add(1, std::memory_order_seq_cst);
    oTaskMgrUpdateSinglePar(pMgr, priority, pauseFlag);
    if (s_TaskMgrUpdateDepth.fetch_sub(1, std::memory_order_seq_cst) == 1) {
        ReapTombstonedTaskNodes(pMgr);
    }
}

void __fastcall hk_TaskMgrRemoveTask(void* pMgr, void* pTask) {
    if (!oTaskMgrRemoveTask || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr ||
        reinterpret_cast<uintptr_t>(pTask) < kMinValidPtr) {
        return;
    }

    uint32_t prio = 0xFFFFFFFFu;
    if (!ReadTaskPrioritySeh(pTask, &prio)) {
        const uint64_t saveNum = g_Telemetry.taskManagerCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in ccTaskManager::RemoveTask! Unreadable pTask=0x%llX",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pTask)));
        }
        return;
    }
    if (prio >= 128u) {
        return;
    }

    const bool locked = TryLockTaskMgrCs(pMgr);
    s_TaskMgrActiveDeferrals.fetch_add(1, std::memory_order_seq_cst);
    const auto* pMgrBytes = reinterpret_cast<const uint8_t*>(pMgr);
    const bool  iterating = (s_TaskMgrUpdateDepth.load(std::memory_order_seq_cst) > 0) ||
                            (*reinterpret_cast<const int32_t*>(pMgrBytes + 0x14D8) != 0);

    if (iterating) {
        if (TombstoneTaskInBucketSeh(pMgr, pTask, prio)) {
            s_LastTombstonedMgr.store(pMgr, std::memory_order_release);
            s_TaskMgrDirtyMask[prio >> 6].fetch_or(1ULL << (prio & 63u), std::memory_order_seq_cst);
            g_Telemetry.taskManagerCrashSaves.fetch_add(1, std::memory_order_relaxed);
            const uint64_t deferCount = s_TaskMgrDeferredCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (deferCount <= 4) {
                LogNetcode("[CrashFix] Deferred ccTaskManager::RemoveTask during active iteration (prio=%u, totalDeferred=%llu) — iterator UAF prevented.",
                           prio, static_cast<unsigned long long>(deferCount));
            }
        }
        s_TaskMgrActiveDeferrals.fetch_sub(1, std::memory_order_seq_cst);
        UnlockTaskMgrCs(pMgr, locked);
        return;
    }
    s_TaskMgrActiveDeferrals.fetch_sub(1, std::memory_order_seq_cst);

    oTaskMgrRemoveTask(pMgr, pTask);
    UnlockTaskMgrCs(pMgr, locked);
}

void __fastcall hk_TaskMgrDeleteAllTasks(void* pMgr) {
    if (!oTaskMgrDeleteAllTasks || reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) {
        return;
    }
    const bool locked = TryLockTaskMgrCs(pMgr);
    // Reap any deferred tombstones BEFORE oTaskMgrDeleteAllTasks runs virtual destructors on bucket nodes!
    ReapTombstonedTaskNodesLocked(pMgr);
    oTaskMgrDeleteAllTasks(pMgr);
    UnlockTaskMgrCs(pMgr, locked);
}

// ============================================================================
// NetFes (ccNetEvergreenFesParamManager & ccNetFesParamManager) Async Callback
// & ccTaskCertainExe ("ccNetFesParamManager::Update") UAF Armor
//
// Fixes deterministic 0xC0000005 @ NSUNSC.exe+0x00B6530B (`call qword ptr [rax + 0x20]`)
// and all 16 twin std::_Func_impl_no_alloc HTTP stats callbacks in NetFes:
//   1) All 9 ccNetEvergreenFesParamManager lambdas (.rdata +0x19987E0..+0x19989A0) and
//      8 ccNetFesParamManager lambdas (.rdata +0x19995B8..+0x1999740) capture only a raw
//      `this` pointer at `[pFuncImpl + 0x08]` with no cancellation or weak_ptr. When
//      ccDataSet tears down/recreates the manager (+0x05B61AF / +0x05B61CC) while a Bandai
//      Namco stats request (+0x0BB7BF0) is in flight or timing out ("networkmode_089" /
//      "networkmode_1073"), `_Do_call` writes into freed memory and dereferences `m_pUpdateTask`.
//   2) If `ccTaskManager::DeleteAllTasks` (+0x10D70E0) or external cleanup destroys the
//      0x30-byte `ccTaskCertainExe` ("ccNetFesParamManager::Update", vtable +0x1761A08)
//      via `vtable[0]` (+0x00B57B0) while the manager is still alive, `[mgr + 0xB8]`
//      (EvergreenFes) / `[mgr + 0x88]` (Fes) is left pointing to a freed nuccHeap block
//      whose `[rcx]` vtable word is 0 -> `mov rax, [rcx]` loads 0, and `call [rax + 0x20]`
//      faults at +0x00B6530B (and causes a later double-free in ~dtor +0x0B64C7B / +0x0B6E45B).
// ============================================================================

namespace {

constexpr size_t kEvergreenFesSize       = 0x5D0;
constexpr size_t kEvergreenFesTaskOffset = 0x0B8;
constexpr size_t kNetFesSize             = 0x4D0;
constexpr size_t kNetFesTaskOffset       = 0x088;

struct alignas(16) InertFesTask {
    uintptr_t vtable;     // +0x00
    uint8_t   pad08[16];  // +0x08..+0x17
    uint32_t  priority;   // +0x18 (0x42)
    uint32_t  pauseBits;  // +0x1C
    int32_t   activeFlag; // +0x20 (written by vtable[4] SetActive)
    uint32_t  pad24;      // +0x24
    uintptr_t pCommand;   // +0x28
};
static_assert(sizeof(InertFesTask) == 0x30, "InertFesTask must match 0x30-byte ccTaskCertainExe");
static_assert(offsetof(InertFesTask, activeFlag) == 0x20, "InertFesTask::activeFlag offset mismatch");

extern InertFesTask s_InertFesTask;

void* __fastcall InertFesTaskNoOpScalarDtor(void* pThis, uint32_t) noexcept {
    return pThis;
}

uintptr_t __fastcall InertFesTaskNoOp(void*) noexcept {
    return 0;
}

void __fastcall InertFesTaskNoOpExec(void*, uint32_t) noexcept {}

void __fastcall InertFesTaskSetActive(void* pTask, int32_t active) noexcept {
    if (pTask == &s_InertFesTask) {
        s_InertFesTask.activeFlag = active;
    }
}

const uintptr_t kInertFesTaskVtable[6] = {
    reinterpret_cast<uintptr_t>(&InertFesTaskNoOpScalarDtor), // +0x00: scalar deleting dtor
    reinterpret_cast<uintptr_t>(&InertFesTaskNoOp),           // +0x08
    reinterpret_cast<uintptr_t>(&InertFesTaskNoOp),           // +0x10
    reinterpret_cast<uintptr_t>(&InertFesTaskNoOpExec),       // +0x18: Exec
    reinterpret_cast<uintptr_t>(&InertFesTaskSetActive),      // +0x20: SetActive(int32_t)
    reinterpret_cast<uintptr_t>(&InertFesTaskNoOp),           // +0x28
};

InertFesTask s_InertFesTask = {
    reinterpret_cast<uintptr_t>(kInertFesTaskVtable),
    { 0 },
    0x42u,
    0u,
    0,
    0u,
    0ULL
};

struct LiveNetFesEntry {
    void* mgr         = nullptr;
    void* task        = nullptr;
    bool  isEvergreen = false;
};

constexpr size_t kMaxTrackedNetFesMgrs   = 16;
constexpr size_t kDestroyedNetFesRingCap = 32;

SRWLOCK         s_NetFesLock                                 = SRWLOCK_INIT;
LiveNetFesEntry s_LiveNetFesMgrs[kMaxTrackedNetFesMgrs]      = {};
void*           s_DestroyedNetFesRing[kDestroyedNetFesRingCap] = {};
size_t          s_DestroyedNetFesHead                        = 0;
thread_local void* t_IntentionalDtorFesTask                  = nullptr;

void ReadMgrTaskPtrSeh(void* pMgr, size_t taskOffset, void** ppOutTask) noexcept {
    *ppOutTask = nullptr;
    if (reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) return;
    __try {
        *ppOutTask = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pMgr) + taskOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ppOutTask = nullptr;
    }
}

bool WriteMgrTaskPtrSeh(void* pMgr, size_t taskOffset, void* newTask) noexcept {
    if (reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) return false;
    __try {
        *reinterpret_cast<void**>(static_cast<uint8_t*>(pMgr) + taskOffset) = newTask;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RegisterLiveNetFesMgr(void* pMgr, bool isEvergreen) noexcept {
    if (reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) return;
    const size_t taskOffset = isEvergreen ? kEvergreenFesTaskOffset : kNetFesTaskOffset;
    void* pTask = nullptr;
    ReadMgrTaskPtrSeh(pMgr, taskOffset, &pTask);
    if (!pTask) {
        WriteMgrTaskPtrSeh(pMgr, taskOffset, &s_InertFesTask);
        pTask = &s_InertFesTask;
    }

    AcquireSRWLockExclusive(&s_NetFesLock);
    for (size_t i = 0; i < kDestroyedNetFesRingCap; ++i) {
        if (s_DestroyedNetFesRing[i] == pMgr) {
            s_DestroyedNetFesRing[i] = nullptr;
        }
    }
    size_t freeIdx = kMaxTrackedNetFesMgrs;
    for (size_t i = 0; i < kMaxTrackedNetFesMgrs; ++i) {
        if (s_LiveNetFesMgrs[i].mgr == pMgr) {
            s_LiveNetFesMgrs[i].task        = pTask;
            s_LiveNetFesMgrs[i].isEvergreen = isEvergreen;
            ReleaseSRWLockExclusive(&s_NetFesLock);
            return;
        }
        if (!s_LiveNetFesMgrs[i].mgr && freeIdx == kMaxTrackedNetFesMgrs) {
            freeIdx = i;
        }
    }
    if (freeIdx < kMaxTrackedNetFesMgrs) {
        s_LiveNetFesMgrs[freeIdx].mgr         = pMgr;
        s_LiveNetFesMgrs[freeIdx].task        = pTask;
        s_LiveNetFesMgrs[freeIdx].isEvergreen = isEvergreen;
    }
    ReleaseSRWLockExclusive(&s_NetFesLock);
}

// Verifies both ccTaskCertainExe vtable (+0x1761A08) AND SimpleCommand back-pointer ([pTask+0x28]->[pCmd+0x08] == pMgr)
// so a freed 0x30-byte block reallocated to an unrelated ccTaskCertainExe is never mistaken for pMgr's task.
bool IsRealOwnedNetFesTaskSeh(void* pTask, void* pExpectedMgr) noexcept {
    if (!g_BaseAddress) return false;
    const uintptr_t taskAddr = reinterpret_cast<uintptr_t>(pTask);
    if (taskAddr < kMinValidPtr || taskAddr > kMaxValidUserPtr - 0x30) {
        return false;
    }
    const uintptr_t expectedTaskVtbl = g_BaseAddress + StormRva::kVtbl_TaskCertainExe;
    __try {
        if (*reinterpret_cast<const volatile uintptr_t*>(taskAddr) != expectedTaskVtbl) {
            return false;
        }
        const uintptr_t cmdAddr = *reinterpret_cast<const volatile uintptr_t*>(taskAddr + 0x28);
        if (cmdAddr < kMinValidPtr || cmdAddr > kMaxValidUserPtr - 0x18) {
            return false;
        }
        return (*reinterpret_cast<void* const volatile*>(cmdAddr + 0x08) == pExpectedMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsDestroyedNetFesMgr(void* pMgr) noexcept;

// Fallback for pre-MinHook boot instances: inspects [pTask + 0x28] -> [pCmd + 0x08] to see if pTask
// belongs to a live ccNetEvergreenFesParamManager / ccNetFesParamManager not yet in s_LiveNetFesMgrs.
bool TryHealPreHookNetFesTaskOwnerSeh(void* pTask, void** ppOutHealedMgr, bool* pOutIsEvergreen) noexcept {
    *ppOutHealedMgr  = nullptr;
    *pOutIsEvergreen = false;
    if (!g_BaseAddress) return false;
    const uintptr_t taskAddr = reinterpret_cast<uintptr_t>(pTask);
    if (taskAddr < kMinValidPtr || taskAddr > kMaxValidUserPtr - 0x30) {
        return false;
    }
    const uintptr_t evergreenVtbl = g_BaseAddress + StormRva::kVtbl_EvergreenFesParamMgr;
    const uintptr_t netFesVtbl    = g_BaseAddress + StormRva::kVtbl_NetFesParamMgr;
    __try {
        const uintptr_t cmdAddr = *reinterpret_cast<const volatile uintptr_t*>(taskAddr + 0x28);
        if (cmdAddr < kMinValidPtr || cmdAddr > kMaxValidUserPtr - 0x18) {
            return false;
        }
        void* pOwner = *reinterpret_cast<void* const volatile*>(cmdAddr + 0x08);
        const uintptr_t ownerAddr = reinterpret_cast<uintptr_t>(pOwner);
        if (ownerAddr < kMinValidPtr || ownerAddr > kMaxValidUserPtr - kEvergreenFesSize) {
            return false;
        }
        const uintptr_t ownerVtbl = *reinterpret_cast<const volatile uintptr_t*>(ownerAddr);
        size_t taskOff = 0;
        bool   evergrn = false;
        if (ownerVtbl == evergreenVtbl) {
            taskOff = kEvergreenFesTaskOffset;
            evergrn = true;
        } else if (ownerVtbl == netFesVtbl) {
            taskOff = kNetFesTaskOffset;
            evergrn = false;
        } else {
            return false;
        }
        if (IsDestroyedNetFesMgr(pOwner)) {
            return false;
        }
        if (*reinterpret_cast<void* const volatile*>(ownerAddr + taskOff) != pTask) {
            return false;
        }
        *reinterpret_cast<void**>(ownerAddr + taskOff) = &s_InertFesTask;
        *ppOutHealedMgr  = pOwner;
        *pOutIsEvergreen = evergrn;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Sanitizes `[pMgr + taskOffset]` prior to ~dtor so an already-freed / sentinel task is never double-freed.
void* UnregisterNetFesMgrAndSanitizeTask(void* pMgr, bool isEvergreen) noexcept {
    if (reinterpret_cast<uintptr_t>(pMgr) < kMinValidPtr) return nullptr;
    const size_t taskOffset = isEvergreen ? kEvergreenFesTaskOffset : kNetFesTaskOffset;

    AcquireSRWLockExclusive(&s_NetFesLock);
    for (size_t i = 0; i < kMaxTrackedNetFesMgrs; ++i) {
        if (s_LiveNetFesMgrs[i].mgr == pMgr) {
            s_LiveNetFesMgrs[i] = {};
        }
    }
    s_DestroyedNetFesRing[s_DestroyedNetFesHead] = pMgr;
    s_DestroyedNetFesHead = (s_DestroyedNetFesHead + 1) % kDestroyedNetFesRingCap;
    ReleaseSRWLockExclusive(&s_NetFesLock);

    void* pTask = nullptr;
    ReadMgrTaskPtrSeh(pMgr, taskOffset, &pTask);
    if (!pTask) return nullptr;

    if (pTask == &s_InertFesTask) {
        WriteMgrTaskPtrSeh(pMgr, taskOffset, nullptr);
        return nullptr;
    }

    if (!IsRealOwnedNetFesTaskSeh(pTask, pMgr)) {
        WriteMgrTaskPtrSeh(pMgr, taskOffset, nullptr);
        const uint64_t saveNum = g_Telemetry.netFesCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 / double-free in ~%s! Scrubbed invalid m_pUpdateTask=0x%llX on mgr=0x%llX.",
                       isEvergreen ? "ccNetEvergreenFesParamManager" : "ccNetFesParamManager",
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pTask)),
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pMgr)));
        }
        return nullptr;
    }
    return pTask;
}

bool IsDestroyedNetFesMgr(void* pMgr) noexcept {
    bool destroyed = false;
    AcquireSRWLockShared(&s_NetFesLock);
    for (size_t i = 0; i < kDestroyedNetFesRingCap; ++i) {
        if (s_DestroyedNetFesRing[i] == pMgr) {
            destroyed = true;
            break;
        }
    }
    ReleaseSRWLockShared(&s_NetFesLock);
    return destroyed;
}

// Returns:
//   0 = pMgr is dead/corrupt (abort callback!)
//   1 = pMgr & m_pUpdateTask are valid
//   2 = pMgr is alive, but m_pUpdateTask was NULL/freed/corrupt and was healed to &s_InertFesTask!
int ValidateAndHealNetFesCallbackSeh(
    void*       pFuncImpl,
    const void* pErr,
    bool        isEvergreen,
    void**      ppOutMgr,
    void**      ppOutBadTask) noexcept {
    *ppOutMgr     = nullptr;
    *ppOutBadTask = nullptr;
    const uintptr_t fnAddr  = reinterpret_cast<uintptr_t>(pFuncImpl);
    const uintptr_t errAddr = reinterpret_cast<uintptr_t>(pErr);
    if (!g_BaseAddress || fnAddr < kMinValidPtr || fnAddr > kMaxValidUserPtr ||
        errAddr < kMinValidPtr || errAddr > kMaxValidUserPtr) {
        return 0;
    }

    const uintptr_t expectedMgrVtbl = g_BaseAddress +
        (isEvergreen ? StormRva::kVtbl_EvergreenFesParamMgr : StormRva::kVtbl_NetFesParamMgr);
    const size_t    mgrSize          = isEvergreen ? kEvergreenFesSize : kNetFesSize;
    const size_t    taskOffset       = isEvergreen ? kEvergreenFesTaskOffset : kNetFesTaskOffset;

    __try {
        (void)*reinterpret_cast<const volatile int32_t*>(pErr);
        void* pMgr = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(pFuncImpl) + 0x08);
        *ppOutMgr = pMgr;
        const uintptr_t mgrAddr = reinterpret_cast<uintptr_t>(pMgr);
        if (mgrAddr < kMinValidPtr || mgrAddr > kMaxValidUserPtr - mgrSize) {
            return 0;
        }
        // Probe start and end of manager + verify live RTTI vtable
        const uintptr_t actualMgrVtbl = *reinterpret_cast<const volatile uintptr_t*>(mgrAddr);
        (void)*reinterpret_cast<const volatile uint8_t*>(mgrAddr + mgrSize - 1);
        if (actualMgrVtbl != expectedMgrVtbl) {
            return 0;
        }

        void* pTask = *reinterpret_cast<void* const*>(mgrAddr + taskOffset);
        const bool taskOk = (pTask == &s_InertFesTask) || IsRealOwnedNetFesTaskSeh(pTask, pMgr);
        if (!taskOk) {
            *ppOutBadTask = pTask;
            *reinterpret_cast<void**>(mgrAddr + taskOffset) = &s_InertFesTask;
            return 2;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

using fnNetFesLambdaDoCall_t = void(__fastcall*)(void* pFuncImpl, const void* pErr, const void* pPayload);

struct NetFesLambdaVtableSpec {
    const char* name;
    uintptr_t   vtableRva;
    uintptr_t   expectedDoCallRva;
    bool        isEvergreen;
    fnNetFesLambdaDoCall_t origDoCall;
    bool        applied;
};

NetFesLambdaVtableSpec g_NetFesLambdaSpecs[17] = {
    // ccNetEvergreenFesParamManager (9 std::_Func_impl_no_alloc vtables)
    { "EvergreenFes::Lambda0_StatsErr",              0x019987E0, 0x00B6BAC0, true,  nullptr, false },
    { "EvergreenFes::Lambda1_Achievement (+0xB654D3)",0x01998818, 0x00B6B9E0, true,  nullptr, false },
    { "EvergreenFes::Lambda2_FesInfoVec (+0xB65894)", 0x01998850, 0x00B6B9F0, true,  nullptr, false },
    { "EvergreenFes::Lambda3_UserFesInfoA",          0x01998888, 0x00B6BB70, true,  nullptr, false },
    { "EvergreenFes::Lambda4_UserFesInfoB (+0xB6530B)",0x019988C0, 0x00B6B9D0, true,  nullptr, false },
    { "EvergreenFes::Lambda5_FesResultA",            0x019988F8, 0x00B6B730, true,  nullptr, false },
    { "EvergreenFes::Lambda6_FesResultB",            0x01998930, 0x00B6B5F0, true,  nullptr, false },
    { "EvergreenFes::Lambda7_StatsErrB",             0x01998968, 0x00B6B8A0, true,  nullptr, false },
    { "EvergreenFes::Lambda8_StatsErrC (+0xB6BA3D)", 0x019989A0, 0x00B6BA00, true,  nullptr, false },
    // ccNetFesParamManager (8 std::_Func_impl_no_alloc vtables)
    { "NetFes::Lambda0_FesInfoVec",                  0x019995B8, 0x00B756F0, false, nullptr, false },
    { "NetFes::Lambda1_StatsErrA (+0xB75487)",       0x019995F0, 0x00B75450, false, nullptr, false },
    { "NetFes::Lambda2_UserFesInfoA (+0xB759C7)",    0x01999628, 0x00B75990, false, nullptr, false },
    { "NetFes::Lambda3_UserFesInfoB",                0x01999660, 0x00B75700, false, nullptr, false },
    { "NetFes::Lambda4_StatsErrB (+0xB75540)",       0x01999698, 0x00B75500, false, nullptr, false },
    { "NetFes::Lambda5_Achievement (+0xB6EC85)",     0x019996D0, 0x00B75A40, false, nullptr, false },
    { "NetFes::Lambda6_StatsErrC (+0xB75667)",       0x01999708, 0x00B75630, false, nullptr, false },
    { "NetFes::Lambda7_FesResult",                   0x01999740, 0x00B75860, false, nullptr, false },
};

void DispatchNetFesLambdaSafe(size_t idx, void* pFuncImpl, const void* pErr, const void* pPayload) noexcept {
    const auto& spec = g_NetFesLambdaSpecs[idx];
    if (!spec.origDoCall) return;

    void* pMgr     = nullptr;
    void* pBadTask = nullptr;
    const int st = ValidateAndHealNetFesCallbackSeh(pFuncImpl, pErr, spec.isEvergreen, &pMgr, &pBadTask);
    if (st == 0 || IsDestroyedNetFesMgr(pMgr)) {
        const uint64_t saveNum = g_Telemetry.netFesCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 UAF in %s! Dropped orphan HTTP stats callback on destroyed/invalid manager=0x%llX.",
                       spec.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pMgr)));
        }
        return;
    }

    if (st == 2) {
        const uint64_t saveNum = g_Telemetry.netFesCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Prevented 0xC0000005 in %s! Healed freed/NULL m_pUpdateTask=0x%llX on live manager=0x%llX.",
                       spec.name,
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pBadTask)),
                       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pMgr)));
        }
    }

    __try {
        spec.origDoCall(pFuncImpl, pErr, pPayload);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const uint64_t saveNum = g_Telemetry.netFesCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
        if (saveNum <= 16) {
            LogNetcode("[CrashFix] Caught exception inside %s (mgr=0x%llX) — crash prevented.",
                       spec.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pMgr)));
        }
    }
}

template <size_t Idx>
void __fastcall hk_NetFesLambdaDoCall(void* pFuncImpl, const void* pErr, const void* pPayload) {
    DispatchNetFesLambdaSafe(Idx, pFuncImpl, pErr, pPayload);
}

const fnNetFesLambdaDoCall_t kNetFesLambdaDetours[17] = {
    &hk_NetFesLambdaDoCall<0>,  &hk_NetFesLambdaDoCall<1>,  &hk_NetFesLambdaDoCall<2>,  &hk_NetFesLambdaDoCall<3>,
    &hk_NetFesLambdaDoCall<4>,  &hk_NetFesLambdaDoCall<5>,  &hk_NetFesLambdaDoCall<6>,  &hk_NetFesLambdaDoCall<7>,
    &hk_NetFesLambdaDoCall<8>,  &hk_NetFesLambdaDoCall<9>,  &hk_NetFesLambdaDoCall<10>, &hk_NetFesLambdaDoCall<11>,
    &hk_NetFesLambdaDoCall<12>, &hk_NetFesLambdaDoCall<13>, &hk_NetFesLambdaDoCall<14>, &hk_NetFesLambdaDoCall<15>,
    &hk_NetFesLambdaDoCall<16>,
};

} // namespace

void* __fastcall hk_EvergreenFesCtor(void* pThis) {
    if (!oEvergreenFesCtor) return pThis;
    void* res = oEvergreenFesCtor(pThis);
    if (res) {
        RegisterLiveNetFesMgr(res, true);
    }
    return res;
}

void __fastcall hk_EvergreenFesDtor(void* pThis) {
    if (!oEvergreenFesDtor) return;
    void* validTask = UnregisterNetFesMgrAndSanitizeTask(pThis, true);
    void* prevIntentional = t_IntentionalDtorFesTask;
    t_IntentionalDtorFesTask = validTask;
    oEvergreenFesDtor(pThis);
    t_IntentionalDtorFesTask = prevIntentional;
}

void* __fastcall hk_NetFesCtor(void* pThis) {
    if (!oNetFesCtor) return pThis;
    void* res = oNetFesCtor(pThis);
    if (res) {
        RegisterLiveNetFesMgr(res, false);
    }
    return res;
}

void __fastcall hk_NetFesDtor(void* pThis) {
    if (!oNetFesDtor) return;
    void* validTask = UnregisterNetFesMgrAndSanitizeTask(pThis, false);
    void* prevIntentional = t_IntentionalDtorFesTask;
    t_IntentionalDtorFesTask = validTask;
    oNetFesDtor(pThis);
    t_IntentionalDtorFesTask = prevIntentional;
}

void* __fastcall hk_TaskCertainExeSDtor(void* pTask, uint32_t flags) {
    if (pTask == &s_InertFesTask) {
        return pTask;
    }
    if (!oTaskCertainExeSDtor || reinterpret_cast<uintptr_t>(pTask) < kMinValidPtr) {
        return pTask;
    }

    if (pTask != t_IntentionalDtorFesTask) {
        void*  healedMgr   = nullptr;
        bool   isEvergreen = false;
        AcquireSRWLockExclusive(&s_NetFesLock);
        for (size_t i = 0; i < kMaxTrackedNetFesMgrs; ++i) {
            if (s_LiveNetFesMgrs[i].mgr && s_LiveNetFesMgrs[i].task == pTask) {
                healedMgr                = s_LiveNetFesMgrs[i].mgr;
                isEvergreen              = s_LiveNetFesMgrs[i].isEvergreen;
                s_LiveNetFesMgrs[i].task = &s_InertFesTask;
                const size_t off         = isEvergreen ? kEvergreenFesTaskOffset : kNetFesTaskOffset;
                WriteMgrTaskPtrSeh(healedMgr, off, &s_InertFesTask);
            }
        }
        ReleaseSRWLockExclusive(&s_NetFesLock);

        if (!healedMgr) {
            TryHealPreHookNetFesTaskOwnerSeh(pTask, &healedMgr, &isEvergreen);
        }

        if (healedMgr) {
            const uint64_t saveNum = g_Telemetry.netFesCrashSaves.fetch_add(1, std::memory_order_relaxed) + 1;
            if (saveNum <= 16) {
                LogNetcode("[CrashFix] Healed dangling %s::m_pUpdateTask (task=0x%llX -> InertTask on live mgr=0x%llX) before external deletion (+0x0B6530B crash prevented)!",
                           isEvergreen ? "ccNetEvergreenFesParamManager" : "ccNetFesParamManager",
                           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pTask)),
                           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(healedMgr)));
            }
        }
    }

    return oTaskCertainExeSDtor(pTask, flags);
}

bool InstallNetFesLambdaVtableGuards() noexcept {
    if (!g_BaseAddress) return false;
    int appliedCount = 0;

    for (size_t i = 0; i < std::size(g_NetFesLambdaSpecs); ++i) {
        auto& spec = g_NetFesLambdaSpecs[i];
        if (spec.applied) {
            ++appliedCount;
            continue;
        }
        auto* pSlot2 = reinterpret_cast<uintptr_t*>(g_BaseAddress + spec.vtableRva + 0x10);
        const uintptr_t expectedVa = g_BaseAddress + spec.expectedDoCallRva;
        if (*pSlot2 != expectedVa) {
            LogNetcode("[WARN] Vtable slot[2] mismatch for %s @ RVA 0x%llX (got 0x%llX, expected 0x%llX); skipping.",
                       spec.name,
                       static_cast<unsigned long long>(spec.vtableRva + 0x10),
                       static_cast<unsigned long long>(*pSlot2),
                       static_cast<unsigned long long>(expectedVa));
            continue;
        }

        DWORD oldProt = 0;
        if (VirtualProtect(pSlot2, sizeof(uintptr_t), PAGE_READWRITE, &oldProt)) {
            spec.origDoCall = reinterpret_cast<fnNetFesLambdaDoCall_t>(expectedVa);
            *pSlot2 = reinterpret_cast<uintptr_t>(kNetFesLambdaDetours[i]);
            DWORD dummyProt = 0;
            VirtualProtect(pSlot2, sizeof(uintptr_t), oldProt, &dummyProt);
            spec.applied = true;
            ++appliedCount;
        }
    }

    LogNetcode("[Patch] Applied  NetFesLambdaGuards     (%d/%zu async HTTP vtables guarded: +0x0B6530B + 16 twins)",
               appliedCount, std::size(g_NetFesLambdaSpecs));
    return appliedCount > 0;
}

void RemoveNetFesLambdaVtableGuards() noexcept {
    if (!g_BaseAddress) return;
    for (auto& spec : g_NetFesLambdaSpecs) {
        if (!spec.applied || !spec.origDoCall) continue;
        auto* pSlot2 = reinterpret_cast<uintptr_t*>(g_BaseAddress + spec.vtableRva + 0x10);
        DWORD oldProt = 0;
        if (VirtualProtect(pSlot2, sizeof(uintptr_t), PAGE_READWRITE, &oldProt)) {
            *pSlot2 = reinterpret_cast<uintptr_t>(spec.origDoCall);
            DWORD dummyProt = 0;
            VirtualProtect(pSlot2, sizeof(uintptr_t), oldProt, &dummyProt);
        }
        spec.applied = false;
    }
}

} // namespace StormInternal
