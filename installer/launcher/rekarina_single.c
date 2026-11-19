/* Rekarina as one file: Rekarina.exe carries the whole setup folder (tools/make_rekarina_release.py --single) and
   starts its launcher, unpacking the folder first where that has not been done.

   The file is this program, then a zip of the setup folder, then a trailer of 56 bytes: "RKSFX001", the zip's offset
   and size (two 64-bit little-endian numbers) and the version (32 bytes, zero padded). The folder goes to
   <Rekarina's folder>\setup\<version>, the folder being the player's choice at the first start (find_home); a file
   named unpacked.txt, written last, says it is whole. The zip is
   copied out of this file and unpacked by Windows' own tar.exe (bsdtar, part of Windows 10 since 1803), in a thread,
   while a small window says what is going on; then <folder>\Rekarina-Launcher.exe starts with this program's
   arguments (--unpack-only: unpack, start nothing). Updates are the launcher's own business from there
   (installer/update.py): they change that folder.

   Messages are in Russian on a Russian Windows and in English elsewhere, as the launcher exe's are.

   Build: llvm/bin/llvm-windres -I runtime -O coff installer/launcher/rekarina.rc -o rekarina_rc.o, then
   llvm/bin/clang -O2 -mwindows -municode installer/launcher/rekarina_single.c rekarina_rc.o -lole32 -luuid
   -lshell32 -o Rekarina.exe */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>
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

static unsigned long long free_gib(const wchar_t *path) {   /* free room on the disk of path, 0 when it cannot be asked */
    wchar_t root[4] = {path[0], L':', L'\\', 0};
    ULARGE_INTEGER avail;
    return path[1] == L':' && GetDiskFreeSpaceExW(root, &avail, NULL, NULL) ? avail.QuadPart >> 30 : 0;
}

/* ---- Rekarina's folder
   Everything of Rekarina lives under one folder the player chooses at the first start: <home>\setup\<version> (this
   program's unpacked files, with the working files of a build in its temp folder) and, as the launcher's first offer
   for the game, <home>\game. The choice is kept in %LOCALAPPDATA%\Rekarina\home.txt (the path, UTF-16), the one thing
   written to the system disk when another disk is chosen. No question is asked when that file names a folder, when
   an earlier version is unpacked in the local application data (that install stays where it is: the folder is then
   %LOCALAPPDATA%\Rekarina), with --home <folder> (a script's choice) or with --unpack-only (no window may come up). */
#define HOME_ROOM_GIB 25   /* the setup 1, a build's working files 9, the game 14 (installer/oot.py NEED_TEMP, NEED_DEST) */

/* A folder as a path ends: no slash, no space and no dot at its end (Windows drops them from a name, so the folder made
   would not be the one the text names), a drive's root as "D:". */
static void tidy_home(wchar_t *p) {
    size_t n = wcslen(p);
    while (n > 2 && (p[n - 1] == L'\\' || p[n - 1] == L'/' || p[n - 1] == L' ' ||
                     (p[n - 1] == L'.' && p[n - 2] != L'\\' && p[n - 2] != L'.' && p[n - 2] != L':')))
        p[--n] = 0;
}

static int read_home(const wchar_t *note, wchar_t *home) {
    HANDLE f = CreateFileW(note, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    int ok = ReadFile(f, home, (PATH_CHARS - 80) * sizeof(wchar_t), &got, NULL) && got >= 3 * sizeof(wchar_t);
    CloseHandle(f);
    if (!ok) return 0;
    home[got / sizeof(wchar_t)] = 0;
    for (wchar_t *p = home; *p; p++)
        if (*p == L'\r' || *p == L'\n') *p = 0;
    if (home[0] == 0xFEFF) memmove(home, home + 1, (wcslen(home + 1) + 1) * sizeof(wchar_t));   /* an editor's byte order mark */
    tidy_home(home);
    return home[1] == L':' && exists(home);   /* a disk that is gone: no folder, the player is asked again */
}

static void write_home(const wchar_t *note, const wchar_t *home) {
    HANDLE f = CreateFileW(note, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put;
    WriteFile(f, home, (DWORD)(wcslen(home) * sizeof(wchar_t)), &put, NULL);
    CloseHandle(f);
}

/* Windows' own folder dialog, opened at `start`. 1 with the folder in out, 0 when the player closed it. */
static int pick_folder(const wchar_t *start, wchar_t *out) {
    IFileOpenDialog *d = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return 0;
    DWORD options = 0;
    IFileOpenDialog_GetOptions(d, &options);
    IFileOpenDialog_SetOptions(d, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(d, russian() ? L"Rekarina: папка для лаунчера и игры (нужно около 25 ГБ)"
                                          : L"Rekarina: a folder for the launcher and the game (about 25 GB)");
    IFileOpenDialog_SetOkButtonLabel(d, russian() ? L"Поставить сюда" : L"Install here");
    IShellItem *at = NULL;
    if (start && SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&at))) {
        IFileOpenDialog_SetFolder(d, at);
        IShellItem_Release(at);
    }
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(d, NULL)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &item))) {
        wchar_t *path = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
            ok = wcslen(path) < PATH_CHARS - 120 && path[1] == L':';
            if (ok) wcscpy(out, path);
            CoTaskMemFree(path);
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(d);
    return ok;
}

static int folder_is_empty(const wchar_t *folder) {
    wchar_t all[PATH_CHARS];
    WIN32_FIND_DATAW fd;
    swprintf(all, PATH_CHARS, L"%ls\\*", folder);
    HANDLE h = FindFirstFileW(all, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int empty = 1;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) empty = 0;
    } while (empty && FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

static int names_rekarina(const wchar_t *folder) {   /* the folder's own name has "rekarina" in it, in any case */
    const wchar_t *leaf = wcsrchr(folder, L'\\');
    wchar_t low[64];
    int n = 0;
    for (leaf = leaf ? leaf + 1 : folder; *leaf && n < 63; leaf++) low[n++] = (wchar_t)towlower(*leaf);
    low[n] = 0;
    return wcsstr(low, L"rekarina") != NULL;
}

/* home: Rekarina's folder, without a slash at its end. *asked: the player has just chosen it (the launcher is then
   given <home>\game as the game folder to offer). Ends the program when the player closes the question. */
static void find_home(const wchar_t *appdata, const wchar_t *args, int quiet, wchar_t *home, int *asked) {
    wchar_t note[PATH_CHARS], old[PATH_CHARS], here[PATH_CHARS];
    *asked = 0;
    swprintf(home, PATH_CHARS, L"%ls\\Rekarina", appdata);
    swprintf(note, PATH_CHARS, L"%ls\\home.txt", home);
    swprintf(old, PATH_CHARS, L"%ls\\setup", home);
    const wchar_t *given = args ? wcsstr(args, L"--home ") : NULL;
    if (given) {   /* --home <folder>, the folder in quotes when it has spaces */
        given += 7;
        while (*given == L' ') given++;
        wchar_t stop = *given == L'"' ? L'"' : L' ';
        if (*given == L'"') given++;
        int n = 0;
        while (given[n] && given[n] != stop && n < PATH_CHARS - 120) n++;
        if (n > 2 && n < PATH_CHARS - 120 && given[1] == L':') {   /* a value too long for the buffers is not taken cut */
            wcsncpy(home, given, n);
            home[n] = 0;
            tidy_home(home);
        }
        return;
    }
    int again = !quiet && args && wcsstr(args, L"--choose") != NULL;   /* --choose: ask even where a folder is known */
    if (!again && read_home(note, here)) {
        wcscpy(home, here);
        return;
    }
    if (!again && (exists(old) || quiet)) return;   /* an install made before the choice existed, or no window allowed */
    wcscpy(here, g_self);
    wchar_t *cut = wcsrchr(here, L'\\');
    if (cut) *cut = 0;
    for (;;) {
        wchar_t picked[PATH_CHARS];
        if (!pick_folder(here, picked)) ExitProcess(0);
        size_t n = wcslen(picked);
        if (n > 3 && picked[n - 1] == L'\\') picked[n - 1] = 0;
        /* a folder that holds other things gets a folder of Rekarina's own inside it */
        if (wcslen(picked) <= 3 || (!folder_is_empty(picked) && !names_rekarina(picked))) {
            if (picked[wcslen(picked) - 1] == L'\\') picked[wcslen(picked) - 1] = 0;
            wcscat(picked, L"\\Rekarina");
        }
        unsigned long long room = free_gib(picked);
        if (room < HOME_ROOM_GIB) {
            static wchar_t text[PATH_CHARS + 500];
            swprintf(text, PATH_CHARS + 500, russian()
                     ? L"На диске %lc: свободно %llu ГБ, а Rekarina вместе с игрой нужно около %d ГБ.\n\nВыбрать другую папку?"
                     : L"Disk %lc: has %llu GB free, and Rekarina with the game needs about %d GB.\n\nChoose another folder?",
                     (wchar_t)towupper(picked[0]), room, HOME_ROOM_GIB);
            if (MessageBoxW(NULL, text, L"Rekarina", MB_YESNO | MB_ICONWARNING) == IDYES) continue;
        }
        wcscpy(home, picked);
        break;
    }
    wchar_t folder[PATH_CHARS];
    swprintf(folder, PATH_CHARS, L"%ls\\Rekarina", appdata);
    make_dirs(folder);
    write_home(note, home);
    *asked = 1;
}

/* The arguments the launcher gets: this program's own (--choose, --home <folder>) taken out of a copy of args. */
static void launcher_args(const wchar_t *args, wchar_t *out, size_t cap) {
    out[0] = 0;
    if (!args) return;
    wcsncpy(out, args, cap - 1);
    out[cap - 1] = 0;
    wchar_t *p = wcsstr(out, L"--choose");
    if (p) memmove(p, p + 8, (wcslen(p + 8) + 1) * sizeof(wchar_t));
    p = wcsstr(out, L"--home ");
    if (p) {
        wchar_t *q = p + 7;
        wchar_t stop = *q == L'"' ? L'"' : L' ';
        if (*q == L'"') q++;
        while (*q && *q != stop) q++;
        if (*q == L'"') q++;
        memmove(p, q, (wcslen(q) + 1) * sizeof(wchar_t));
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, wchar_t *args, int show) {
    (void)prev, (void)show;
    wchar_t version[40], base[PATH_CHARS], home[PATH_CHARS], launcher[PATH_CHARS], mark[PATH_CHARS];
    int asked = 0;
    int unpack_only = args && wcsstr(args, L"--unpack-only");
    if (!GetModuleFileNameW(NULL, g_self, PATH_CHARS) || !read_trailer(version, 40))
        fail(L"This copy of Rekarina.exe is incomplete. Download it again.",
             L"Эта копия Rekarina.exe неполная. Скачайте её заново.");
    DWORD base_chars = GetEnvironmentVariableW(L"LOCALAPPDATA", base, PATH_CHARS - 120);
    if (base_chars == 0 || base_chars >= PATH_CHARS - 120)   /* unset, or too long for the buffer (the call then writes nothing) */
        fail(L"Windows did not say where local application data goes (LOCALAPPDATA).",
             L"Windows не сообщила, где хранить данные приложений (LOCALAPPDATA).");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* the folder dialog of find_home */
    find_home(base, args, unpack_only, home, &asked);
    swprintf(g_dir, PATH_CHARS, L"%ls\\setup\\%ls", home, version);
    swprintf(g_zip, PATH_CHARS, L"%ls.zip", g_dir);
    swprintf(launcher, PATH_CHARS, L"%ls\\Rekarina-Launcher.exe", g_dir);
    swprintf(mark, PATH_CHARS, L"%ls\\unpacked.txt", g_dir);
    if (!exists(mark) || !exists(launcher)) {
        if (!make_dirs(g_dir))
            fail(L"Rekarina could not make the folder for its files.", L"Rekarina не смогла создать папку для своих файлов.");
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
            fail(L"Rekarina could not write its files. Check the free space on the disk they go to (about 1 GB is needed).",
                 L"Rekarina не смогла записать свои файлы. Проверьте свободное место на диске, куда они пишутся (нужно около 1 ГБ).");
        if (g_result)
            fail(L"Windows could not unpack Rekarina's files (tar.exe; Windows 10 version 1803 or newer is needed).",
                 L"Windows не смогла распаковать файлы Rekarina (tar.exe; нужна Windows 10 версии 1803 или новее).");
    }
    if (unpack_only) return 0;   /* a test, or a setup script: the folder alone */
    wchar_t command[PATH_CHARS + 4096];
    /* the game folder the launcher offers after the choice: beside the setup, under the same folder */
    static wchar_t rest[4096];
    launcher_args(args, rest, 4096);
    if (asked) swprintf(command, PATH_CHARS + 4096, L"\"%ls\" --dest \"%ls\\game\" %ls", launcher, home, rest);
    else swprintf(command, PATH_CHARS + 4096, L"\"%ls\" %ls", launcher, rest);
    if (run(command, g_dir, 0, 0) < 0)
        fail(L"Rekarina's launcher did not start.", L"Лаунчер Rekarina не запустился.");
    return 0;
}
