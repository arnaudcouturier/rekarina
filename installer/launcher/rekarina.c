/* Rekarina's launcher exe: starts the launcher and setup window (python -m installer.gui_oot) without a console,
   after swapping in an update the window downloaded.

   Two layouts: a release folder (a python folder with pythonw.exe beside this exe, the installer package beside it
   too) and a source checkout (this exe in the folder that holds src and .venv). The window's own text is in
   installer/text; the messages here are shown when no Python is found or an update could not be swapped in, in
   Russian on a Russian Windows and in English elsewhere.

   The update (installer/update.py has the whole story): the window unpacks a newer release into
   update-staging\new beside this exe and writes update-staging\ready.txt last. This exe runs before any Python of
   the folder does, so it is the one place where the folder's files are free to be moved: apply_update moves each
   entry the new release brings into update-old-<n> and the staged entry into its place, all renames on one disk. A
   failed move (another program holds a file of the old release open) moves everything back and the old version
   starts; ready.txt is renamed to failed.txt so a start is never held up twice by the same failure.
   REKARINA_LAUNCHER_QUIET=1 keeps the message boxes away (tests).

   Build: llvm/bin/llvm-windres -I runtime -O coff installer/launcher/rekarina.rc -o rekarina_rc.o, then
   llvm/bin/clang -O2 -mwindows -municode installer/launcher/rekarina.c rekarina_rc.o -o Rekarina-Launcher.exe */
#include <windows.h>
#include <wchar.h>

#define PATH_CHARS (MAX_PATH * 2)
#define MAX_ENTRIES 64
#define NAME_CHARS 128
#define MOVE_WAIT_MS 15000   /* how long the first move waits for the window that started us to let go of its files */

static int exists(const wchar_t *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static int quiet(void) { return GetEnvironmentVariableW(L"REKARINA_LAUNCHER_QUIET", NULL, 0) > 0; }

static void message(const wchar_t *english, const wchar_t *russian, UINT icon)
{
    int ru = (GetUserDefaultUILanguage() & 0x3ff) == 0x19;
    if (!quiet()) MessageBoxW(NULL, ru ? russian : english, L"Rekarina", MB_OK | icon);
}

/* A folder and everything in it; what is in use stays. */
static void remove_tree(const wchar_t *path)
{
    wchar_t pattern[PATH_CHARS], child[PATH_CHARS];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (swprintf(pattern, PATH_CHARS, L"%ls\\*", path) < 0) return;
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (swprintf(child, PATH_CHARS, L"%ls\\%ls", path, fd.cFileName) < 0) continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree(child);
            else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child); else DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

/* Rename from to to, trying again for wait_ms while another process holds a file of it. */
static int move_wait(const wchar_t *from, const wchar_t *to, DWORD wait_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        DWORD e;
        if (MoveFileExW(from, to, 0)) return 1;
        e = GetLastError();
        if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) ||
            GetTickCount() - start >= wait_ms)
            return 0;
        Sleep(250);
    }
}

/* Swap a staged update in (the file's head). 0: none staged; 1: swapped; -1: failed and undone, the old release is
   whole; -2: failed and the undo failed too. */
static int apply_update(const wchar_t *root)
{
    static wchar_t names[MAX_ENTRIES][NAME_CHARS];
    static char moved_old[MAX_ENTRIES], moved_new[MAX_ENTRIES];
    wchar_t staging[PATH_CHARS], ready[PATH_CHARS], failed[PATH_CHARS], fresh[PATH_CHARS], old[PATH_CHARS];
    wchar_t pattern[PATH_CHARS], a[PATH_CHARS], b[PATH_CHARS];
    WIN32_FIND_DATAW fd;
    HANDLE h, mutex;
    int n = 0, i, result = 1;
    DWORD wait = MOVE_WAIT_MS;

    swprintf(staging, PATH_CHARS, L"%ls\\update-staging", root);
    swprintf(ready, PATH_CHARS, L"%ls\\ready.txt", staging);
    swprintf(failed, PATH_CHARS, L"%ls\\failed.txt", staging);
    swprintf(fresh, PATH_CHARS, L"%ls\\new", staging);
    if (!exists(ready)) return 0;
    if (!exists(fresh)) { DeleteFileW(ready); return 0; }

    /* one launcher at a time: a double click starts two */
    mutex = CreateMutexW(NULL, FALSE, L"Local\\RekarinaForkLauncherUpdate");
    if (mutex && WaitForSingleObject(mutex, 60000) == WAIT_TIMEOUT) { CloseHandle(mutex); return 0; }
    if (!exists(ready)) { result = 0; goto done; }   /* the other launcher did it while this one waited */

    swprintf(old, PATH_CHARS, L"%ls\\update-old-%08lx", root, (unsigned long)GetTickCount());
    if (!CreateDirectoryW(old, NULL)) { result = -1; goto done; }

    /* What the new release brings. The Python folder goes first: it is the one a running window holds, so a swap
       that cannot happen fails before anything moved. This exe goes with the rest: a running exe may be renamed. */
    swprintf(pattern, PATH_CHARS, L"%ls\\*", fresh);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { result = -1; goto done; }
    do {
        const wchar_t *name = fd.cFileName;
        if (!wcscmp(name, L".") || !wcscmp(name, L"..") || !_wcsicmp(name, L"temp") || !_wcsicmp(name, L"update-staging") ||
            !_wcsnicmp(name, L"update-old", 10))
            continue;
        if (n == MAX_ENTRIES || wcslen(name) >= NAME_CHARS) { FindClose(h); result = -1; goto done; }
        if (!_wcsicmp(name, L"python") && n > 0) {
            wcscpy(names[n], names[0]);
            wcscpy(names[0], name);
        } else
            wcscpy(names[n], name);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (i = 0; i < n && result == 1; i++) {
        swprintf(a, PATH_CHARS, L"%ls\\%ls", root, names[i]);
        swprintf(b, PATH_CHARS, L"%ls\\%ls", old, names[i]);
        if (exists(a)) {
            if (!move_wait(a, b, wait)) { result = -1; break; }
            moved_old[i] = 1;
        }
        wait = 2000;   /* the first move waited for the window to close; a later failure is not that */
        swprintf(b, PATH_CHARS, L"%ls\\%ls", fresh, names[i]);
        if (!move_wait(b, a, wait)) { result = -1; break; }
        moved_new[i] = 1;
    }
    if (result != 1) {   /* undo, last move first */
        for (i = n - 1; i >= 0; i--) {
            swprintf(a, PATH_CHARS, L"%ls\\%ls", root, names[i]);
            if (moved_new[i]) {
                swprintf(b, PATH_CHARS, L"%ls\\%ls", fresh, names[i]);
                if (!move_wait(a, b, 2000)) result = -2;
            }
            if (moved_old[i]) {
                swprintf(b, PATH_CHARS, L"%ls\\%ls", old, names[i]);
                if (!move_wait(b, a, 2000)) result = -2;
            }
        }
        if (result == -1) RemoveDirectoryW(old);
        DeleteFileW(failed);
        MoveFileExW(ready, failed, 0);
    } else {
        DeleteFileW(ready);
        remove_tree(staging);
    }
done:
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return result;
}

static int start(const wchar_t *exe, const wchar_t *command_line, const wchar_t *cwd)
{
    static wchar_t command[PATH_CHARS * 2 + 64];
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (wcslen(command_line) >= PATH_CHARS * 2 + 64) return 0;
    wcscpy(command, command_line);   /* CreateProcessW may write into it */
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show)
{
    static wchar_t folder[PATH_CHARS], python[PATH_CHARS], cwd[PATH_CHARS], command[PATH_CHARS * 2 + 64];
    const wchar_t *arguments = command_line ? command_line : L"";
    DWORD n = GetModuleFileNameW(NULL, folder, PATH_CHARS);
    int ok = 0;
    (void)instance; (void)previous; (void)show;
    if (n > 0 && n < PATH_CHARS) {
        while (n > 0 && folder[n - 1] != L'\\') n--;
        folder[n > 0 ? n - 1 : 0] = 0;
        switch (apply_update(folder)) {
        case 1:   /* the new launcher takes over; this old exe now lies in update-old-<n> and ends */
            swprintf(python, PATH_CHARS, L"%ls\\Rekarina-Launcher.exe", folder);
            swprintf(command, PATH_CHARS * 2 + 64, L"\"%ls\" --after-update", python);
            if (start(python, command, folder)) return 0;
            arguments = L"--after-update";
            break;
        case -1:
            message(L"The update could not be installed: files of the launcher are in use. Close every Rekarina window and "
                    L"press Update now again. The current version starts now.",
                    L"\x041e\x0431\x043d\x043e\x0432\x043b\x0435\x043d\x0438\x0435 \x043d\x0435 "
                    L"\x0443\x0441\x0442\x0430\x043d\x043e\x0432\x043b\x0435\x043d\x043e: \x0444\x0430\x0439\x043b\x044b "
                    L"\x043b\x0430\x0443\x043d\x0447\x0435\x0440\x0430 \x0437\x0430\x043d\x044f\x0442\x044b. "
                    L"\x0417\x0430\x043a\x0440\x043e\x0439\x0442\x0435 \x0432\x0441\x0435 \x043e\x043a\x043d\x0430 Rekarina \x0438 "
                    L"\x0432\x044b\x0431\x0435\x0440\x0438\x0442\x0435 \x043e\x0431\x043d\x043e\x0432\x043b\x0435\x043d\x0438\x0435 "
                    L"\x0441\x043d\x043e\x0432\x0430. \x0421\x0435\x0439\x0447\x0430\x0441 "
                    L"\x0437\x0430\x043f\x0443\x0441\x0442\x0438\x0442\x0441\x044f \x0442\x0435\x043a\x0443\x0449\x0430\x044f "
                    L"\x0432\x0435\x0440\x0441\x0438\x044f.", MB_ICONWARNING);
            break;
        case -2:
            message(L"The update stopped halfway and the launcher's folder is incomplete. Unpack the Rekarina archive over "
                    L"this folder again. Your game folder, saves and settings are not affected.",
                    L"\x041e\x0431\x043d\x043e\x0432\x043b\x0435\x043d\x0438\x0435 \x043f\x0440\x0435\x0440\x0432\x0430\x043b\x043e\x0441\x044c "
                    L"\x043d\x0430 \x043f\x043e\x043b\x043f\x0443\x0442\x0438, \x043f\x0430\x043f\x043a\x0430 "
                    L"\x043b\x0430\x0443\x043d\x0447\x0435\x0440\x0430 \x043d\x0435\x043f\x043e\x043b\x043d\x0430. "
                    L"\x0420\x0430\x0441\x043f\x0430\x043a\x0443\x0439\x0442\x0435 \x0430\x0440\x0445\x0438\x0432 Rekarina "
                    L"\x043f\x043e\x0432\x0435\x0440\x0445 \x044d\x0442\x043e\x0439 \x043f\x0430\x043f\x043a\x0438 \x0435\x0449\x0451 "
                    L"\x0440\x0430\x0437. \x041f\x0430\x043f\x043a\x0430 \x0441 \x0438\x0433\x0440\x043e\x0439, "
                    L"\x0441\x043e\x0445\x0440\x0430\x043d\x0435\x043d\x0438\x044f \x0438 "
                    L"\x043d\x0430\x0441\x0442\x0440\x043e\x0439\x043a\x0438 \x043d\x0435 "
                    L"\x0437\x0430\x0442\x0440\x043e\x043d\x0443\x0442\x044b.", MB_ICONERROR);
            return 1;
        }
        swprintf(python, PATH_CHARS, L"%ls\\python\\pythonw.exe", folder);
        swprintf(cwd, PATH_CHARS, L"%ls", folder);
        if (!exists(python)) {   /* a source checkout */
            swprintf(python, PATH_CHARS, L"%ls\\.venv\\Scripts\\pythonw.exe", folder);
            swprintf(cwd, PATH_CHARS, L"%ls\\src", folder);
        }
        if (exists(python) && exists(cwd)) {
            swprintf(command, PATH_CHARS * 2 + 64, L"\"%ls\" -m installer.gui_oot %ls", python, arguments);
            ok = start(python, command, cwd);
        }
    }
    if (!ok) {
        message(L"Python was not found beside the launcher. Unpack the whole archive again and start the launcher "
                L"from the unpacked folder.",
                L"\x041d\x0435 \x043d\x0430\x0439\x0434\x0435\x043d Python \x0440\x044f\x0434\x043e\x043c \x0441 "
                L"\x043b\x0430\x0443\x043d\x0447\x0435\x0440\x043e\x043c. \x0420\x0430\x0441\x043f\x0430\x043a\x0443\x0439\x0442\x0435 "
                L"\x0430\x0440\x0445\x0438\x0432 \x0446\x0435\x043b\x0438\x043a\x043e\x043c \x0438 "
                L"\x0437\x0430\x043f\x0443\x0441\x0442\x0438\x0442\x0435 \x043b\x0430\x0443\x043d\x0447\x0435\x0440 \x0438\x0437 "
                L"\x0440\x0430\x0441\x043f\x0430\x043a\x043e\x0432\x0430\x043d\x043d\x043e\x0439 \x043f\x0430\x043f\x043a\x0438.",
                MB_ICONERROR);
        return 1;
    }
    return 0;
}
