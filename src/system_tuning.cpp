#include "netcode_internal.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wlanapi.h>
#include <algorithm>
#include <intrin.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace StormInternal {

// ============================================================================
// 0b. DirectX 11 Zero-Queue Render & Win11 Gaming Scheduler Boost
//     1) Forces IDXGIDevice1::SetMaximumFrameLatency(1) + SetGPUThreadPriority(5) (-33.3 ms local lag).
//     2) Disables Windows 11 EcoQoS & Timer-Coalescing Power Throttling so NSUNSC.exe's Sleep(0)
//        60.00 Hz frame-limiter at 0x1414173D2 never drifts lockstep phase against remote peers.
//     3) Locks 1.0 ms kernel timer resolution (winmm.dll) & registers main thread with MMCSS "Games".
// ============================================================================

namespace {

struct StormPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

using fnSetProcessInformation_t = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using fnNtSetTimerResolution_t  = LONG(NTAPI*)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
using fnTimeBeginPeriod_t       = UINT(WINAPI*)(UINT);
using fnTimeEndPeriod_t         = UINT(WINAPI*)(UINT);
using fnAvSetMmThreadCharsW_t   = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using fnAvSetMmThreadPriority_t = BOOL(WINAPI*)(HANDLE, int);
using fnAvRevertMmThreadChars_t = BOOL(WINAPI*)(HANDLE);

HMODULE                   s_hWinMmModule       = nullptr;
fnTimeEndPeriod_t         s_fnTimeEndPeriod    = nullptr;
fnNtSetTimerResolution_t  s_fnNtSetTimerRes    = nullptr;
ULONG                     s_ActualTimerRes100ns = 0;
bool                      s_NtSubMsTimerActive = false;
bool                      s_TimerPeriodActive  = false;
HMODULE                   s_hAvrtModule        = nullptr;
HANDLE                    s_hMmcssHandle       = nullptr;
fnAvRevertMmThreadChars_t s_fnAvRevertMm       = nullptr;
std::atomic<bool>         s_MmcssThreadApplied{false};
ID3D11Device*             s_LastConfiguredD3D11Dev = nullptr;

// Precision drift-free 60.000 / 30.000 Hz frame pacer state (render thread only)
HANDLE         s_hFramePacerTimer      = nullptr;
bool           s_FramePacerInit        = false;
bool           s_FramePacerHighRes     = false;
int64_t        s_NextFrameQpc          = 0;
int64_t        s_FrameRem              = 0;
uint32_t       s_LastFrameDiv          = 0xFFFFFFFFu;
IDXGISwapChain* s_LastCheckedSwapChain = nullptr;
uint64_t       s_LastDisplayQueryMs    = 0;
uint32_t       s_CachedDisplayHz       = 0;
bool           s_CachedBypassVsync     = false;
uint32_t       s_LastLoggedBypassHz    = 0;

bool ShouldBypassWindowedNon60HzVsync(IDXGISwapChain* pSwapChain) noexcept {
    if (!pSwapChain || !g_Config.bypass_non60hz_vsync_judder) {
        return false;
    }
    const uint64_t nowMs = GetTickCount64();
    if (pSwapChain == s_LastCheckedSwapChain && s_LastDisplayQueryMs != 0 && (nowMs - s_LastDisplayQueryMs) < 2000ULL) {
        return s_CachedBypassVsync;
    }
    s_LastCheckedSwapChain = pSwapChain;
    s_LastDisplayQueryMs   = nowMs;

    DXGI_SWAP_CHAIN_DESC scDesc{};
    if (FAILED(pSwapChain->GetDesc(&scDesc)) || scDesc.Windowed == FALSE) {
        // Never force SyncInterval=0 in exclusive fullscreen where DWM does not prevent tearing
        s_CachedBypassVsync = false;
        return false;
    }

    uint32_t hz = 0;
    IDXGIOutput* pOutput = nullptr;
    if (SUCCEEDED(pSwapChain->GetContainingOutput(&pOutput)) && pOutput) {
        DXGI_OUTPUT_DESC outDesc{};
        if (SUCCEEDED(pOutput->GetDesc(&outDesc)) && outDesc.Monitor != nullptr) {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(outDesc.Monitor, &mi)) {
                DEVMODEW dm{};
                dm.dmSize = sizeof(dm);
                if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1u) {
                    hz = static_cast<uint32_t>(dm.dmDisplayFrequency);
                }
            }
        }
        pOutput->Release();
    }

    s_CachedDisplayHz   = hz;
    s_CachedBypassVsync = (hz > 62u) && ((hz % 60u) != 0u);
    if (s_CachedBypassVsync && s_LastLoggedBypassHz != hz) {
        s_LastLoggedBypassHz = hz;
        LogNetcode("[FramePacer] Windowed %u Hz display detected (non-60Hz multiple) -> bypassing Present(1,0) VBlank beat; exact 60.000Hz pacer active.",
                   hz);
    }
    return s_CachedBypassVsync;
}

void InitFramePacerTimerOnce() noexcept {
    if (s_FramePacerInit) {
        return;
    }
    s_FramePacerInit = true;
    // Try Windows 10 1803+ / Windows 11 high-resolution waitable timer first (~0.3-0.5ms granularity)
    s_hFramePacerTimer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (s_hFramePacerTimer) {
        s_FramePacerHighRes = true;
    } else {
        s_hFramePacerTimer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        s_FramePacerHighRes = false;
    }
    LogNetcode("[FramePacer] Precision drift-free 60.000Hz pacer active (HighResWaitableTimer=%s, Sleep(0) starvation eliminated).",
               s_FramePacerHighRes ? "YES" : "FALLBACK");
}

void PaceFrameExact(uint32_t frameDiv, bool vsyncEnabled) noexcept {
    if (g_QpcFreq.QuadPart <= 0) {
        return;
    }
    InitFramePacerTimerOnce();

    const uint32_t clampedDiv = (frameDiv <= 3u) ? frameDiv : 0u;
    const int64_t  freq       = g_QpcFreq.QuadPart;
    const int64_t  num        = freq * static_cast<int64_t>(clampedDiv + 1u);
    const int64_t  baseTicks  = num / 60LL;
    const int64_t  remTicks   = num % 60LL;

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);

    if (s_NextFrameQpc == 0 || s_LastFrameDiv != clampedDiv) {
        s_LastFrameDiv = clampedDiv;
        s_NextFrameQpc = now.QuadPart;
        s_FrameRem     = 0;
        return;
    }

    // Advance target by exact rational period (1/60s * (frameDiv + 1)) with zero cumulative drift
    int64_t step = baseTicks;
    s_FrameRem += remTicks;
    if (s_FrameRem >= 60LL) {
        ++step;
        s_FrameRem -= 60LL;
    }

    // Gentle clock-slip tracking ONLY during active 60 FPS online battles (bounded 0..350 us):
    // When remote peer drifts at ~58.8-59.5Hz, smoothly match its cadence to prevent periodic 1-tick stalls.
    if (clampedDiv == 0u) {
        const int32_t extraUs = g_FramePacerExtraUs.load(std::memory_order_relaxed);
        if (extraUs > 0) {
            const uint64_t lastBattleMs = g_LastActiveBattleTickMs.load(std::memory_order_relaxed);
            const uint64_t nowMs        = GetTickCount64();
            if (nowMs >= lastBattleMs && (nowMs - lastBattleMs) <= 250ULL) {
                step += (freq * static_cast<int64_t>(std::clamp(extraUs, 0, 350))) / 1000000LL;
            } else {
                g_FramePacerExtraUs.store(0, std::memory_order_relaxed);
            }
        }
    }
    s_NextFrameQpc += step;

    // If frame already exceeded target (e.g. 60Hz VSync VBlank, alt-tab, loading screen), do not wait
    if (now.QuadPart >= s_NextFrameQpc) {
        if (vsyncEnabled || (now.QuadPart - s_NextFrameQpc) > (baseTicks * 2LL)) {
            s_NextFrameQpc = now.QuadPart;
            s_FrameRem     = 0;
        }
        return;
    }

    // Phase 1: True OS sleep while > 1.5 ms remains so lower-priority Steam P2P & NIC threads never starve
    constexpr int64_t kSpinTailUs = 1200LL;
    while (true) {
        QueryPerformanceCounter(&now);
        const int64_t remQpc = s_NextFrameQpc - now.QuadPart;
        if (remQpc <= 0) {
            return;
        }
        const int64_t remUs = (remQpc * 1000000LL) / freq;
        if (remUs <= 1500LL) {
            break;
        }
        if (s_hFramePacerTimer) {
            LARGE_INTEGER dueTime = {};
            dueTime.QuadPart = -((remUs - kSpinTailUs) * 10LL); // Negative = relative 100ns units
            if (dueTime.QuadPart < 0 && SetWaitableTimer(s_hFramePacerTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(s_hFramePacerTimer, 20);
                continue;
            }
        }
        Sleep(1);
    }

    // Phase 2: Sub-millisecond _mm_pause() polish for +-10 us frame-pacing precision
    while (true) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= s_NextFrameQpc) {
            break;
        }
        _mm_pause();
    }
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

HMODULE                s_hWlanApiModule    = nullptr;
HANDLE                 s_hWlanClientHandle = nullptr;
fnWlanCloseHandle_t    s_fnWlanCloseHandle = nullptr;
fnWlanEnumInterfaces_t s_fnWlanEnumIfaces  = nullptr;
fnWlanSetInterface_t   s_fnWlanSetIface    = nullptr;
fnWlanQueryInterface_t s_fnWlanQueryIface  = nullptr;
fnWlanFreeMemory_t     s_fnWlanFreeMemory  = nullptr;

} // namespace

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
            auto fnBegin      = reinterpret_cast<fnTimeBeginPeriod_t>(GetProcAddress(s_hWinMmModule, "timeBeginPeriod"));
            s_fnTimeEndPeriod = reinterpret_cast<fnTimeEndPeriod_t>(GetProcAddress(s_hWinMmModule, "timeEndPeriod"));
            if (fnBegin && s_fnTimeEndPeriod && fnBegin(1) == 0) {
                s_TimerPeriodActive = true;
            }
        }
    }

    if (!s_NtSubMsTimerActive) {
        if (HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll")) {
            s_fnNtSetTimerRes = reinterpret_cast<fnNtSetTimerResolution_t>(
                GetProcAddress(hNtdll, "NtSetTimerResolution"));
            if (s_fnNtSetTimerRes) {
                ULONG curRes = 0;
                if (s_fnNtSetTimerRes(5000u, TRUE, &curRes) >= 0) { // 5000 * 100ns = 0.500 ms
                    s_ActualTimerRes100ns = curRes;
                    s_NtSubMsTimerActive  = true;
                }
            }
        }
    }

    g_Telemetry.schedulerBoostActive.store(ecoQosDisabled || prioElevated || s_TimerPeriodActive || s_NtSubMsTimerActive);
    LogNetcode("[Scheduler] Gaming Boost active: EcoQoS/TimerCoalesce=%s | Priority=%s | KernelTimer=%s (%0.3f ms)",
               ecoQosDisabled ? "DISABLED" : "N/A",
               prioElevated ? "ABOVE_NORMAL" : "KEPT",
               s_NtSubMsTimerActive ? "0.5ms_NT_LOCKED" : (s_TimerPeriodActive ? "1.0ms_LOCKED" : "DEFAULT"),
               s_NtSubMsTimerActive ? (static_cast<double>(s_ActualTimerRes100ns) / 10000.0) : (s_TimerPeriodActive ? 1.0 : 15.625));
}

void ShutdownProcessGamingBoost(bool processTerminating) noexcept {
    if (s_NtSubMsTimerActive && s_fnNtSetTimerRes) {
        ULONG dummyRes = 0;
        s_fnNtSetTimerRes(5000u, FALSE, &dummyRes);
        s_NtSubMsTimerActive = false;
    }
    if (s_TimerPeriodActive && s_fnTimeEndPeriod) {
        s_fnTimeEndPeriod(1);
        s_TimerPeriodActive = false;
    }
    if (!processTerminating) {
        if (s_hFramePacerTimer) {
            CloseHandle(s_hFramePacerTimer);
            s_hFramePacerTimer = nullptr;
        }
        s_FramePacerInit       = false;
        s_NextFrameQpc         = 0;
        s_FrameRem             = 0;
        s_LastCheckedSwapChain = nullptr;
        s_LastDisplayQueryMs   = 0;
        s_CachedBypassVsync    = false;
        if (s_hMmcssHandle && s_fnAvRevertMm) {
            s_fnAvRevertMm(s_hMmcssHandle);
            s_hMmcssHandle = nullptr;
        }
        if (s_hAvrtModule) {
            FreeLibrary(s_hAvrtModule);
            s_hAvrtModule = nullptr;
        }
        if (s_hWinMmModule) {
            FreeLibrary(s_hWinMmModule);
            s_hWinMmModule = nullptr;
        }
    }
    g_Telemetry.schedulerBoostActive.store(false);
}

// ============================================================================
// 0b-2. Lock-Free Invariant-TSC nuccTimer Replacements (+0x13D7490 / +0x13D75B0)
//       Vanilla NSUNSC.exe inherits a 2006 Ogre3D Windows XP workaround that calls
//       SetThreadAffinityMask(GetCurrentThread(), Core0) TWICE on every single
//       timer query (including inside Present's spin loop!), triggering hundreds of
//       thousands of kernel syscalls/sec & Core-0 migration thrashing where NIC/USB
//       DPC interrupts execute. Modern Windows QPC is a ~15ns user-mode RDTSC read.
// ============================================================================

uint64_t __fastcall hk_NuccGetTimeUs() {
    if (g_BaseAddress) {
        const int32_t useQpc = *reinterpret_cast<const int32_t*>(g_BaseAddress + StormRva::kData_NuccTimerUseQpc);
        const int64_t freq   = *reinterpret_cast<const int64_t*>(g_BaseAddress + StormRva::kData_NuccTimerQpcFreq);
        if (useQpc != 0 && freq > 0) {
            LARGE_INTEGER now = {};
            if (QueryPerformanceCounter(&now)) {
                const int64_t startQpc = *reinterpret_cast<const int64_t*>(g_BaseAddress + StormRva::kData_NuccTimerStartQpc);
                const int64_t delta    = now.QuadPart - startQpc;
                if (delta <= 0) {
                    return 0ULL;
                }
                const uint64_t uDelta = static_cast<uint64_t>(delta);
                const uint64_t uFreq  = static_cast<uint64_t>(freq);
                return (uDelta / uFreq) * 1000000ULL + ((uDelta % uFreq) * 1000000ULL) / uFreq;
            }
        }
    }
    return oNuccGetTimeUs ? oNuccGetTimeUs() : 0ULL;
}

uint32_t __fastcall hk_NuccGetTimeMs() {
    if (g_BaseAddress) {
        const int32_t useQpc = *reinterpret_cast<const int32_t*>(g_BaseAddress + StormRva::kData_NuccTimerUseQpc);
        const int64_t freq   = *reinterpret_cast<const int64_t*>(g_BaseAddress + StormRva::kData_NuccTimerQpcFreq);
        if (useQpc != 0 && freq > 0) {
            LARGE_INTEGER now = {};
            if (QueryPerformanceCounter(&now)) {
                const int64_t startQpc = *reinterpret_cast<const int64_t*>(g_BaseAddress + StormRva::kData_NuccTimerStartQpc);
                const int64_t delta    = now.QuadPart - startQpc;
                if (delta <= 0) {
                    return 0U;
                }
                const uint64_t uDelta = static_cast<uint64_t>(delta);
                const uint64_t uFreq  = static_cast<uint64_t>(freq);
                return static_cast<uint32_t>((uDelta / uFreq) * 1000ULL + ((uDelta % uFreq) * 1000ULL) / uFreq);
            }
        }
    }
    return oNuccGetTimeMs ? oNuccGetTimeMs() : 0U;
}

void __fastcall hk_NuccD3D11Present(void* pNuccD3D11) {
    ApplyRenderThreadMmcssBoostOnce();

    auto* pMutableBytes = reinterpret_cast<uint8_t*>(pNuccD3D11);
    if (g_Config.enable_low_latency_render && pMutableBytes) {
        auto* pDevice = *reinterpret_cast<ID3D11Device* const*>(pMutableBytes + 0x48);

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

    // Bypass vanilla's priority-starving Sleep(0) + 16666us drifting busy-spin loop inside
    // nuccD3D11Device::Present (+0x14173A7..+0x14173ED) while still executing IDXGISwapChain::Present
    // and any Steam/OBS/RTSS overlays unmodified, then apply exact drift-free 60.000Hz pacing.
    const bool usePrecisionPacer = g_Config.enable_precision_frame_pacer && pMutableBytes && (hk_NuccGetTimeUs() >= 50000ULL);
    bool     vsyncEnabled   = false;
    bool     bypassedVsync  = false;
    uint8_t  savedVsyncByte = 0;
    uint32_t frameDiv       = 0;
    if (usePrecisionPacer) {
        savedVsyncByte = pMutableBytes[0x128];
        vsyncEnabled   = (savedVsyncByte != 0);
        frameDiv       = *reinterpret_cast<const uint32_t*>(pMutableBytes + 0x12C);
        *reinterpret_cast<uint64_t*>(pMutableBytes + 0x130) = 0ULL;

        if (vsyncEnabled) {
            auto* pSwapChain = *reinterpret_cast<IDXGISwapChain* const*>(pMutableBytes + 0x60);
            if (ShouldBypassWindowedNon60HzVsync(pSwapChain)) {
                pMutableBytes[0x128] = 0; // Present(0, 0) avoids 75/100/144/165Hz VBlank beat in DWM/VRR windowed mode
                vsyncEnabled         = false;
                bypassedVsync        = true;
            }
        }
    }

    oNuccD3D11Present(pNuccD3D11);

    if (usePrecisionPacer) {
        if (bypassedVsync) {
            pMutableBytes[0x128] = savedVsyncByte;
        }
        PaceFrameExact(frameDiv, vsyncEnabled);
        *reinterpret_cast<uint64_t*>(pMutableBytes + 0x130) = hk_NuccGetTimeUs();
    }
}

void ApplyWifiGamingLowLatencyMode(bool verboseLog) noexcept {
    if (!g_Config.enable_wifi_gaming_mode) {
        return;
    }

    if (!s_hWlanApiModule) {
        s_hWlanApiModule = LoadLibraryW(L"wlanapi.dll");
        if (!s_hWlanApiModule) {
            return; // System without WLAN service (e.g. Desktop Server / Wine)
        }
        auto fnOpen         = reinterpret_cast<fnWlanOpenHandle_t>(GetProcAddress(s_hWlanApiModule, "WlanOpenHandle"));
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

void ShutdownWifiGamingLowLatencyMode(bool processTerminating) noexcept {
    if (!processTerminating) {
        if (s_hWlanClientHandle && s_fnWlanCloseHandle) {
            s_fnWlanCloseHandle(s_hWlanClientHandle, nullptr);
        }
        if (s_hWlanApiModule) {
            FreeLibrary(s_hWlanApiModule);
        }
    }
    s_hWlanClientHandle = nullptr;
    s_hWlanApiModule    = nullptr;
    g_Telemetry.wifiGamingModeActive.store(false);
}

} // namespace StormInternal
