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

    // Input / Virtual Keyboard (fixes 0xC0000005 crash @ RVA 0x0120FFB5 & twin @ 0x0120FEE0,
    // plus ccNetIdSearchState Use-After-Free @ RVA 0x00170878)
    // ccVirtualKeyboard_Win64.cpp (+0x10C7473 & +0x10C7782) passes ([rbx+0x38] + 0x60) without testing
    // m_pKeyboard for NULL, passing 0x60 when GetKeyboardDevice(0) returns NULL.
    // ~ccNetIdSearchState (+0x016FBB0) never calls ccVirtualKeyboard::Close (+0x10C7910) or clears
    // [pVkImpl + 0xC0], leaving a dangling callback that crashes in OnVirtualKeyboardFinish (+0x0170878).
    constexpr uintptr_t kGlobalInputBackendPtr        = 0x9751E70; // VA 0x149751E70 (+0x08 -> +0x18 -> KeyboardBackend)
    constexpr uintptr_t kGlobalInputManagerPtr        = 0x9708328; // VA 0x149708328 (InputManager*, passed to GetKeyboardDevice)
    constexpr uintptr_t kInput_GetKeyboardDevice      = 0x120FE30; // VA 0x14120FE30 .. 0x14120FE53 (returns &m_keyboards[idx] or NULL)
    constexpr uintptr_t kInput_KeyboardGetPressedChar = 0x120FEB0; // VA 0x14120FEB0 .. 0x14120FF87 (faults @ +0x120FEE0)
    constexpr uintptr_t kInput_KeyboardGetPressedKey  = 0x120FF90; // VA 0x14120FF90 .. 0x141210036 (faults @ +0x120FFB5)
    constexpr uintptr_t kGlobalVirtualKeyboardPtr     = 0x2335D50; // VA 0x142335D50 (ccVirtualKeyboard*, +0x18 = ccVirtualKeyboard_Win64*)
    constexpr uintptr_t kVirtualKeyboard_Win64Reset   = 0x10C78A0; // VA 0x1410C78A0 .. 0x1410C7906 (.pdata, closes active VK & frees buffer)
    constexpr uintptr_t kNetIdSearch_Dtor             = 0x016FBB0; // VA 0x14016FBB0 .. 0x14016FD89 (.pdata, ~ccNetIdSearchState)
    constexpr uintptr_t kNetIdSearch_OnVkFinish       = 0x0170840; // VA 0x140170840 .. 0x14017095D (.pdata, faults @ +0x0170878)
    constexpr uintptr_t kVtbl_NetIdSearchState        = 0x179A638; // VA 0x14179A638 (ccNetIdSearchState vtable)
    constexpr uintptr_t kVtbl_SimpleCmdNetIdSearch    = 0x179A840; // VA 0x14179A840 (ccSimpleCommand<ccNetIdSearchState> vtable)

    // Session Abrupt-Disconnect Null-Deref, ChatTemplate UAF & Custom/Private Lobby Duplicate-Player Fixes
    constexpr uintptr_t kCustomLobby_UpdateBattleTable = 0x00F4310; // VA 0x1400F4310 .. 0x1400F4A65 (BattleTable change queue)
    constexpr uintptr_t kCustomLobby_RecvBattleTable   = 0x00F6F30; // VA 0x1400F6F30 .. 0x1400F73A2 (Host BattleTable packet callback)
    constexpr uintptr_t kCustomLobbyUi_UpdateUserList  = 0x00FF810; // VA 0x1400FF810 .. 0x1400FFBA0 (Custom Lobby UI player rows)
    constexpr uintptr_t kChatTemplate_ClearDynamic     = 0x0150C90; // VA 0x140150C90 .. 0x140150DD4 (faults @ +0x0150CC8 on NULL/dangling entry)
    // ccSpSkillCustomizeParam (data/spc/spSkillCustomizeParam.xfbin) OOB index fixes (float[5]: 0..4, msg[4]: 0..3):
    // None of the 5 indexed accessors bounds-check r8d, causing 0xC0000005 @ +0x0702447 (and twins) on corrupted local or remote presets.
    constexpr uintptr_t kSpSkill_GetFloatParam1        = 0x0702220; // VA 0x140702220 .. 0x140702266 (leaf, float[5] @ +0x2C, faults @ +0x070225B)
    constexpr uintptr_t kSpSkill_GetFloatParam2        = 0x0702270; // VA 0x140702270 .. 0x1407022B6 (leaf, float[5] @ +0x40, faults @ +0x07022AB)
    constexpr uintptr_t kSpSkill_GetMsgIdPtr           = 0x07022C0; // VA 0x1407022C0 .. 0x140702307 (leaf, entry[4] @ +0x58, faults @ +0x07022FE)
    constexpr uintptr_t kSpSkill_GetDwordParam         = 0x0702310; // VA 0x140702310 .. 0x140702359 (leaf, entry[4] @ +0x60, faults @ +0x0702352)
    constexpr uintptr_t kSpSkill_GetMessageId          = 0x0702400; // VA 0x140702400 .. 0x140702493 (.pdata, entry[4] @ +0x58, faults @ +0x0702447)
    constexpr uintptr_t kSaveData_SyncShadow           = 0x0AEED10; // VA 0x140AEED10 .. 0x140AEED56 (.pdata, copies g_MainSaveData -> Shadow)
    constexpr uintptr_t kData_MainSavePreSetArray      = 0x225B7F0; // VA 0x14225B7F0 (ccPreSetSaveData::m_presets[24], stride 0x1C0)
    constexpr uintptr_t kTeamContainer_GetByIndex      = 0x094B3E0; // VA 0x14094B3E0 .. 0x14094B44C (faults @ +0x094B3ED when _Myhead=0)
    constexpr uintptr_t kTeamContainer_FindByCharId    = 0x094B830; // VA 0x14094B830 .. 0x14094B8B7 (faults @ +0x094B837 when _Myhead=0)
    constexpr uintptr_t kTeamContainer_FindByIndex     = 0x094B8C0; // VA 0x14094B8C0 .. 0x14094B96E (faults @ +0x094B8D8 when _Myhead=0)
    constexpr uintptr_t kSession_GetMySlot             = 0x10B8E20; // VA 0x1410B8E20 .. 0x1410B8E36 (returns [session + 0x500])
    constexpr uintptr_t kSessionData_AddMember         = 0x113AF60; // VA 0x14113AF60 .. 0x14113B3BF (adds SteamID to m_slots[0..7])
    constexpr uintptr_t kSession_AssignNextUId         = 0x113B3C0; // VA 0x14113B3C0 .. 0x14113B3CE (assigns ++[session+0x168] to slot->uId)
    constexpr uintptr_t kSession_FindSlotBySteamId     = 0x113BDC0; // VA 0x14113BDC0 .. 0x14113BE49 (finds slot* in m_slots by SteamID)
    constexpr uintptr_t kSession_CheckExistingNewMbr   = 0x113C8B0; // VA 0x14113C8B0 .. 0x14113C9E0 (State1_Update existing SteamID check)
    constexpr uintptr_t kSession_OnMemberLeft          = 0x113D1E0; // VA 0x14113D1E0 .. 0x14113D63E (.pdata, erases self on Steam LobbyChatUpdate -> zombie 0-member lobby)
    constexpr uintptr_t kPatch_LobbyState1SortFix      = 0x113D736; // VA 0x14113D736 .. 0x14113D797 (fixes stale rdi after qsort in State1_Update)
    constexpr uintptr_t kSession_InvokeCallback        = 0x113D840; // VA 0x14113D840 .. 0x14113D889 (invokes OnMemberJoined callback)
    constexpr uintptr_t kSession_SortSlots             = 0x113FA50; // VA 0x14113FA50 .. 0x14113FADB (qsort m_slots & refresh [session+0x500])
    constexpr uintptr_t kSessionComm_DestroySockets    = 0x1168E20; // VA 0x141168E20 .. 0x141168E91 (.pdata, new vs delete[] mismatch -> 0xC0000005 @ +0x117116C)
    constexpr uintptr_t kSessionComm_SendPacket        = 0x1169290; // VA 0x141169290 .. 0x14116930C (faults @ +0x11692A3 when RCX=0)
    constexpr uintptr_t kSessionComm_BroadcastPacket   = 0x1169310; // VA 0x141169310 .. 0x14116938F (faults @ +0x1169320 when RCX=0)
    constexpr uintptr_t kSession_NewMemberTblAdd       = 0x1169E20; // VA 0x141169E20 .. 0x141169FBA (NewMemberTbl::Add queue insert)
    constexpr uintptr_t kSession_ProcessMessages       = 0x116DAE0; // VA 0x14116DAE0 .. 0x14116E02B (session packet dispatcher)

    // ccTaskManager iterator-invalidation UAF & parallel lock fixes (fixes 0xC0000005 @ +0x0109C8E0)
    constexpr uintptr_t kTask_Exec                     = 0x109C8E0; // VA 0x14109C8E0 .. 0x14109C8F3 (leaf, faults @ +0x0109C8E0 `cmp [rcx+0x20], 0`)
    constexpr uintptr_t kTaskMgr_AddTask               = 0x10D6200; // VA 0x1410D6200 .. 0x1410D62F8 (.pdata, missing [this+0x1470] lock)
    constexpr uintptr_t kTaskMgr_UpdateRange           = 0x10D6740; // VA 0x1410D6740 .. 0x1410D6AA0 (.pdata, reads [rbx] AFTER ccTask::Exec!)
    constexpr uintptr_t kTaskMgr_UpdateSingleParallel  = 0x10D6AA0; // VA 0x1410D6AA0 .. 0x1410D6CE7 (.pdata, parallel worker dispatch)
    constexpr uintptr_t kTaskMgr_RemoveTask            = 0x10D6FE0; // VA 0x1410D6FE0 .. 0x1410D70D8 (.pdata, zeroes & frees node during iteration)
    constexpr uintptr_t kTaskMgr_DeleteAllTasks        = 0x10D70E0; // VA 0x1410D70E0 .. 0x1410D71CD (.pdata, destroys all tasks in 128 buckets)
    constexpr uintptr_t kNuccCs_Lock                   = 0x12A7920; // VA 0x1412A7920 (nuccCriticalSection::Lock, recursive)
    constexpr uintptr_t kNuccCs_Unlock                 = 0x12A7990; // VA 0x1412A7990 (nuccCriticalSection::Unlock)

    // NetFes (ccNetEvergreenFesParamManager & ccNetFesParamManager) async HTTP callback & m_pUpdateTask UAF fixes
    // Fixes 0xC0000005 @ NSUNSC.exe+0x00B6530B (`call qword ptr [rax + 0x20]` on dangling/freed m_pUpdateTask or manager) + 16 twin lambdas
    constexpr uintptr_t kEvergreenFes_Ctor             = 0x0B64080; // VA 0x140B64080 .. 0x140B64649 (.pdata, size 0x5D0, m_pUpdateTask @ +0xB8)
    constexpr uintptr_t kEvergreenFes_Dtor             = 0x0B64C40; // VA 0x140B64C40 .. 0x140B64EF5 (.pdata, ~ccNetEvergreenFesParamManager)
    constexpr uintptr_t kNetFes_Ctor                   = 0x0B6DE00; // VA 0x140B6DE00 .. 0x140B6E266 (.pdata, size 0x4D0, m_pUpdateTask @ +0x88)
    constexpr uintptr_t kNetFes_Dtor                   = 0x0B6E420; // VA 0x140B6E420 .. 0x140B6E6E2 (.pdata, ~ccNetFesParamManager)
    constexpr uintptr_t kTaskCertainExe_SDtor          = 0x00B57B0; // VA 0x1400B57B0 .. 0x1400B57E4 (.pdata, ccTaskCertainExe scalar deleting dtor)
    constexpr uintptr_t kVtbl_EvergreenFesParamMgr     = 0x1997A88; // VA 0x141997A88 (ccNetEvergreenFesParamManager vtable)
    constexpr uintptr_t kVtbl_NetFesParamMgr           = 0x1998B18; // VA 0x141998B18 (ccNetFesParamManager vtable)
    constexpr uintptr_t kVtbl_TaskCertainExe           = 0x1761A08; // VA 0x141761A08 (ccTaskCertainExe vtable for "ccNetFesParamManager::Update")

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
    constexpr uintptr_t kFreeSync_SendWatcherRelayPacket = 0x10F3D70; // VA 0x1410F3D70 .. 0x1410F4098
    constexpr uintptr_t kFreeSync_UpdateMemberDelay     = 0x10F40D0; // VA 0x1410F40D0 .. 0x1410F41F8
    constexpr uintptr_t kFreeSync_ExtractFramePkt   = 0x10F4200; // VA 0x1410F4200 .. 0x1410F426F
    constexpr uintptr_t kFreeSync_StartFrameSync    = 0x10F4270; // VA 0x1410F4270 .. 0x1410F42A4

    // ccNetUDPSocket (Steam ISteamNetworking P2P channel 6 wrapper)
    constexpr uintptr_t kUdpSocket_Recv             = 0x1162140; // VA 0x141162140 .. 0x1411623A8
    constexpr uintptr_t kUdpSocket_Send             = 0x11623B0; // VA 0x1411623B0 .. 0x14116241E

    // nuccD3D11Device (DirectX 11 / DXGI presentation & frame-latency pipeline)
    constexpr uintptr_t kNuccD3D11_Present          = 0x1417370; // VA 0x141417370 .. 0x1414173FF (+0x48=ID3D11Device*, +0x50=Context*, +0x60=SwapChain*)

    // nuccTimer (eliminates obsolete 2006 SetThreadAffinityMask Core-0 migration on every timer query)
    constexpr uintptr_t kNuccTimer_GetTimeUs        = 0x13D7490; // VA 0x1413D7490 .. 0x1413D75A9
    constexpr uintptr_t kNuccTimer_GetTimeMs        = 0x13D75B0; // VA 0x1413D75B0 .. 0x1413D76CA
    constexpr uintptr_t kData_NuccTimerUseQpc       = 0x97518E4; // VA 0x1497518E4 (int32_t m_UseQpc)
    constexpr uintptr_t kData_NuccTimerQpcFreq      = 0x97518E8; // VA 0x1497518E8 (int64_t m_QpcFreq)
    constexpr uintptr_t kData_NuccTimerStartQpc     = 0x97518F0; // VA 0x1497518F0 (int64_t m_StartQpc)

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
    // 7) nuccFree at-exit heap corruption crash (0xC0000374 in ntdll!RtlReportFatalFailure when NUCC allocator is destroyed)
    constexpr uintptr_t kPatch_NuccFreeAtExitCrash  = 0x12732D8; // VA 0x1412732D8 (`48 85 C0 74 17` -> `74 20`)
    // 8-10) ccStateChatWindow exit blocks (+0x16A795, +0x16A8CB, +0x16AAE1):
    //       Unload("data/network/chatTemplateParam.bin.xfbin") executes BEFORE DestroySingleton(0x140150950),
    //       freeing static entry buffers while m_ptrVec still points into them (UAF 0xC0000005 @ +0x0150CC8).
    constexpr uintptr_t kPatch_ChatWindowExit1      = 0x016A795; // VA 0x14016A795 .. 0x14016A7AD (24 bytes)
    constexpr uintptr_t kPatch_ChatWindowExit2      = 0x016A8CB; // VA 0x14016A8CB .. 0x14016A8E3 (24 bytes)
    constexpr uintptr_t kPatch_ChatWindowExit3      = 0x016AAE1; // VA 0x14016AAE1 .. 0x14016AAF9 (24 bytes)
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
        int32_t          frameNumber;     // +0x00: When active in m_pResendQueueHead (+0x250)
        ccNetResendNode* pNextFree;       // +0x00: When on free list m_pResendFreeList (+0x248)
    };
    ccNetResendNode* pNextQueue;          // +0x08: next node in m_pResendQueueHead
};
static_assert(sizeof(ccNetResendNode) == 0x10, "ccNetResendNode must be 0x10 bytes");
static_assert(offsetof(ccNetResendNode, pNextQueue) == 0x08, "ccNetResendNode::pNextQueue offset mismatch");

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
    // null-pointer SSE write crashes at +0x10F1455 & +0x10F2928 when the 16-slot resend pool fills,
    // lobby slot null-deref at +0x113D7F2, plus ccVirtualKeyboard null m_pKeyboard derefs at +0x120FFB5/+0x120FEE0.
    bool fix_online_persona_crash  = true;
    bool fix_lobby_wait_deadlock   = true;

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
    int  jitter_margin_ms      = 2;    // Tight VSync-phase guard (2ms default, 0..10) + automatic +3ms + RTT/13 route cushion

    // DirectX 11 Zero-Queue Low-Latency Render & OS Gaming Scheduler Boost
    bool enable_low_latency_render    = true;
    int  max_frame_latency            = 1;    // IDXGIDevice1::SetMaximumFrameLatency(1) + GPUThreadPriority(+5)
    bool enable_scheduler_boost       = true; // Win11 EcoQoS/Timer-Coalescing OFF + 0.5ms timer + MMCSS "Games" + lock-free QPC timer
    bool enable_precision_frame_pacer = true; // Replaces vanilla Sleep(0)+16666us drifting spin with exact 60.000Hz WaitableTimer pacer
    bool bypass_non60hz_vsync_judder  = true; // Bypasses Present(1,0) VBlank beat on windowed non-60Hz displays (75/100/144/165Hz)

    // Multi-frame UDP packet redundancy (Vanilla-lookahead-safe packing + single-record auxiliary UDP sends)
    bool enable_packet_redundancy = true;
    int  redundancy_frame_depth   = 3; // Send [frame - 2, frame - 1, frame] per tick (1..4 battler, 1..5 spectator)
    int  max_resend_batch         = 3; // Drain up to 3 NACK resend requests per tick (vanilla drains 1)

    // Micro-jitter absorption, Wi-Fi anti-scan gaming mode & stall governance
    bool enable_wifi_gaming_mode      = true; // Win32 wlanapi MediaStreaming=ON + BackgroundScan=OFF while game runs
    bool enable_dynamic_peer_autotune = true; // Auto +1/+2f rematch cushion if peer exceeds 1.8% / 12% mid-fight net stalls
    bool enable_micro_jitter_poll     = true;
    int  micro_jitter_spin_us         = 1100; // High-precision QPC poll (1100us on tick 0, 650us on tick 1, 25us spacing)
    bool fast_nack_recovery           = true; // Trigger RTT-paced accelerated NACK (2f initial + RTT-spaced retries)
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
    std::atomic<uint64_t> virtualKeyboardNullSaves{0};
    std::atomic<uint64_t> sessionDisconnectNullSaves{0};
    std::atomic<uint64_t> duplicateLobbySlotSaves{0};
    std::atomic<uint64_t> lobbyWaitDeadlockSaves{0};
    std::atomic<uint64_t> chatTemplateCrashSaves{0};
    std::atomic<uint64_t> spSkillOobSaves{0};
    std::atomic<uint64_t> taskManagerCrashSaves{0};
    std::atomic<uint64_t> netFesCrashSaves{0};
};

extern HMODULE          g_hSelfModule;
extern NetcodeConfig    g_Config;
extern NetcodeTelemetry g_Telemetry;

void LogNetcode(const char* fmt, ...);
void LoadConfig(const std::wstring& configPath);
void WriteDefaultConfigIfMissing(const std::wstring& configPath);
void InitializeNetcodeHooks();
void ShutdownNetcodeHooks(bool processTerminating = false);
