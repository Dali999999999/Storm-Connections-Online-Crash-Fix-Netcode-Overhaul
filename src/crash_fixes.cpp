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

// ============================================================================
// Session Abrupt-Disconnect Null-Deref & Custom/Private Lobby Duplicate-Player
// Helpers & State
// ============================================================================

constexpr uintptr_t kMinValidPtr = 0x10000ULL;
alignas(64) uint8_t   s_SafeDummySessionSlot[0x48] = {};
std::atomic<uint64_t> s_LastKnownLocalSteamId{0};
std::atomic<uint64_t> s_LastAssignedMemberSteamId{0};
std::atomic<int32_t>  s_LastAssignedMemberUId{0};
std::atomic<bool>     s_LastAssignedMemberWasDuplicate{false};

// Exact match to NSUNSC.exe!CSteamID::IsValid (0x14113C9E0)
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

// Deduplicates entries in Custom Lobby BattleTable[0..7] (stride 0x258, +0x00 = int32_t uId, +0x04 = int32_t status)
void DeduplicateCustomLobbyBattleTable(uint8_t* pBattleTable, int32_t maxEntries, const char* contextTag) noexcept {
    if (reinterpret_cast<uintptr_t>(pBattleTable) < kMinValidPtr) {
        return;
    }
    const int32_t limit = std::clamp(maxEntries, 1, 8);
    for (int32_t i = 0; i < limit; ++i) {
        uint8_t* entryI = pBattleTable + static_cast<size_t>(i) * 0x258;
        const int32_t uidI    = *reinterpret_cast<const int32_t*>(entryI + 0x00);
        const int32_t statusI = *reinterpret_cast<const int32_t*>(entryI + 0x04);
        if (uidI < 0 || statusI < 0) {
            continue;
        }
        for (int32_t j = i + 1; j < limit; ++j) {
            uint8_t* entryJ = pBattleTable + static_cast<size_t>(j) * 0x258;
            const int32_t uidJ    = *reinterpret_cast<const int32_t*>(entryJ + 0x00);
            const int32_t statusJ = *reinterpret_cast<const int32_t*>(entryJ + 0x04);
            if (uidJ == uidI && statusJ >= 0) {
                // If entryJ has a newer active status (e.g. 0=fighter or 5=waiting vs 6=initial join), keep it in entryI
                if (statusI == 6 && statusJ != 6) {
                    *reinterpret_cast<int32_t*>(entryI + 0x04) = statusJ;
                }
                *reinterpret_cast<int32_t*>(entryJ + 0x00) = -1;
                *reinterpret_cast<int32_t*>(entryJ + 0x04) = -1;
                const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                if (saveNum <= 16) {
                    LogNetcode("[LobbyFix] Cleared duplicate BattleTable row (%d & %d) for uId=%d in %s!",
                               i, j, uidI, contextTag);
                }
            }
        }
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
    return oKeyboardGetPressedChar ? oKeyboardGetPressedChar(pKeyMap) : 0;
}

uint32_t __fastcall hk_KeyboardGetPressedKey(const void* pKeyMap) {
    if (!IsKeyboardKeyMapSafeToQuery(pKeyMap, "KeyboardGetPressedKey (+0x120FFB5)")) {
        return 0;
    }
    return oKeyboardGetPressedKey ? oKeyboardGetPressedKey(pKeyMap) : 0;
}

// Guards ccNetSessionCommunicator::SendPacket (0x141169290, faults @ +0x11692A3 in 14496.dmp)
// when [pSession + 0x518] (m_pCommunicator) or [pComm + 0x08] becomes NULL during abrupt disconnect.
int32_t __fastcall hk_SessionCommSendPacket(void* pComm, void* pPacket, uint32_t packetSize, int32_t targetUId) {
    const uintptr_t commAddr = reinterpret_cast<uintptr_t>(pComm);
    const uintptr_t pktAddr  = reinterpret_cast<uintptr_t>(pPacket);
    if (commAddr < kMinValidPtr || pktAddr < kMinValidPtr ||
        *reinterpret_cast<const uintptr_t*>(commAddr + 0x08) < kMinValidPtr) {
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
    if (commAddr < kMinValidPtr || pktAddr < kMinValidPtr ||
        *reinterpret_cast<const uintptr_t*>(commAddr + 0x08) < kMinValidPtr) {
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
        if (s_LastAssignedMemberWasDuplicate.exchange(false, std::memory_order_relaxed)) {
            return; // Do not fire a duplicate OnMemberJoined event for an already-joined member!
        }

        auto* pSessBytes = reinterpret_cast<uint8_t*>(pSession);
        uint8_t* const slotsBase = pSessBytes + 0x2C0;
        const uint64_t expectedSid = s_LastAssignedMemberSteamId.load(std::memory_order_relaxed);
        const int32_t  expectedUId = s_LastAssignedMemberUId.load(std::memory_order_relaxed);

        if (IsValidSteamId(expectedSid)) {
            const bool slotMatches =
                reinterpret_cast<uintptr_t>(pSlot) >= kMinValidPtr &&
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

// Fixes UpdateBattleTableFromChangeQueue (0x1400F4310):
// At +0x00F4439..+0x00F4479, vanilla handles `status == 6` (initial join) by searching BattleTable[0..7]
// ONLY for `uId <= -1` (first empty slot) without checking whether `uId` is already present in BattleTable!
// Whenever a player's status==6 entry is queued more than once, vanilla inserts a second copy of the same uId.
void __fastcall hk_CustomLobbyUpdateBattleTable(void* pLobbyState) {
    if (!oCustomLobbyUpdateBattleTbl || reinterpret_cast<uintptr_t>(pLobbyState) < kMinValidPtr || !g_BaseAddress) {
        return;
    }

    uint8_t* pBattleTable = nullptr;
    int32_t  maxMembers   = 8;
    const uintptr_t pNetMgr = *reinterpret_cast<const uintptr_t*>(g_BaseAddress + StormRva::kGlobalNetManagerPtr);
    if (pNetMgr >= kMinValidPtr) {
        const uintptr_t pLocalInfo = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0x100);
        const uintptr_t pSessData  = *reinterpret_cast<const uintptr_t*>(pNetMgr + 0xF8);
        if (pLocalInfo >= kMinValidPtr) {
            pBattleTable = reinterpret_cast<uint8_t*>(pLocalInfo + 0xC8);
        }
        if (pSessData >= kMinValidPtr) {
            maxMembers = std::clamp(*reinterpret_cast<const int32_t*>(pSessData + 0x28), 1, 8);
        }
    }

    if (pBattleTable) {
        // 1. Deduplicate any existing duplicate rows in BattleTable[0..maxMembers-1]
        DeduplicateCustomLobbyBattleTable(pBattleTable, maxMembers, "Pre-UpdateBattleTable");

        // 2. Sanitize m_changeQueue ([pLobbyState + 0x448] .. [pLobbyState + 0x450], stride 0x258)
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
                if (qUId < 0 || qStatus != 6) {
                    continue;
                }

                // Check if qUId is already present in BattleTable[0..maxMembers-1]
                int32_t existingBtIdx = -1;
                for (int32_t bi = 0; bi < maxMembers; ++bi) {
                    const uint8_t* btEntry = pBattleTable + static_cast<size_t>(bi) * 0x258;
                    if (*reinterpret_cast<const int32_t*>(btEntry + 0x00) == qUId &&
                        *reinterpret_cast<const int32_t*>(btEntry + 0x04) >= 0) {
                        existingBtIdx = bi;
                        break;
                    }
                }

                if (existingBtIdx >= 0) {
                    // Already in BattleTable! Convert status 6 -> existing status (or 5=waiting) if needed,
                    // or drop redundant initial-join so +0x00F4450 never allocates a second slot for qUId!
                    *reinterpret_cast<int32_t*>(qEntry + 0x00) = -1;
                    *reinterpret_cast<int32_t*>(qEntry + 0x04) = -1;
                    const uint64_t saveNum = g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (saveNum <= 16) {
                        LogNetcode("[LobbyFix] Suppressed duplicate status=6 changeQueue insertion for uId=%d (already in BattleTable[%d])",
                                   qUId, existingBtIdx);
                    }
                    continue;
                }

                // Also suppress duplicate status==6 entries for the same qUId within the same changeQueue batch
                for (size_t qj = qi + 1; qj < qCount; ++qj) {
                    uint8_t* qNext = qBegin + qj * 0x258;
                    if (*reinterpret_cast<const int32_t*>(qNext + 0x00) == qUId &&
                        *reinterpret_cast<const int32_t*>(qNext + 0x04) == 6) {
                        *reinterpret_cast<int32_t*>(qNext + 0x00) = -1;
                        *reinterpret_cast<int32_t*>(qNext + 0x04) = -1;
                        g_Telemetry.duplicateLobbySlotSaves.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    oCustomLobbyUpdateBattleTbl(pLobbyState);

    if (pBattleTable) {
        DeduplicateCustomLobbyBattleTable(pBattleTable, maxMembers, "Post-UpdateBattleTable");
    }
}

// Guards ccStateMachineCustomMatchLobbyUi::UpdateUserList (0x1400FF810) so duplicate uId rows
// can never be rendered in the Custom/Private Lobby UI list.
// Note: 0x00FF810 takes 6 arguments (rcx=pLobbyUi, rdx=pSessionData, r8=pBattleTable, r9=pArg4,
// [rsp+0x28]=selectedIdx, [rsp+0x30]=pArg6); all 6 must be forwarded intact!
void __fastcall hk_CustomLobbyUiUpdateUserList(
    void* pLobbyUi, void* pSessionData, void* pBattleTable, void* pArg4, uint64_t selectedIdx, void* pArg6) {
    if (reinterpret_cast<uintptr_t>(pBattleTable) >= kMinValidPtr &&
        reinterpret_cast<uintptr_t>(pSessionData) >= kMinValidPtr) {
        int32_t maxMembers = 8;
        __try {
            maxMembers = std::clamp(
                *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(pSessionData) + 0x28),
                1, 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            maxMembers = 8;
        }
        if (maxMembers > 1) {
            DeduplicateCustomLobbyBattleTable(reinterpret_cast<uint8_t*>(pBattleTable),
                                              maxMembers,
                                              "CustomLobbyUi::UpdateUserList(0x00FF810)");
        }
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

void ApplyCrashAndNetcodeBytePatches(bool applyCrashPatches, bool applyNetcodePatches) noexcept {
    if (applyCrashPatches || applyNetcodePatches) {
        ApplyVerifiedBytePatch(g_EnginePatches[0]); // PushResendRequest null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[1]); // ReceivePackets NACK null-deref fix
        ApplyVerifiedBytePatch(g_EnginePatches[5]); // nuccFree at-exit heap corruption fix (+0x12732D8)
        ApplyVerifiedBytePatch(g_EnginePatches[6]); // ccStateChatWindow exit1 teardown order fix (+0x016A795)
        ApplyVerifiedBytePatch(g_EnginePatches[7]); // ccStateChatWindow exit2 teardown order fix (+0x016A8CB)
        ApplyVerifiedBytePatch(g_EnginePatches[8]); // ccStateChatWindow exit3 teardown order fix (+0x016AAE1)
        InstallLobbySlotNullFixBridge();            // LobbySlotNullFix bridge (+0x113D7E6) & State1SortFix (+0x113D736)
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

} // namespace StormInternal
