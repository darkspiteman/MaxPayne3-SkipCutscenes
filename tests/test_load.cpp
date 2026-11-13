// Loads the built .asi into a process that is NOT Max Payne 3 and checks that it stays idle (touches nothing) and
// writes its log. Usage: test_load <path to MP3SkipCutscenes.asi>

#include <windows.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: test_load <asi>\n");
        return 2;
    }

    char logPath[MAX_PATH];
    lstrcpynA(logPath, argv[1], MAX_PATH - 4);
    char* dot = strrchr(logPath, '.');
    if (!dot)
        return 2;
    lstrcpyA(dot, ".log");
    DeleteFileA(logPath);

    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod)
    {
        printf("FAILED: LoadLibrary error %lu\n", GetLastError());
        return 1;
    }

    for (int i = 0; i < 50; ++i)
    {
        Sleep(100);
        HANDLE file = CreateFileA(logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            continue;
        char text[4096] = {};
        DWORD read = 0;
        ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
        CloseHandle(file);
        if (strstr(text, "host is not MaxPayne3.exe - staying idle"))
        {
            printf("test_load: ok, log:\n%s", text);
            return 0;
        }
    }
    printf("FAILED: the expected log line did not appear in %s\n", logPath);
    return 1;
}
