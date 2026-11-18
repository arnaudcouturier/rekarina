// The update notice's request on Windows: GET https://rekarina.dev/releases/latest.json (update_note.h) through
// WinHTTP, loaded at run time so a system without it only loses the notice. Timeouts of five seconds each; the answer
// is read up to 64 KiB (the release's notes may be long, tag_name comes first or early).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winhttp.h>

#include "host.h"
#include "update_note.h"

typedef HINTERNET (WINAPI *OpenFn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *ConnectFn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *OpenRequestFn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *SetTimeoutsFn)(HINTERNET, int, int, int, int);
typedef BOOL (WINAPI *SendFn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *ReceiveFn)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *QueryHeadersFn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *ReadFn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *CloseFn)(HINTERNET);

int update_http_latest(char *out, size_t cap) {
    HMODULE lib = LoadLibraryW(L"winhttp.dll");
    if (!lib) return 0;
#define FN(type, name) type name = (type)(void *)GetProcAddress(lib, "WinHttp" #name)
    FN(OpenFn, Open);
    FN(ConnectFn, Connect);
    FN(OpenRequestFn, OpenRequest);
    FN(SetTimeoutsFn, SetTimeouts);
    FN(SendFn, SendRequest);
    FN(ReceiveFn, ReceiveResponse);
    FN(QueryHeadersFn, QueryHeaders);
    FN(ReadFn, ReadData);
    CloseFn Close = (CloseFn)(void *)GetProcAddress(lib, "WinHttpCloseHandle");
#undef FN
    int ok = 0;
    HINTERNET session = NULL, conn = NULL, req = NULL;
    char *body = NULL;
    if (!Open || !Connect || !OpenRequest || !SetTimeouts || !SendRequest || !ReceiveResponse || !QueryHeaders ||
        !ReadData || !Close)
        goto done;
    wchar_t agent[64];
    {
        char a[64];
        snprintf(a, sizeof a, RT_PORT_NAME "/%.*s", (int)strcspn(rt_port_version, "+"), rt_port_version);
        MultiByteToWideChar(CP_UTF8, 0, a, -1, agent, 64);
    }
    session = Open(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto done;
    SetTimeouts(session, 5000, 5000, 5000, 5000);
    conn = Connect(session, UPDATE_REPO_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) goto done;
    static const wchar_t *accept[] = {L"application/vnd.github+json", NULL};
    req = OpenRequest(conn, L"GET", UPDATE_REPO_PATH, NULL, WINHTTP_NO_REFERER, accept, WINHTTP_FLAG_SECURE);
    if (!req || !SendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !ReceiveResponse(req, NULL))
        goto done;
    DWORD status = 0, len = sizeof status;
    if (!QueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                      &len, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        rt_log("update: the release feed answered %lu", (unsigned long)status);
        goto done;
    }
    enum { CAP = 64 * 1024 };
    body = malloc(CAP + 1);
    if (!body) goto done;
    DWORD total = 0, got = 0;
    while (total < CAP && ReadData(req, body + total, CAP - total, &got) && got) total += got;
    body[total] = 0;
    ok = update_tag_from_release(body, out, cap);
done:
    free(body);
    if (req) Close(req);
    if (conn) Close(conn);
    if (session) Close(session);
    FreeLibrary(lib);
    return ok;
}
