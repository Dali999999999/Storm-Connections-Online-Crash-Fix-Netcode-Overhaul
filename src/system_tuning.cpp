#include "netcode_internal.h"
#include <d3d11.h>
#include <dxgi.h>
#include <wlanapi.h>
#include <algorithm>

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
using fnTimeBeginPeriod_t       = UINT(WINAPI*)(UINT);
using fnTimeEndPeriod_t         = UINT(WINAPI*)(UINT);
using fnAvSetMmThreadCharsW_t   = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using fnAvSetMmThreadPriority_t = BOOL(WINAPI*)(HANDLE, int);
using fnAvRevertMmThreadChars_t = BOOL(WINAPI*)(HANDLE);

HMODULE                   s_hWinMmModule       = nullptr;
fnTimeEndPeriod_t         s_fnTimeEndPeriod    = nullptr;
bool                      s_TimerPeriodActive  = false;
HMODULE                   s_hAvrtModule        = nullptr;
HANDLE                    s_hMmcssHandle       = nullptr;
fnAvRevertMmThreadChars_t s_fnAvRevertMm       = nullptr;
std::atomic<bool>         s_MmcssThreadApplied{false};
ID3D11Device*             s_LastConfiguredD3D11Dev = nullptr;

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

    g_Telemetry.schedulerBoostActive.store(ecoQosDisabled || prioElevated || s_TimerPeriodActive);
    LogNetcode("[Scheduler] Gaming Boost active: EcoQoS/TimerCoalesce=%s | Priority=%s | 1msKernelTimer=%s",
               ecoQosDisabled ? "DISABLED" : "N/A",
               prioElevated ? "ABOVE_NORMAL" : "KEPT",
               s_TimerPeriodActive ? "LOCKED" : "DEFAULT");
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

} // namespace StormInternal
