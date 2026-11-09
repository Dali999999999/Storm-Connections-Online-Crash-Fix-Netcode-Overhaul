#include <windows.h>
#include <cstdint>
#include <string>

extern HMODULE g_hSelfModule;

namespace {

using GenericD3DFn = uintptr_t(WINAPI*)(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t,
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);

enum ExportIndex : size_t {
    idx_D3DAssemble = 0,
    idx_DebugSetMute,
    idx_D3DCompile,
    idx_D3DCompile2,
    idx_D3DCompileFromFile,
    idx_D3DCompressShaders,
    idx_D3DCreateBlob,
    idx_D3DCreateFunctionLinkingGraph,
    idx_D3DCreateLinker,
    idx_D3DDecompressShaders,
    idx_D3DDisassemble,
    idx_D3DDisassemble10Effect,
    idx_D3DDisassemble11Trace,
    idx_D3DDisassembleRegion,
    idx_D3DGetBlobPart,
    idx_D3DGetDebugInfo,
    idx_D3DGetInputAndOutputSignatureBlob,
    idx_D3DGetInputSignatureBlob,
    idx_D3DGetOutputSignatureBlob,
    idx_D3DGetTraceInstructionOffsets,
    idx_D3DLoadModule,
    idx_D3DPreprocess,
    idx_D3DReadFileToBlob,
    idx_D3DReflect,
    idx_D3DReflectLibrary,
    idx_D3DReturnFailure1,
    idx_D3DSetBlobPart,
    idx_D3DStripShader,
    idx_D3DWriteBlobToFile,
    kExportCount
};

constexpr const char* kExportNames[kExportCount] = {
    "D3DAssemble",
    "DebugSetMute",
    "D3DCompile",
    "D3DCompile2",
    "D3DCompileFromFile",
    "D3DCompressShaders",
    "D3DCreateBlob",
    "D3DCreateFunctionLinkingGraph",
    "D3DCreateLinker",
    "D3DDecompressShaders",
    "D3DDisassemble",
    "D3DDisassemble10Effect",
    "D3DDisassemble11Trace",
    "D3DDisassembleRegion",
    "D3DGetBlobPart",
    "D3DGetDebugInfo",
    "D3DGetInputAndOutputSignatureBlob",
    "D3DGetInputSignatureBlob",
    "D3DGetOutputSignatureBlob",
    "D3DGetTraceInstructionOffsets",
    "D3DLoadModule",
    "D3DPreprocess",
    "D3DReadFileToBlob",
    "D3DReflect",
    "D3DReflectLibrary",
    "D3DReturnFailure1",
    "D3DSetBlobPart",
    "D3DStripShader",
    "D3DWriteBlobToFile"
};

INIT_ONCE    g_InitOnce        = INIT_ONCE_STATIC_INIT;
HMODULE      g_hRealD3D        = nullptr;
GenericD3DFn g_FnTable[kExportCount] = {};

BOOL CALLBACK InitProxyCallback(PINIT_ONCE, PVOID, PVOID*) {
    // 1. Check for d3dcompiler_47_original.dll or d3dcompiler_47_o.dll in executable directory
    WCHAR exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring dir(exePath);
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            const wchar_t* candidates[] = {
                L"\\d3dcompiler_47_original.dll",
                L"\\d3dcompiler_47_o.dll"
            };
            for (const wchar_t* suffix : candidates) {
                std::wstring chainPath = dir.substr(0, pos) + suffix;
                if (GetFileAttributesW(chainPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    HMODULE hCand = LoadLibraryW(chainPath.c_str());
                    if (hCand && hCand != g_hSelfModule) {
                        g_hRealD3D = hCand;
                        break;
                    }
                }
            }
        }
    }

    // 2. Fallback to System32\d3dcompiler_47.dll
    if (!g_hRealD3D) {
        WCHAR sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        std::wstring sysDll = std::wstring(sysDir) + L"\\d3dcompiler_47.dll";
        g_hRealD3D = LoadLibraryW(sysDll.c_str());
    }

    if (!g_hRealD3D) {
        return FALSE;
    }

    for (size_t i = 0; i < kExportCount; ++i) {
        g_FnTable[i] = reinterpret_cast<GenericD3DFn>(GetProcAddress(g_hRealD3D, kExportNames[i]));
    }
    return TRUE;
}

__forceinline GenericD3DFn GetExportFn(ExportIndex idx) {
    InitOnceExecuteOnce(&g_InitOnce, InitProxyCallback, nullptr, nullptr);
    return g_FnTable[idx];
}

#define DEFINE_D3D_EXPORT(Name)                                                                 \
    extern "C" uintptr_t WINAPI Name(                                                           \
        uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4,                                 \
        uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8,                                 \
        uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)                              \
    {                                                                                           \
        GenericD3DFn fn = GetExportFn(idx_##Name);                                              \
        if (!fn) return static_cast<uintptr_t>(E_FAIL);                                         \
        return fn(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);                           \
    }

} // namespace

DEFINE_D3D_EXPORT(D3DAssemble)
DEFINE_D3D_EXPORT(DebugSetMute)
DEFINE_D3D_EXPORT(D3DCompile)
DEFINE_D3D_EXPORT(D3DCompile2)
DEFINE_D3D_EXPORT(D3DCompileFromFile)
DEFINE_D3D_EXPORT(D3DCompressShaders)
DEFINE_D3D_EXPORT(D3DCreateBlob)
DEFINE_D3D_EXPORT(D3DCreateFunctionLinkingGraph)
DEFINE_D3D_EXPORT(D3DCreateLinker)
DEFINE_D3D_EXPORT(D3DDecompressShaders)
DEFINE_D3D_EXPORT(D3DDisassemble)
DEFINE_D3D_EXPORT(D3DDisassemble10Effect)
DEFINE_D3D_EXPORT(D3DDisassemble11Trace)
DEFINE_D3D_EXPORT(D3DDisassembleRegion)
DEFINE_D3D_EXPORT(D3DGetBlobPart)
DEFINE_D3D_EXPORT(D3DGetDebugInfo)
DEFINE_D3D_EXPORT(D3DGetInputAndOutputSignatureBlob)
DEFINE_D3D_EXPORT(D3DGetInputSignatureBlob)
DEFINE_D3D_EXPORT(D3DGetOutputSignatureBlob)
DEFINE_D3D_EXPORT(D3DGetTraceInstructionOffsets)
DEFINE_D3D_EXPORT(D3DLoadModule)
DEFINE_D3D_EXPORT(D3DPreprocess)
DEFINE_D3D_EXPORT(D3DReadFileToBlob)
DEFINE_D3D_EXPORT(D3DReflect)
DEFINE_D3D_EXPORT(D3DReflectLibrary)
DEFINE_D3D_EXPORT(D3DReturnFailure1)
DEFINE_D3D_EXPORT(D3DSetBlobPart)
DEFINE_D3D_EXPORT(D3DStripShader)
DEFINE_D3D_EXPORT(D3DWriteBlobToFile)
