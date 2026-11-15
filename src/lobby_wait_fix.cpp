#include "netcode_internal.h"
#include "MinHook.h"
#include <cstring>

namespace StormInternal {

// ============================================================================
// Module 6: Endless / Custom Match Lobby WaitDialog ("MSG_WaitMoment") Deadlock Fix
//
// Root cause observed in NSUNSC.exe (v1.71):
//   1. ccNetEndlessMatchLobbyState_New::CtrlState_MoveLoser (+0x06C1CF0, state 0x12)
//      opens MSG_WaitMoment ("Merci de patienter") with no Flash auto-close timer.
//   2. Host waits on IsAllSignalWait (+0x00FB130), which requires
//        m_lobbyWaitingState.size() >= (memberCount - 1) - m_lobbyEditingState.size().
//      After 30 x 2.0s retries (60s), vanilla logs "Timeout\n" at +0x06C1ED5, resets
//      retry=0, and KEEPS SPINNING FOREVER resending Signal(0) every 2s until every
//      unresponsive peer disconnects (when memberCount drops so numWait >= numMember).
//   3. Clients have NO timeout check at all (retry counter goes negative forever).
//   4. Client race: Sub_140103BD0 only sets [+0x764]=1 if [+0x768]!=0, and Execute()
//      only calls Sub_140103BD0 on Host. If BattleTable arrives before Signal(2), or
//      Signal(2) arrives right before STEP_BEGIN zeroes [+0x764..+0x768], Client stays
//      frozen with [+0x768]=1, [+0x764]=0 until another BattleTable/disconnect arrives.
// ============================================================================

namespace LobbyRva {
constexpr uintptr_t kCtrlState_MoveLoser    = 0x06C1CF0; // VA 0x1406C1CF0
constexpr uintptr_t kLobby_SignalReceive    = 0x00F9520; // VA 0x1400F9520
constexpr uintptr_t kLobby_SignalSend       = 0x00F97A0; // VA 0x1400F97A0
constexpr uintptr_t kLobby_ClearBtlTblWait  = 0x00F39B0; // VA 0x1400F39B0
constexpr uintptr_t kLobby_QueueMoveLoser   = 0x00F3DD0; // VA 0x1400F3DD0
constexpr uintptr_t kLobby_SyncRecords      = 0x0103BD0; // VA 0x140103BD0
constexpr uintptr_t kLobby_WaitDlgClose1    = 0x0103E70; // VA 0x140103E70
constexpr uintptr_t kSession_IsHost         = 0x10B8F10; // VA 0x1410B8F10
constexpr uintptr_t kSession_IsChangeOwner  = 0x10B8EF0; // VA 0x1410B8EF0
} // namespace LobbyRva

namespace LobbyOff {
constexpr size_t kSessionPtr         = 0x030;
constexpr size_t kBattleTableDirty   = 0x468;
constexpr size_t kTableUpdatedFlag   = 0x764;
constexpr size_t kSignal2ReadyFlag   = 0x768;
constexpr size_t kPendingMoveLoser   = 0x76C;
constexpr size_t kChangeOwnerDlgOpen = 0x7C8;
constexpr size_t kResendTimerSec     = 0x7D8;
constexpr size_t kWaitDlgOpen        = 0x7F0;
constexpr size_t kWaitDlgRefCount    = 0x7F4;
} // namespace LobbyOff

namespace {

constexpr uint64_t kHostBarrierTimeoutMs     = 10000ULL;
constexpr uint64_t kHostTableGraceMs         = 2000ULL;
constexpr uint64_t kClientTableRaceGraceMs   = 1500ULL;
constexpr uint64_t kClientBarrierTimeoutMs   = 14000ULL;
constexpr uint64_t kClientOwnerMigMaxGraceMs = 22000ULL;
constexpr uint64_t kSignal0SpamWindowMs      = 6000ULL;
constexpr uint64_t kBailoutCooldownMs        = 60000ULL;

using fnCtrlStateMoveLoser_t = void(__fastcall*)(void* pLobbyState, int32_t step);
using fnSignalReceive_t      = void(__fastcall*)(void* pLobbyState, void* pArg2, const void* pPacket);
using fnSignalSend_t         = void(__fastcall*)(void* pLobbyState, int32_t signalId, int32_t toAll);
using fnClearBtlTblWait_t    = void(__fastcall*)(void* pLobbyState, int32_t oldStatus, int32_t newStatus);
using fnQueueMoveLoser_t     = void(__fastcall*)(void* pLobbyState);
using fnSyncRecords_t        = void(__fastcall*)(void* pLobbyState);
using fnWaitDlgClose1_t      = void(__fastcall*)(void* pLobbyState);
using fnSessionPred_t        = int32_t(__fastcall*)(void* pSession);

fnCtrlStateMoveLoser_t oCtrlStateMoveLoser = nullptr;
fnSignalReceive_t      oSignalReceive      = nullptr;
fnSignalSend_t         g_fnLobbySignalSend = nullptr;
fnClearBtlTblWait_t    g_fnClearBtlTblWait = nullptr;
fnQueueMoveLoser_t     g_fnQueueMoveLoser  = nullptr;
fnSyncRecords_t        g_fnSyncRecords     = nullptr;
fnWaitDlgClose1_t      g_fnWaitDlgClose1   = nullptr;
fnSessionPred_t        g_fnSessionIsHost   = nullptr;
fnSessionPred_t        g_fnIsChangeOwner   = nullptr;

struct LobbyWaitTracker {
    void*    activeLobbyPtr         = nullptr;
    bool     inMoveLoser            = false;
    uint64_t moveLoserEnterMs       = 0;
    uint64_t signal2SeenMs          = 0;
    bool     hostForcedSignal2      = false;
    bool     hostForcedTable        = false;
    bool     clientSyncedAfterSig2  = false;
    bool     preEntrySignal2Seen    = false;
    uint64_t lastBailoutMs          = 0;
    uint64_t lastSignal0Ms          = 0;
};

SRWLOCK          s_LobbyWaitLock = SRWLOCK_INIT;
LobbyWaitTracker s_LobbyWait     = {};

template <typename Fn>
Fn ResolveVerifiedHelper(const char* name, uintptr_t rva, const uint8_t* expected, size_t len) noexcept {
    if (!g_BaseAddress) return nullptr;
    const auto* ptr = reinterpret_cast<const uint8_t*>(g_BaseAddress + rva);
    if (std::memcmp(ptr, expected, len) != 0) {
        LogNetcode("[WARN] %s prologue mismatch @ RVA 0x%llX; helper skipped.",
                   name, static_cast<unsigned long long>(rva));
        return nullptr;
    }
    return reinterpret_cast<Fn>(g_BaseAddress + rva);
}

bool SafeReadI32(const void* base, size_t offset, int32_t* outVal) noexcept {
    if (!base || !outVal) return false;
    __try {
        *outVal = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(base) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteI32(void* base, size_t offset, int32_t val) noexcept {
    if (!base) return false;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(base) + offset) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteF32(void* base, size_t offset, float val) noexcept {
    if (!base) return false;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(base) + offset) = val;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeQuerySessionPred(void* pLobby, fnSessionPred_t predFn) noexcept {
    if (!pLobby || !predFn) return false;
    __try {
        void* pSession = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pLobby) + LobbyOff::kSessionPtr);
        if (!pSession) return false;
        return predFn(pSession) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SafeRunHostReleaseSequence(void* pLobby) noexcept {
    if (!pLobby) return;
    __try {
        auto* bytes = reinterpret_cast<uint8_t*>(pLobby);
        *reinterpret_cast<int32_t*>(bytes + LobbyOff::kSignal2ReadyFlag) = 1;
        *reinterpret_cast<int32_t*>(bytes + LobbyOff::kBattleTableDirty) = 1;
        *reinterpret_cast<float*>(bytes + LobbyOff::kResendTimerSec)     = 2.0f;
        if (g_fnLobbySignalSend) g_fnLobbySignalSend(pLobby, 2, 1);
        if (g_fnClearBtlTblWait) g_fnClearBtlTblWait(pLobby, 1, 0);
        if (g_fnQueueMoveLoser)  g_fnQueueMoveLoser(pLobby);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SafeFinishTableAndUnlock(void* pLobby) noexcept {
    if (!pLobby) return;
    __try {
        auto* bytes = reinterpret_cast<uint8_t*>(pLobby);
        *reinterpret_cast<int32_t*>(bytes + LobbyOff::kSignal2ReadyFlag) = 1;
        if (g_fnSyncRecords) g_fnSyncRecords(pLobby);
        *reinterpret_cast<int32_t*>(bytes + LobbyOff::kTableUpdatedFlag) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SafeCloseLeakedWaitDialog(void* pLobby) noexcept {
    if (!pLobby || !g_fnWaitDlgClose1) return;
    __try {
        auto* bytes = reinterpret_cast<uint8_t*>(pLobby);
        const int32_t chgOwner = *reinterpret_cast<int32_t*>(bytes + LobbyOff::kChangeOwnerDlgOpen);
        int32_t openFlag = *reinterpret_cast<int32_t*>(bytes + LobbyOff::kWaitDlgOpen);
        if (openFlag != 0 && chgOwner == 0) {
            auto* pRef = reinterpret_cast<int32_t*>(bytes + LobbyOff::kWaitDlgRefCount);
            if (*pRef > 4) {
                *pRef = 1;
            }
            for (int iter = 0; iter < 4; ++iter) {
                openFlag = *reinterpret_cast<int32_t*>(bytes + LobbyOff::kWaitDlgOpen);
                if (openFlag == 0) break;
                g_fnWaitDlgClose1(pLobby);
            }
            LogNetcode("[LobbyFix] Closed leaked MSG_WaitMoment refcount on CtrlState_MoveLoser STEP_END.");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall hk_SignalReceive(void* pLobbyState, void* pArg2, const void* pPacket) {
    if (!oSignalReceive) return;

    int32_t signalId = -1;
    if (pPacket) {
        SafeReadI32(pPacket, 0x0C, &signalId);
    }

    const uint64_t now = GetTickCount64();
    bool suppressReentry = false;

    if (pLobbyState && (signalId == 0 || signalId == 2)) {
        AcquireSRWLockExclusive(&s_LobbyWaitLock);
        if (s_LobbyWait.activeLobbyPtr != pLobbyState) {
            s_LobbyWait = {};
            s_LobbyWait.activeLobbyPtr = pLobbyState;
        }

        if (signalId == 0) {
            if (s_LobbyWait.lastBailoutMs != 0) {
                const bool withinCooldown  = (now - s_LobbyWait.lastBailoutMs) < kBailoutCooldownMs;
                const bool continuousSpam  = (now - s_LobbyWait.lastSignal0Ms) < kSignal0SpamWindowMs;
                if (withinCooldown && continuousSpam) {
                    suppressReentry = true;
                } else if (!continuousSpam || !withinCooldown) {
                    s_LobbyWait.lastBailoutMs = 0;
                }
            }
            if (!suppressReentry) {
                int32_t pending = 0;
                if (SafeReadI32(pLobbyState, LobbyOff::kPendingMoveLoser, &pending) && pending == 0) {
                    s_LobbyWait.preEntrySignal2Seen = false;
                }
            }
            s_LobbyWait.lastSignal0Ms = now;
        } else { // signalId == 2 (ReleaseMoveLoser)
            s_LobbyWait.lastBailoutMs = 0;
            int32_t pending = 0;
            if (!s_LobbyWait.inMoveLoser &&
                SafeReadI32(pLobbyState, LobbyOff::kPendingMoveLoser, &pending) && pending != 0) {
                s_LobbyWait.preEntrySignal2Seen = true;
            }
        }
        ReleaseSRWLockExclusive(&s_LobbyWaitLock);
    }

    if (suppressReentry) {
        return;
    }

    oSignalReceive(pLobbyState, pArg2, pPacket);
}

void HandleMoveLoserUpdate(void* pLobbyState) noexcept {
    const uint64_t now = GetTickCount64();
    int32_t signal2Ready = 0;
    int32_t tableUpdated = 0;
    if (!SafeReadI32(pLobbyState, LobbyOff::kSignal2ReadyFlag, &signal2Ready) ||
        !SafeReadI32(pLobbyState, LobbyOff::kTableUpdatedFlag, &tableUpdated)) {
        return;
    }
    if (signal2Ready != 0 && tableUpdated != 0) {
        return;
    }

    const bool isHost = SafeQuerySessionPred(pLobbyState, g_fnSessionIsHost);
    const bool changingOwner = SafeQuerySessionPred(pLobbyState, g_fnIsChangeOwner);

    uint64_t enterMs = 0;
    uint64_t sig2Ms  = 0;
    bool runHostRelease      = false;
    bool runHostFinishTable  = false;
    bool runClientRaceSync   = false;
    bool runClientBailout    = false;

    AcquireSRWLockExclusive(&s_LobbyWaitLock);
    if (!s_LobbyWait.inMoveLoser || s_LobbyWait.activeLobbyPtr != pLobbyState) {
        s_LobbyWait.activeLobbyPtr        = pLobbyState;
        s_LobbyWait.inMoveLoser           = true;
        s_LobbyWait.moveLoserEnterMs      = now;
        s_LobbyWait.signal2SeenMs         = (signal2Ready != 0) ? now : 0;
        s_LobbyWait.hostForcedSignal2     = false;
        s_LobbyWait.hostForcedTable       = false;
        s_LobbyWait.clientSyncedAfterSig2 = false;
    }
    if (signal2Ready != 0 && s_LobbyWait.signal2SeenMs == 0) {
        s_LobbyWait.signal2SeenMs = now;
    }
    enterMs = s_LobbyWait.moveLoserEnterMs;
    sig2Ms  = s_LobbyWait.signal2SeenMs;

    const uint64_t elapsedMs = now - enterMs;

    if (isHost) {
        if (signal2Ready == 0 && !s_LobbyWait.hostForcedSignal2 && elapsedMs >= kHostBarrierTimeoutMs) {
            s_LobbyWait.hostForcedSignal2 = true;
            s_LobbyWait.signal2SeenMs     = now;
            runHostRelease = true;
        } else if (signal2Ready != 0 && tableUpdated == 0 && !s_LobbyWait.hostForcedTable &&
                   sig2Ms != 0 && (now - sig2Ms) >= kHostTableGraceMs) {
            s_LobbyWait.hostForcedTable = true;
            runHostFinishTable = true;
        }
    } else {
        if (signal2Ready != 0 && tableUpdated == 0 && !s_LobbyWait.clientSyncedAfterSig2 &&
            sig2Ms != 0 && (now - sig2Ms) >= kClientTableRaceGraceMs) {
            s_LobbyWait.clientSyncedAfterSig2 = true;
            runClientRaceSync = true;
        } else if (signal2Ready == 0) {
            const uint64_t limitMs = changingOwner ? kClientOwnerMigMaxGraceMs : kClientBarrierTimeoutMs;
            if (elapsedMs >= limitMs) {
                s_LobbyWait.lastBailoutMs = now;
                s_LobbyWait.lastSignal0Ms = now;
                runClientBailout = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&s_LobbyWaitLock);

    if (runHostRelease) {
        SafeRunHostReleaseSequence(pLobbyState);
        g_Telemetry.lobbyWaitDeadlockSaves.fetch_add(1, std::memory_order_relaxed);
        LogNetcode("[LobbyFix] CtrlState_MoveLoser Host barrier timeout (%llu ms) — broadcasting Signal(2) & unlocking WaitDialog!",
                   static_cast<unsigned long long>(elapsedMs));
    } else if (runHostFinishTable) {
        SafeFinishTableAndUnlock(pLobbyState);
        g_Telemetry.lobbyWaitDeadlockSaves.fetch_add(1, std::memory_order_relaxed);
        LogNetcode("[LobbyFix] Host [Signal2=1, Table=0] stalled (%llu ms) — forcing table sync & WaitDialog exit!",
                   static_cast<unsigned long long>(now - sig2Ms));
    } else if (runClientRaceSync) {
        SafeFinishTableAndUnlock(pLobbyState);
        g_Telemetry.lobbyWaitDeadlockSaves.fetch_add(1, std::memory_order_relaxed);
        LogNetcode("[LobbyFix] Client resolved out-of-order Signal(2)/BattleTable race (+0x768=1, +0x764=0 -> 1 after %llu ms).",
                   static_cast<unsigned long long>(now - sig2Ms));
    } else if (runClientBailout) {
        SafeFinishTableAndUnlock(pLobbyState);
        g_Telemetry.lobbyWaitDeadlockSaves.fetch_add(1, std::memory_order_relaxed);
        LogNetcode("[LobbyFix] CtrlState_MoveLoser Client timeout (%llu ms) — safely escaping stalled WaitDialog ('Merci de patienter')!",
                   static_cast<unsigned long long>(elapsedMs));
    }
}

void __fastcall hk_CtrlStateMoveLoser(void* pLobbyState, int32_t step) {
    if (!oCtrlStateMoveLoser) return;

    const uint64_t now = GetTickCount64();

    if (step == 0 && pLobbyState) { // STEP_BEGIN
        bool restorePreSig2 = false;
        AcquireSRWLockExclusive(&s_LobbyWaitLock);
        const bool sameLobby = (s_LobbyWait.activeLobbyPtr == pLobbyState);
        restorePreSig2 = sameLobby && s_LobbyWait.preEntrySignal2Seen;
        s_LobbyWait.activeLobbyPtr        = pLobbyState;
        s_LobbyWait.inMoveLoser           = true;
        s_LobbyWait.moveLoserEnterMs      = now;
        s_LobbyWait.signal2SeenMs         = restorePreSig2 ? now : 0;
        s_LobbyWait.hostForcedSignal2     = false;
        s_LobbyWait.hostForcedTable       = false;
        s_LobbyWait.clientSyncedAfterSig2 = false;
        s_LobbyWait.preEntrySignal2Seen   = false;
        ReleaseSRWLockExclusive(&s_LobbyWaitLock);

        oCtrlStateMoveLoser(pLobbyState, step);

        if (restorePreSig2) {
            SafeWriteI32(pLobbyState, LobbyOff::kSignal2ReadyFlag, 1);
            LogNetcode("[LobbyFix] Preserved pre-entry Signal(2) across CtrlState_MoveLoser STEP_BEGIN.");
        }
        return;
    }

    if (step == 1 && pLobbyState) { // STEP_UPDATE
        HandleMoveLoserUpdate(pLobbyState);
        oCtrlStateMoveLoser(pLobbyState, step);
        return;
    }

    if (step == 2 && pLobbyState) { // STEP_END
        oCtrlStateMoveLoser(pLobbyState, step);
        SafeWriteI32(pLobbyState, LobbyOff::kPendingMoveLoser, 0);
        SafeCloseLeakedWaitDialog(pLobbyState);

        AcquireSRWLockExclusive(&s_LobbyWaitLock);
        if (s_LobbyWait.activeLobbyPtr == pLobbyState) {
            s_LobbyWait.inMoveLoser           = false;
            s_LobbyWait.signal2SeenMs         = 0;
            s_LobbyWait.hostForcedSignal2     = false;
            s_LobbyWait.hostForcedTable       = false;
            s_LobbyWait.clientSyncedAfterSig2 = false;
            s_LobbyWait.preEntrySignal2Seen   = false;
        }
        ReleaseSRWLockExclusive(&s_LobbyWaitLock);
        return;
    }

    oCtrlStateMoveLoser(pLobbyState, step);
}

} // namespace

void ResetLobbyWaitDeadlockState() noexcept {
    AcquireSRWLockExclusive(&s_LobbyWaitLock);
    s_LobbyWait.lastBailoutMs       = 0;
    s_LobbyWait.lastSignal0Ms       = 0;
    s_LobbyWait.preEntrySignal2Seen = false;
    ReleaseSRWLockExclusive(&s_LobbyWaitLock);
}

void InstallLobbyWaitDeadlockHooks(int* pInstalledCount, int* pExpectedCount) noexcept {
    if (!g_BaseAddress || !g_Config.fix_lobby_wait_deadlock || !g_Config.fix_online_persona_crash) {
        return;
    }

    static constexpr uint8_t kPrologue_IsHost[]        = { 0x48, 0x8B, 0x49, 0x20, 0x33, 0xD2, 0x48, 0x8D };
    static constexpr uint8_t kPrologue_IsChangeOwner[] = { 0x83, 0x79, 0x28, 0x02, 0x74, 0x03, 0x33, 0xC0 };
    static constexpr uint8_t kPrologue_SignalSend[]    = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83 };
    static constexpr uint8_t kPrologue_ClearBtlTbl[]   = { 0x48, 0x8B, 0x0D, 0x09, 0xF0, 0x0C, 0x02, 0x8B };
    static constexpr uint8_t kPrologue_QueueMoveTbl[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74 };
    static constexpr uint8_t kPrologue_SyncRecords[]   = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C };
    static constexpr uint8_t kPrologue_WaitDlgClose1[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B };

    g_fnSessionIsHost   = ResolveVerifiedHelper<fnSessionPred_t>("Session::IsHost", LobbyRva::kSession_IsHost, kPrologue_IsHost, sizeof(kPrologue_IsHost));
    g_fnIsChangeOwner   = ResolveVerifiedHelper<fnSessionPred_t>("Session::IsExecChangeOwner", LobbyRva::kSession_IsChangeOwner, kPrologue_IsChangeOwner, sizeof(kPrologue_IsChangeOwner));
    g_fnLobbySignalSend = ResolveVerifiedHelper<fnSignalSend_t>("EndlessLobby::SignalSend", LobbyRva::kLobby_SignalSend, kPrologue_SignalSend, sizeof(kPrologue_SignalSend));
    g_fnClearBtlTblWait = ResolveVerifiedHelper<fnClearBtlTblWait_t>("EndlessLobby::ClearBtlTblWait", LobbyRva::kLobby_ClearBtlTblWait, kPrologue_ClearBtlTbl, sizeof(kPrologue_ClearBtlTbl));
    g_fnQueueMoveLoser  = ResolveVerifiedHelper<fnQueueMoveLoser_t>("EndlessLobby::QueueMoveLoserTbl", LobbyRva::kLobby_QueueMoveLoser, kPrologue_QueueMoveTbl, sizeof(kPrologue_QueueMoveTbl));
    g_fnSyncRecords     = ResolveVerifiedHelper<fnSyncRecords_t>("EndlessLobby::SyncRecordsAndUnlock", LobbyRva::kLobby_SyncRecords, kPrologue_SyncRecords, sizeof(kPrologue_SyncRecords));
    g_fnWaitDlgClose1   = ResolveVerifiedHelper<fnWaitDlgClose1_t>("EndlessLobby::WaitDlgClose1", LobbyRva::kLobby_WaitDlgClose1, kPrologue_WaitDlgClose1, sizeof(kPrologue_WaitDlgClose1));

    static constexpr uint8_t kPrologue_MoveLoser[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    static constexpr uint8_t kPrologue_SignalReceive[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30 };

    struct LobbyHookSpec {
        const char*    name;
        uintptr_t      rva;
        const uint8_t* prologue;
        size_t         len;
        LPVOID         detour;
        LPVOID*        original;
    };

    const LobbyHookSpec specs[] = {
        { "EndlessLobby::CtrlState_MoveLoser (+0x06C1CF0)", LobbyRva::kCtrlState_MoveLoser, kPrologue_MoveLoser,     sizeof(kPrologue_MoveLoser),     reinterpret_cast<LPVOID>(&hk_CtrlStateMoveLoser), reinterpret_cast<LPVOID*>(&oCtrlStateMoveLoser) },
        { "EndlessLobby::Signal_Receive (+0x00F9520)",      LobbyRva::kLobby_SignalReceive, kPrologue_SignalReceive, sizeof(kPrologue_SignalReceive), reinterpret_cast<LPVOID>(&hk_SignalReceive),      reinterpret_cast<LPVOID*>(&oSignalReceive) },
    };

    for (const auto& h : specs) {
        if (pExpectedCount) (*pExpectedCount)++;
        const uintptr_t targetVa = g_BaseAddress + h.rva;
        if (std::memcmp(reinterpret_cast<const void*>(targetVa), h.prologue, h.len) != 0) {
            LogNetcode("[ERROR] Prologue mismatch for %s @ VA 0x%llX! Skipping.",
                       h.name, static_cast<unsigned long long>(targetVa));
            continue;
        }
        auto* pTarget = reinterpret_cast<LPVOID>(targetVa);
        if (MH_CreateHook(pTarget, h.detour, h.original) == MH_OK && MH_EnableHook(pTarget) == MH_OK) {
            if (pInstalledCount) (*pInstalledCount)++;
            LogNetcode("[Hook] Installed %-43s @ VA 0x%llX (RVA 0x%07llX)",
                       h.name,
                       static_cast<unsigned long long>(targetVa),
                       static_cast<unsigned long long>(h.rva));
        } else {
            LogNetcode("[ERROR] Failed to hook %s @ VA 0x%llX",
                       h.name, static_cast<unsigned long long>(targetVa));
        }
    }
}

} // namespace StormInternal
