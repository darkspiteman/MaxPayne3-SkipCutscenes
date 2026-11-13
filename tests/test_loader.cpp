// Checks the optional dinput8.dll proxy: copies it and the .asi into an empty temp folder, loads the proxy, creates
// a real DirectInput8 object through it and verifies that the .asi got loaded.
// Usage: test_loader <path to built dinput8.dll> <path to MP3SkipCutscenes.asi>

#include <windows.h>

#include <stdio.h>

namespace
{
    // {BF798030-483A-4DA2-AA99-5D64ED369700}
    const GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: test_loader <dinput8.dll> <asi>\n");
        return 2;
    }

    // A scratch folder next to this test executable (inside the build tree)
    char dir[MAX_PATH], dll[MAX_PATH * 2], asi[MAX_PATH * 2];
    DWORD len = GetModuleFileNameA(nullptr, dir, MAX_PATH);
    while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/')
        --len;
    dir[len] = 0;
    if (len == 0 || len > MAX_PATH - 32)
        return 2;
    lstrcatA(dir, "loader_test_run");
    CreateDirectoryA(dir, nullptr);
    wsprintfA(dll, "%s\\dinput8.dll", dir);
    wsprintfA(asi, "%s\\MP3SkipCutscenes.asi", dir);
    if (!CopyFileA(argv[1], dll, FALSE) || !CopyFileA(argv[2], asi, FALSE))
    {
        printf("FAILED: can't copy the files to %s (error %lu)\n", dir, GetLastError());
        return 1;
    }

    int result = 1;
    HMODULE proxy = LoadLibraryA(dll);
    if (!proxy)
    {
        printf("FAILED: LoadLibrary(%s) error %lu\n", dll, GetLastError());
    }
    else if (GetModuleHandleA("MP3SkipCutscenes.asi"))
    {
        printf("FAILED: the plugin was loaded inside DllMain\n");
    }
    else
    {
        using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        auto create = reinterpret_cast<CreateFn>(GetProcAddress(proxy, "DirectInput8Create"));
        bool exportsOk = create && GetProcAddress(proxy, "DllCanUnloadNow") &&
                         GetProcAddress(proxy, "DllGetClassObject") && GetProcAddress(proxy, "DllRegisterServer") &&
                         GetProcAddress(proxy, "DllUnregisterServer");
        if (!exportsOk)
        {
            printf("FAILED: missing exports\n");
        }
        else
        {
            IUnknown* input = nullptr;
            HRESULT hr = create(GetModuleHandleA(nullptr), 0x0800, kIID_IDirectInput8A,
                                reinterpret_cast<LPVOID*>(&input), nullptr);
            HMODULE plugin = GetModuleHandleA("MP3SkipCutscenes.asi");
            printf("DirectInput8Create -> 0x%08lX, object %p, plugin %p\n", static_cast<unsigned long>(hr),
                   static_cast<void*>(input), static_cast<void*>(plugin));
            if (SUCCEEDED(hr) && input && plugin)
                result = 0;
            if (input)
                input->Release();
            Sleep(500); // let the plugin's init thread write its log
        }
    }

    printf("test_loader: %s\n", result == 0 ? "ok" : "FAILED");
    printf("(scratch folder with both logs: %s)\n", dir);
    return result;
}
