/* Rekarina as one file: Rekarina.exe carries the whole setup folder (tools/make_rekarina_release.py --single) and
   starts its launcher, unpacking the folder first where that has not been done.

   The file is this program, then a zip of the setup folder, then a trailer of 56 bytes: "RKSFX001", the zip's offset
   and size (two 64-bit little-endian numbers) and the version (32 bytes, zero padded). The folder goes to
   %LOCALAPPDATA%\Rekarina\setup\<version>; a file named unpacked.txt, written last, says it is whole. The zip is
   copied out of this file and unpacked by Windows' own tar.exe (bsdtar, part of Windows 10 since 1803), in a thread,
   while a small window says what is going on; then <folder>\Rekarina-Launcher.exe starts with this program's
   arguments (--unpack-only: unpack, start nothing). Updates are the launcher's own business from there
   (installer/update.py): they change that folder.

   Messages are in Russian on a Russian Windows and in English elsewhere, as the launcher exe's are.

   Build: llvm/bin/llvm-windres -I runtime -O coff installer/launcher/rekarina.rc -o rekarina_rc.o, then
   llvm/bin/clang -O2 -mwindows -municode installer/launcher/rekarina_single.c rekarina_rc.o -o Rekarina.exe */
#include <windows.h>
#include <wchar.h>

#define PATH_CHARS (MAX_PATH * 2)
#define TRAILER 56

static wchar_t g_self[PATH_CHARS], g_dir[PATH_CHARS], g_zip[PATH_CHARS];
static unsigned long long g_offset, g_size;
static HWND g_window;
static volatile LONG g_result = -1;   /* the unpacking thread's answer: 0 done, else where it failed */

static int russian(void) { return (GetUserDefaultUILanguage() & 0x3ff) == 0x19; }

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static void fail(const wchar_t *en, const wchar_t *ru) {
    MessageBoxW(NULL, russian() ? ru : en, L"Rekarina", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

/* The trailer of this file: the zip's place and the version. 0 when the file carries none. */
static int read_trailer(wchar_t *version, int chars) {
    HANDLE f = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size, at;
    unsigned char t[TRAILER];
    DWORD got = 0;
    int ok = GetFileSizeEx(f, &size) && size.QuadPart > TRAILER;
    at.QuadPart = size.QuadPart - TRAILER;
    ok = ok && SetFilePointerEx(f, at, NULL, FILE_BEGIN) && ReadFile(f, t, TRAILER, &got, NULL) && got == TRAILER &&
         !memcmp(t, "RKSFX001", 8);
    CloseHandle(f);
    if (!ok) return 0;
    memcpy(&g_offset, t + 8, 8);
    memcpy(&g_size, t + 16, 8);
    if (g_offset + g_size + TRAILER != (unsigned long long)size.QuadPart) return 0;
    int n = 0;
    for (; n < 31 && n < chars - 1 && t[24 + n]; n++) {
        unsigned char c = t[24 + n];   /* a version is digits, letters, dots and dashes: nothing a path cannot hold */
        version[n] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ? c : L'_';
    }
    version[n] = 0;
    return n > 0;
}

static int make_dirs(wchar_t *path) {   /* every folder of path, which ends without a slash */
    for (wchar_t *p = path + 3; *p; p++)
        if (*p == L'\\') {
            *p = 0;
            CreateDirectoryW(path, NULL);
            *p = L'\\';
        }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int run(wchar_t *command, const wchar_t *cwd, int wait, DWORD flags) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, flags, NULL, cwd, &si, &pi)) return -1;
    DWORD code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* The zip out of this file into g_zip, then tar unpacks it into g_dir. 0, or the step that failed (1 copy, 2 tar). */
static DWORD WINAPI unpack(void *arg) {
    (void)arg;
    LONG result = 1;
    HANDLE in = CreateFileW(g_self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE out = CreateFileW(g_zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    enum { CHUNK = 1 << 20 };
    unsigned char *buf = HeapAlloc(GetProcessHeap(), 0, CHUNK);
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)g_offset;
    if (in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && buf && SetFilePointerEx(in, at, NULL, FILE_BEGIN)) {
        unsigned long long left = g_size;
        result = 0;
        while (left && !result) {
            DWORD want = left > CHUNK ? CHUNK : (DWORD)left, got = 0, put = 0;
            if (!ReadFile(in, buf, want, &got, NULL) || got != want || !WriteFile(out, buf, got, &put, NULL) || put != got) result = 1;
            left -= got;
        }
    }
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    if (!result) {
        wchar_t sys[MAX_PATH], command[PATH_CHARS * 3];
        GetSystemDirectoryW(sys, MAX_PATH);
        swprintf(command, PATH_CHARS * 3, L"\"%ls\\tar.exe\" -xf \"%ls\" -C \"%ls\"", sys, g_zip, g_dir);
        if (run(command, NULL, 1, CREATE_NO_WINDOW) != 0) result = 2;
    }
    DeleteFileW(g_zip);
    if (!result) {   /* whole: the mark the next start looks for */
        wchar_t mark[PATH_CHARS];
        swprintf(mark, PATH_CHARS, L"%ls\\unpacked.txt", g_dir);
        HANDLE m = CreateFileW(mark, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (m == INVALID_HANDLE_VALUE) result = 1;
        else CloseHandle(m);
    }
    InterlockedExchange(&g_result, result);
    if (g_window) PostMessageW(g_window, WM_CLOSE, 0, 0);
    return 0;
}

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE && g_result < 0) return 0;   /* not while the files are being written */
    if (m == WM_DESTROY) PostQuitMessage(0);
    if (m == WM_CTLCOLORSTATIC) {
        SetTextColor((HDC)wp, RGB(246, 239, 214));
        SetBkColor((HDC)wp, RGB(42, 29, 18));
        return (LRESULT)GetClassLongPtrW(w, GCLP_HBRBACKGROUND);
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* The small window shown while the folder is unpacked: dark wood, cream text, in the middle of the screen. */
static void waiting_window(HINSTANCE inst) {
    WNDCLASSW wc = {.lpfnWndProc = window_proc, .hInstance = inst, .lpszClassName = L"RekarinaSingle",
                    .hbrBackground = CreateSolidBrush(RGB(42, 29, 18)), .hCursor = LoadCursorW(NULL, IDC_WAIT),
                    .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1))};
    RegisterClassW(&wc);
    int w = 520, h = 150;
    g_window = CreateWindowExW(0, wc.lpszClassName, L"Rekarina", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                               (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                               NULL, NULL, inst, NULL);
    HWND text = CreateWindowExW(0, L"STATIC",
                                russian() ? L"Первый запуск: Rekarina распаковывает свои файлы.\nЭто занимает около минуты и делается один раз."
                                          : L"First start: Rekarina is unpacking its files.\nThis takes about a minute and happens once.",
                                WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 34, w - 56, 60, g_window, NULL, inst, NULL);
    SendMessageW(text, WM_SETFONT, (WPARAM)CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"), TRUE);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, wchar_t *args, int show) {
    (void)prev, (void)show;
    wchar_t version[40], base[PATH_CHARS], launcher[PATH_CHARS], mark[PATH_CHARS];
    int unpack_only = args && wcsstr(args, L"--unpack-only");
    if (!GetModuleFileNameW(NULL, g_self, PATH_CHARS) || !read_trailer(version, 40))
        fail(L"This copy of Rekarina.exe is incomplete. Download it again.",
             L"Эта копия Rekarina.exe неполная. Скачайте её заново.");
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, PATH_CHARS - 120))
        fail(L"Windows did not say where local application data goes (LOCALAPPDATA).",
             L"Windows не сообщила, где хранить данные приложений (LOCALAPPDATA).");
    swprintf(g_dir, PATH_CHARS, L"%ls\\Rekarina\\setup\\%ls", base, version);
    swprintf(g_zip, PATH_CHARS, L"%ls.zip", g_dir);
    swprintf(launcher, PATH_CHARS, L"%ls\\Rekarina-Launcher.exe", g_dir);
    swprintf(mark, PATH_CHARS, L"%ls\\unpacked.txt", g_dir);
    if (!exists(mark) || !exists(launcher)) {
        if (!make_dirs(g_dir))
            fail(L"Rekarina could not make its folder under %LOCALAPPDATA%\\Rekarina.",
                 L"Rekarina не смогла создать свою папку в %LOCALAPPDATA%\\Rekarina.");
        if (unpack_only) {   /* no window: nothing may come up on the screen of a test or a script */
            unpack(NULL);
        } else {
            waiting_window(inst);
            HANDLE thread = CreateThread(NULL, 0, unpack, NULL, 0, NULL);
            if (!thread) fail(L"Rekarina could not start unpacking.", L"Rekarina не смогла начать распаковку.");
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (g_result == 1)
            fail(L"Rekarina could not write its files. Check the free space on the system disk (about 1 GB is needed).",
                 L"Rekarina не смогла записать свои файлы. Проверьте свободное место на системном диске (нужно около 1 ГБ).");
        if (g_result)
            fail(L"Windows could not unpack Rekarina's files (tar.exe; Windows 10 version 1803 or newer is needed).",
                 L"Windows не смогла распаковать файлы Rekarina (tar.exe; нужна Windows 10 версии 1803 или новее).");
    }
    if (unpack_only) return 0;   /* a test, or a setup script: the folder alone */
    wchar_t command[PATH_CHARS + 4096];
    swprintf(command, PATH_CHARS + 4096, L"\"%ls\" %ls", launcher, args ? args : L"");
    if (run(command, g_dir, 0, 0) < 0)
        fail(L"Rekarina's launcher did not start.", L"Лаунчер Rekarina не запустился.");
    return 0;
}
