#include "netcode_mod.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <share.h>

HMODULE g_hSelfModule = nullptr;

namespace {
std::mutex g_LogMutex;
FILE*      g_LogFile = nullptr;

std::wstring TrimWide(const std::wstring& s) {
    const size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}
} // namespace

void LogNetcode(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_Config.enable_console) {
        std::printf("[%02d:%02d:%02d.%03d] %s\n",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    }
    if (g_LogFile) {
        std::fprintf(g_LogFile, "[%02d:%02d:%02d.%03d] %s\n",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        std::fflush(g_LogFile);
    }
}

void WriteDefaultConfigIfMissing(const std::wstring& configPath) {
    if (GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::ofstream out(configPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return;

    out << "; ============================================================================\n"
        << "; NARUTO X BORUTO Ultimate Ninja STORM CONNECTIONS - Online Crash Fix & Netcode v2.7\n"
        << "; Reverse-Engineered & Verified (.pdata / SteamFriends017 / ccNetFreeBattleSync / D3D11 / WLAN / MMCSS)\n"
        << "; ============================================================================\n\n"
        << "[General]\n"
        << "EnableConsole = 1\n"
        << "EnableFileLog = 1\n"
        << "EnableNetcodeOverhaul = 1\n\n"
        << "[CrashFix]\n"
        << "; Fixes deterministic 0xC0000005 online crashes at:\n"
        << ";   - NSUNSC.exe+0x010BAD24 & +0x0847A67 (SteamFriends017 SSE 4KB page over-read)\n"
        << ";   - NSUNSC.exe+0x010F1455 & +0x010F2928 (16-slot resend pool null-pointer SSE write)\n"
        << "FixOnlinePersonaCrash = 1\n\n"
        << "[InputDelay]\n"
        << "EnableCustomDelay = 1\n"
        << "; DelayMode:\n"
        << ";   1 = Smart Peer-Safe One-Way Delay (DEFAULT: negotiates reduced OWD delay via Host RTT\n"
        << ";       with automatic VSync-phase & route-jitter guard; 100% compatible with ALL opponents)\n"
        << ";   2 = Direct Modded-Peer Override (use only when both players share identical settings)\n"
        << ";   0 = Vanilla Delay (keeps Vanilla-Safe packet redundancy & crash fixes active)\n"
        << "DelayMode = 1\n"
        << "; ForcedDelayFrames: -1 = Adaptive One-Way Latency (RTT/2 + phase/route cushion), or 1..16 for fixed\n"
        << "ForcedDelayFrames = -1\n"
        << "; MinDelayFrames: Protocol mathematical minimum is 1 frame (16.67ms at 60 FPS)\n"
        << "MinDelayFrames = 1\n"
        << "; MaxDelayClamp: 16 allows smooth stall-free play even on 300ms+ transcontinental matches\n"
        << "MaxDelayClamp = 16\n"
        << "; JitterMarginMs: Phase-safe cushion (5ms + automatic RTT/20 route scaling)\n"
        << "JitterMarginMs = 5\n\n"
        << "[LowLatencyRender]\n"
        << "; Forces IDXGIDevice1::SetMaximumFrameLatency(1) + GPUThreadPriority(+5),\n"
        << "; eliminating ~33.3 ms (2 frames) of Windows default 3-frame DXGI render-queue lag.\n"
        << "EnableLowLatencyRender = 1\n"
        << "MaxFrameLatency = 1\n"
        << "; Disables Win11 EcoQoS/Timer-Coalescing, locks 1ms kernel timer & enables MMCSS 'Games' thread priority\n"
        << "EnableSchedulerBoost = 1\n\n"
        << "[PacketRedundancy]\n"
        << "; Vanilla-Lookahead-Safe multi-frame UDP redundancy + burst-loss window persistence.\n"
        << "; Validated against NSUNSC.exe+0x10F299B so unmodded opponents NEVER drop mixed-size packets.\n"
        << "EnablePacketRedundancy = 1\n"
        << "RedundancyFrameDepth = 3\n"
        << "MaxResendBatch = 3\n\n"
        << "[JitterAndStallGovernance]\n"
        << "; Prevents Windows 65-second Wi-Fi background channel scans & enables Media Streaming low-latency\n"
        << "; mode via Win32 wlanapi.dll while NSUNSC.exe is running (automatically restored on game exit).\n"
        << "EnableWifiGamingMode = 1\n"
        << "; Automatically adds +1/+2 frame cushion on rematches if a peer had >=3% / >=18% mid-fight stalls\n"
        << "EnableDynamicPeerAutoTune = 1\n"
        << "; High-precision dual-edge QPC poll (1100us tick 0, 650us tick 1, 25us step) across 16.67ms VSync boundaries\n"
        << "EnableMicroJitterPoll = 1\n"
        << "MicroJitterSpinUs = 1100\n"
        << "; Triggers accelerated NACK at 2 stalled frames (33ms) + single backup retry at 7 frames (116ms)\n"
        << "FastNackRecovery = 1\n"
        << "; Suppresses the intrusive dark 'Transmitting...' modal on brief spikes (< 180 frames / 3.0s)\n"
        << "SuppressShortOverlay = 1\n"
        << "OverlayThresholdFrames = 180\n";
}

void LoadConfig(const std::wstring& configPath) {
    WriteDefaultConfigIfMissing(configPath);

    std::wifstream file(configPath);
    if (!file.is_open()) {
        return;
    }

    std::wstring line;
    while (std::getline(file, line)) {
        std::wstring trimmed = TrimWide(line);
        if (trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#' || trimmed[0] == L'[') {
            continue;
        }

        // Strip inline comments
        size_t commentPos = trimmed.find_first_of(L";#");
        if (commentPos != std::wstring::npos) {
            trimmed = TrimWide(trimmed.substr(0, commentPos));
        }

        size_t eqPos = trimmed.find(L'=');
        if (eqPos == std::wstring::npos) continue;

        std::wstring key = TrimWide(trimmed.substr(0, eqPos));
        std::wstring val = TrimWide(trimmed.substr(eqPos + 1));
        if (key.empty() || val.empty()) continue;

        int intVal = _wtoi(val.c_str());

        if (key == L"EnableConsole") {
            g_Config.enable_console = (intVal != 0);
        } else if (key == L"EnableFileLog") {
            g_Config.enable_file_log = (intVal != 0);
        } else if (key == L"FixOnlinePersonaCrash") {
            g_Config.fix_online_persona_crash = (intVal != 0);
        } else if (key == L"EnableNetcodeOverhaul") {
            g_Config.enable_netcode_overhaul = (intVal != 0);
        } else if (key == L"EnableCustomDelay") {
            g_Config.enable_custom_delay = (intVal != 0);
        } else if (key == L"DelayMode") {
            g_Config.delay_mode = std::clamp(intVal, 0, 2);
        } else if (key == L"ForcedDelayFrames") {
            // Backwards compatibility: old config used 0 to mean "lowest adaptive/1-frame delay"
            g_Config.forced_delay_frames = (intVal <= 0) ? -1 : std::clamp(intVal, 1, 16);
        } else if (key == L"MinDelayFrames") {
            g_Config.min_delay_frames = std::clamp(intVal, 1, 16);
        } else if (key == L"MaxDelayClamp") {
            g_Config.max_delay_clamp = std::clamp(intVal, 1, 16);
        } else if (key == L"JitterMarginMs") {
            if (intVal == 8) intVal = 5; // Upgrade legacy v2.6 default to optimal 5ms
            g_Config.jitter_margin_ms = std::clamp(intVal, 0, 50);
        } else if (key == L"EnableLowLatencyRender") {
            g_Config.enable_low_latency_render = (intVal != 0);
        } else if (key == L"MaxFrameLatency") {
            g_Config.max_frame_latency = std::clamp(intVal, 1, 3);
        } else if (key == L"EnableSchedulerBoost") {
            g_Config.enable_scheduler_boost = (intVal != 0);
        } else if (key == L"EnablePacketRedundancy") {
            g_Config.enable_packet_redundancy = (intVal != 0);
        } else if (key == L"RedundancyFrameDepth") {
            g_Config.redundancy_frame_depth = std::clamp(intVal, 1, 6);
        } else if (key == L"MaxResendBatch") {
            g_Config.max_resend_batch = std::clamp(intVal, 1, 4);
        } else if (key == L"EnableWifiGamingMode") {
            g_Config.enable_wifi_gaming_mode = (intVal != 0);
        } else if (key == L"EnableDynamicPeerAutoTune") {
            g_Config.enable_dynamic_peer_autotune = (intVal != 0);
        } else if (key == L"EnableMicroJitterPoll") {
            g_Config.enable_micro_jitter_poll = (intVal != 0);
        } else if (key == L"MicroJitterSpinUs") {
            if (intVal == 850) intVal = 1100; // Upgrade legacy v2.6 default to 1100us
            g_Config.micro_jitter_spin_us = std::clamp(intVal, 0, 2000);
        } else if (key == L"FastNackRecovery") {
            g_Config.fast_nack_recovery = (intVal != 0);
        } else if (key == L"SuppressShortOverlay" || key == L"DisableTaskPause") {
            g_Config.suppress_short_overlay = (intVal != 0);
        } else if (key == L"OverlayThresholdFrames") {
            g_Config.overlay_threshold_frames = std::clamp(intVal, 30, 1800);
        }
    }
}

DWORD WINAPI MainThread(LPVOID) {
    WCHAR modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring fullPath(modulePath);
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? fullPath.substr(0, lastSlash) : L".";

    std::wstring configPath = dir + L"\\netcode_config.ini";
    std::wstring logPath    = dir + L"\\storm_netcode.log";

    LoadConfig(configPath);

    if (g_Config.enable_file_log) {
        // Open with _SH_DENYNO (FILE_SHARE_READ | FILE_SHARE_WRITE) so external tools can read live logs
        g_LogFile = _wfsopen(logPath.c_str(), L"w", _SH_DENYNO);
    }

    if (g_Config.enable_console) {
        AllocConsole();
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        SetConsoleTitleA("NARUTO STORM CONNECTIONS - Netcode Overhaul Engine v2.7");
    }

    LogNetcode("========================================================================");
    LogNetcode("  NARUTO X BORUTO STORM CONNECTIONS - ONLINE CRASH FIX & NETCODE v2.7  ");
    LogNetcode("  Universal Matrix (30/60FPS) + Spectator Stream + True-RTT AutoTune   ");
    LogNetcode("========================================================================");
    LogNetcode("[Config] OnlinePersonaCrashFix=%s | NetcodeOverhaul=%s | LowLatencyD3D11=%s (MaxQ=%d, SchedBoost=%s)",
               g_Config.fix_online_persona_crash ? "ON" : "OFF",
               g_Config.enable_netcode_overhaul ? "ON" : "OFF",
               g_Config.enable_low_latency_render ? "ON" : "OFF",
               g_Config.max_frame_latency,
               g_Config.enable_scheduler_boost ? "ON" : "OFF");
    LogNetcode("[Config] DelayMode=%d | ForcedDelay=%d | MinFloor=%d f | MaxClamp=%d f | JitterMargin=%d ms | AutoTune=%s",
               g_Config.delay_mode, g_Config.forced_delay_frames,
               g_Config.min_delay_frames, g_Config.max_delay_clamp, g_Config.jitter_margin_ms,
               g_Config.enable_dynamic_peer_autotune ? "ON" : "OFF");
    LogNetcode("[Config] PacketRedundancy=%s (Depth=%d frames, MaxResendBatch=%d, VanillaSafe=ON)",
               g_Config.enable_packet_redundancy ? "ON" : "OFF",
               g_Config.redundancy_frame_depth, g_Config.max_resend_batch);
    LogNetcode("[Config] WifiGamingMode=%s | MicroJitterPoll=%s (%d us, 25us step) | FastNACK=%s | Overlay=%d f",
               g_Config.enable_wifi_gaming_mode ? "ON" : "OFF",
               g_Config.enable_micro_jitter_poll ? "ON" : "OFF",
               g_Config.micro_jitter_spin_us,
               g_Config.fast_nack_recovery ? "ON" : "OFF",
               g_Config.overlay_threshold_frames);
    LogNetcode("------------------------------------------------------------------------");

    InitializeNetcodeHooks();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_hSelfModule = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread) {
            CloseHandle(hThread);
        }
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        ShutdownNetcodeHooks();
        if (g_LogFile) {
            std::fclose(g_LogFile);
            g_LogFile = nullptr;
        }
    }
    return TRUE;
}
