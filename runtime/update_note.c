// The update notice: whether a newer release of the port exists, for the line the overlay shows when the game starts
// (ui/menu.cpp update_notice_draw) and the menu's title line.
//
// The answer is kept in <app data>/Rekarina/update.json, which the launcher (installer/update.py) writes too:
//   {"latest": "1.0.1", "checked": 1731326400}
// latest: the newest release's version without its "v"; checked: Unix seconds of the check. Other members are left to
// the launcher and not read here. A file younger than CHECK_EVERY_S is trusted; else one HTTPS request asks GitHub for
// the newest release (update_http_latest, update_note_win.c) and the file is written again. The request carries no
// more than the port's name and version as its User-Agent. UPDATE_CHECK=0 (menu: Interface) stops both the request and
// the notice. All of it runs on a thread of its own at idle priority: a slow or absent network costs the game nothing.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "update_note.h"

#define CHECK_EVERY_S (6 * 3600)

static _Atomic int newer;       // 1: `latest` names a release newer than this build
static char latest[32];         // written before `newer` is set, never after

// "1.0.1", "v1.0.1" or "1.0.1+abc" as three numbers; 0 when the text is not a version.
static int version_parse(const char *s, int v[3]) {
    if (*s == 'v' || *s == 'V') s++;
    v[0] = v[1] = v[2] = 0;
    for (int i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') return 0;
        char *end;
        long n = strtol(s, &end, 10);
        if (n < 0 || n > 100000) return 0;
        v[i] = (int)n;
        s = end;
        if (*s != '.') break;
        s++;
    }
    return 1;
}

int update_version_newer(const char *candidate, const char *current) {
    int a[3], b[3];
    if (!version_parse(candidate, a) || !version_parse(current, b)) return 0;
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

// The string value of "key" in a flat JSON object, without escapes (versions have none); 0 when absent.
static int json_string(const char *json, const char *key, char *out, size_t cap) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != '"') return 0;
    size_t n = strcspn(p, "\"\\");
    if (p[n] != '"' || n + 1 > cap) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static long long json_number(const char *json, const char *key) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return 0;
    return atoll(p);
}

int update_tag_from_release(const char *json, char *out, size_t cap) {
    char tag[48];
    if (!json_string(json, "tag_name", tag, sizeof tag)) return 0;
    const char *t = tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag;
    int v[3];
    if (!version_parse(t, v) || strlen(t) + 1 > cap) return 0;
    strcpy(out, t);
    return 1;
}

static int cache_path(char *out, size_t cap) {
    char base[512];
#ifdef _WIN32
    if (!plat_getenv("APPDATA", base, sizeof base)) return 0;
    snprintf(out, cap, "%s/Rekarina", base);
#else
    if (plat_getenv("XDG_CONFIG_HOME", base, sizeof base)) snprintf(out, cap, "%s/rekarina", base);
    else if (plat_getenv("HOME", base, sizeof base)) snprintf(out, cap, "%s/.config/rekarina", base);
    else return 0;
#endif
    plat_file_mkdir_p(out);
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "/update.json");
    return 1;
}

static void decide(const char *version, const char *from) {
    if (!update_version_newer(version, rt_port_version)) {
        rt_log("update: this is %s, the newest release is %s (%s)", rt_port_version, version, from);
        return;
    }
    snprintf(latest, sizeof latest, "%s", version);
    atomic_store(&newer, 1);
    rt_log("update: release %s is newer than this build (%s; %s)", version, rt_port_version, from);
}

static void check_thread(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_IDLE);
    char path[640], version[32];
    if (!cache_path(path, sizeof path)) return;
    long long now = (long long)time(NULL);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (text) {   // plat_file_read ends the bytes with a 0
        long long checked = json_number(text, "checked");
        int have = json_string(text, "latest", version, sizeof version);
        free(text);
        if (have && checked > 0 && now - checked < CHECK_EVERY_S && now >= checked) {
            decide(version, "the launcher's or an earlier check");
            return;
        }
    }
    if (!update_http_latest(version, sizeof version)) {
        rt_log("update: no answer about the newest release (offline, or the request was refused)");
        return;
    }
    char json[160];
    int n = snprintf(json, sizeof json, "{\"latest\": \"%s\", \"checked\": %lld}\n", version, now);
    if (!plat_file_write_atomic(path, json, (size_t)n)) rt_log("update: %s could not be written", path);
    decide(version, "asked now");
}

void update_note_start(void) {
    if (!cfg_bool("TOTK_UPDATE_CHECK")) {
        rt_log("update: the check is off (UPDATE_CHECK=0)");
        return;
    }
    if (!plat_thread_spawn(check_thread, NULL, "update check", 256 * 1024)) rt_log("update: the check thread did not start");
}

int update_note_latest(char *out, size_t cap) {
    if (!atomic_load(&newer) || !cfg_bool("TOTK_UPDATE_CHECK")) return 0;
    if (out && cap) snprintf(out, cap, "%s", latest);
    return 1;
}

#ifndef _WIN32
int update_http_latest(char *out, size_t cap) {   // other systems: the launcher's file is the only source
    (void)out, (void)cap;
    return 0;
}
#endif
