// version.dll proxy entry point for the Species Unknown 5-player mod.
#include <windows.h>

extern "C" {
void* p_GetFileVersionInfoA;
void* p_GetFileVersionInfoByHandle;
void* p_GetFileVersionInfoExA;
void* p_GetFileVersionInfoExW;
void* p_GetFileVersionInfoSizeA;
void* p_GetFileVersionInfoSizeExA;
void* p_GetFileVersionInfoSizeExW;
void* p_GetFileVersionInfoSizeW;
void* p_GetFileVersionInfoW;
void* p_VerFindFileA;
void* p_VerFindFileW;
void* p_VerInstallFileA;
void* p_VerInstallFileW;
void* p_VerLanguageNameA;
void* p_VerLanguageNameW;
void* p_VerQueryValueA;
void* p_VerQueryValueW;
}

void StartFivePlayerMod(HMODULE self);  // fiveplayer.cpp

static void InitVersionProxy()
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 20) return;
    lstrcatW(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return;
#define RESOLVE(name) p_##name = (void*)GetProcAddress(real, #name)
    RESOLVE(GetFileVersionInfoA);
    RESOLVE(GetFileVersionInfoByHandle);
    RESOLVE(GetFileVersionInfoExA);
    RESOLVE(GetFileVersionInfoExW);
    RESOLVE(GetFileVersionInfoSizeA);
    RESOLVE(GetFileVersionInfoSizeExA);
    RESOLVE(GetFileVersionInfoSizeExW);
    RESOLVE(GetFileVersionInfoSizeW);
    RESOLVE(GetFileVersionInfoW);
    RESOLVE(VerFindFileA);
    RESOLVE(VerFindFileW);
    RESOLVE(VerInstallFileA);
    RESOLVE(VerInstallFileW);
    RESOLVE(VerLanguageNameA);
    RESOLVE(VerLanguageNameW);
    RESOLVE(VerQueryValueA);
    RESOLVE(VerQueryValueW);
#undef RESOLVE
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitVersionProxy();
        // Only patch the actual game process (version.dll may also be picked up by helper exes).
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* base = exe;
        for (const wchar_t* p = exe; *p; ++p) if (*p == L'\\' || *p == L'/') base = p + 1;
        if (lstrcmpiW(base, L"SpeciesUnknown-Win64-Shipping.exe") == 0)
            StartFivePlayerMod(hModule);
    }
    return TRUE;
}
